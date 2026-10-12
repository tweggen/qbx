#include "app/objects/cut/ssplitclipaction.h"
#include "app/model/sobjectpath.h"
#include "app/model/splacements.h"
#include "app/objects/cut/sunsplitclipaction.h"
#include "app/model/sproject.h"
#include "app/actions/sactionregistry.h"
#include "app/model/slink.h"
#include "app/model/sclipwindow.h"
#include "app/objects/cut/stakestack.h"
#include "app/objects/cut/stakehelpers.h"
#include "app/model/seditgroups.h"
#include "app/actions/scompositeaction.h"
#include "tw/core/twfraction.h"
#include "tw/core/twlog.h"
#include <QDomElement>

using namespace strackpath;

SSplitClipAction::SSplitClipAction(const QList<int> &clipPath, offset_t splitTime,
                                   bool broadcast)
    : clipPath_(clipPath), splitTime_(splitTime), broadcast_(broadcast)
{
}

SApplyResult SSplitClipAction::apply(SProject *project)
{
    if (!project || clipPath_.isEmpty()) {
        return {false, nullptr};
    }
    // Edit-group broadcast: split every member's corresponding clip too.
    if (broadcast_) {
        QList<QList<int>> targets =
            seditgroups::expandClipPaths(project, clipPath_);
        if (targets.size() > 1) {
            SCompositeAction composite;
            for (const QList<int> &p : targets)
                composite.append(new SSplitClipAction(p, splitTime_, false));
            return composite.apply(project);
        }
    }
    SObject *mixer = splacements::rootNamed( project, pathRoot_ );
    if (!mixer) {
        return {false, nullptr};
    }
    QList<int> trackPath = clipPath_;
    int idx = trackPath.takeLast();
    SObject *track = splacements::laneAt( mixer, trackPath);
    if (!track) {
        return {false, nullptr};
    }
    SLink *link = track->childAt(idx);
    if (!link || (link->getSObject().isLane())) {
        return {false, nullptr};
    }

    offset_t startTime = link->getStartTime();
    offset_t inObjOffset = splitTime_ - startTime;
    SObject &obj0 = link->getSObject();
    // Blocking read (P19): split geometry must never come from the stale
    // try-lock fallback (edit path, bounded block).
    length_t fullDur = obj0.getDurationBlocking();
    if (inObjOffset <= 1 || inObjOffset >= (offset_t)fullDur - 1) {
        return {false, nullptr};        // split point outside the clip
    }

    // A TAKE STACK SPLITS INTO TWO STACKS, PER TAKE EXTENT (plan 53 §4.1).
    //
    // Before invariant 1′ every take spanned the whole column, so the split
    // was one rule: clone every take into the tail and trim every take in the
    // head. Under 1′ a take covers only its own span, so MEMBERSHIP is the
    // first question and there are three answers:
    //
    //   * a take WHOLLY IN THE HEAD belongs to the head only, untouched;
    //   * a take WHOLLY IN THE TAIL belongs to the tail only, and is MOVED
    //     there -- the same object, its tStart shifted by -inObjOffset
    //     (plan 52's identity rule: an undo must give back the object the
    //     edit moved, not a rebuild of it);
    //   * a take CROSSING the split is windowed into BOTH: trimmed in the head
    //     and CLONED into the tail, which is what this code always did.
    //
    // Take indices are then renumbered PER HALF and each half's comp map is
    // RE-INDEXED onto its own numbering -- B3's "head keeps < split, tail
    // shifted" becomes "... and re-indexed", because a segment's take number
    // means a different take once a take has gone missing from that half.
    //
    // FOR A FULL-EXTENT COLUMN EVERY TAKE CROSSES, so this reduces exactly to
    // the old clone-and-trim with identity index maps and unremapped maps,
    // which is why no existing split gate moves.
    if (STakeStack *stack = dynamic_cast<STakeStack*>(&obj0)) {
        const int nT = stack->nTakes();
        const length_t colDur = fullDur;          // the COLUMN's own length

        enum Where { HeadOnly, TailOnly, Crossing };
        struct Rec {
            offset_t s = 0;
            length_t len = 0;
            Where    where = Crossing;
            Fraction tailAnchor;     // crossing takes only
            Fraction stretch;
        };
        QList<Rec> recs;
        int nHead = 0, nTail = 0;
        for (int i = 0; i < nT; ++i) {
            SClipWindow *w = stack->takeAt(i);
            Rec r;
            r.s   = stack->takeExtentStart(i);
            r.len = stack->takeExtentLength(i);
            const offset_t e = r.s + (offset_t)r.len;
            r.where = (e <= inObjOffset)   ? HeadOnly
                    : (r.s >= inObjOffset) ? TailOnly
                                           : Crossing;
            if (w) {
                r.stretch = w->stretchOrRate();
                if (r.where == Crossing) {
                    // The content position the split maps to, computed from the
                    // take's OWN window while it is still intact: exact
                    // rational, no floor (proposal 18 Phase 3; W1 makes it
                    // piecewise through the warp map, identical to the old
                    // /stretch when there are no anchors). The split offset is
                    // relative to the TAKE, which begins at its tStart.
                    r.tailAnchor = w->timelineToSourceExact(
                        Fraction( (int64_t)( inObjOffset - r.s ) ) );
                }
            }
            if (r.where != TailOnly) ++nHead;
            if (r.where != HeadOnly) ++nTail;
            recs.append(r);
        }
        // A HALF WITH NO TAKES IS REFUSED, not built (invariant 3′ says a
        // column never has zero takes). It is unreachable for a full-extent
        // column -- every take crosses -- and becomes reachable only with the
        // §4.2 placeholder, a column longer than any take reaches. Refusing
        // keeps the alternative, an empty column, out of the model entirely.
        if (nHead == 0 || nTail == 0) {
            TW_LOGW( "cut", "split-clip: refusing to split a take column at "
                            "%lld -- %s half would hold no take at all "
                            "(plan 53 invariant 3′).",
                     (long long)inObjOffset, nHead == 0 ? "the head" : "the tail" );
            return {false, nullptr};
        }

        // Per-half numbering, and the index maps the comp maps are remapped
        // through. Original order is preserved within each half.
        QList<int> headIdxOf, tailIdxOf;
        int hn = 0, tn = 0;
        for (int i = 0; i < nT; ++i) {
            headIdxOf.append( recs[i].where != TailOnly ? hn++ : -1 );
            tailIdxOf.append( recs[i].where != HeadOnly ? tn++ : -1 );
        }

        // THE COMP MAP IS SPLIT TOO (plan 53 B3). On the direct shape the
        // column IS the placement, so the tail's frame domain re-bases to 0 and
        // the map has to follow it; the original is kept for the inverse,
        // because neither half can reconstruct it from the other.
        const twCompMap origMap = stack->compMap();
        const int origActive = stack->activeTakeIndex();
        twCompMap headMap, tailMap;
        stakes::splitCompMap( origMap, (int64_t)inObjOffset,
                              &headMap, &tailMap );
        headMap = stakes::remapCompMap( headMap, headIdxOf );
        tailMap = stakes::remapCompMap( tailMap, tailIdxOf );

        // BUILD THE TAIL IN ORIGINAL TAKE ORDER, so its indices are the
        // original order restricted to the takes that have material there --
        // which is exactly what tailIdxOf says, and what the inverse walks
        // back. `liveOrig` tracks what is still in the head as takes are moved
        // out of it, because a removal shifts every later index down.
        STakeStack *stack2 = new STakeStack(project);
        QList<int> liveOrig;
        for (int i = 0; i < nT; ++i) liveOrig.append(i);
        // The inverse's per-take record, filled as the halves are built.
        QList<SUnsplitClipAction::TakeRestore> restores;
        for (int i = 0; i < nT; ++i) {
            const Rec &r = recs[i];
            if (r.where == HeadOnly) continue;
            const int curIn = liveOrig.indexOf(i);
            if (curIn < 0) continue;
            if (r.where == Crossing) {
                SClipWindow *w1 = stack->takeAt(curIn);
                if (!w1) continue;
                SClipWindow *w2 = w1->cloneWindowOver(project);
                if (!w2) continue;
                // The tail piece starts at the tail column's own 0 and runs to
                // the take's original end. No loop -- a loop is a property of
                // the region that was cut in two.
                const length_t tailLen =
                    (length_t)( r.s + (offset_t)r.len - inObjOffset );
                w2->setWindowExact( r.tailAnchor, tailLen, 0, r.stretch );
                stack2->insertTake( *w2, -1, 0, tailLen );
            } else {
                // WHOLLY IN THE TAIL: moved, same object, tStart re-based.
                stakes::moveTakeBetweenColumns(
                    *stack, curIn, *stack2, -1,
                    r.s - inObjOffset, r.len );
                liveOrig.removeAt(curIn);
            }
        }
        // TRIM THE HEAD'S CROSSING TAKES, after the clones have read them.
        // A take wholly in the head is NOT touched: it already ends before the
        // split, and trimming it would be the old all-takes rule reasserting
        // itself over an extent the user chose.
        for (int i = 0; i < nT; ++i) {
            if (recs[i].where != Crossing) continue;
            const int curIn = liveOrig.indexOf(i);
            if (curIn >= 0)
                stack->setTakeExtent( curIn, recs[i].s,
                                      (length_t)( inObjOffset - recs[i].s ) );
        }

        // Each half's own length, written DIRECTLY: both halves' takes have
        // just been placed deliberately, and setDurationAll's clamp would be a
        // second opinion about geometry this code already settled.
        stack2->setColumnDuration( colDur - (length_t)inObjOffset );
        stack->setColumnDuration( (length_t)inObjOffset );

        // EACH HALF'S ACTIVE TAKE (plan 53 §4.1): the original one when it has
        // material in that half (re-indexed), else the lowest-index take with
        // material at the half's START -- a half may not come out with an
        // active take that is silent everywhere in it, which under the old
        // "both halves keep the selection" rule is exactly what a take wholly
        // on the other side would have left behind.
        auto pickActive = []( STakeStack *col, int preferred ) {
            if( preferred >= 0 && preferred < col->nTakes() ) return preferred;
            for( int i = 0; i < col->nTakes(); ++i )
                if( col->takeCoversPos( i, 0 ) ) return i;
            for( int i = 0; i < col->nTakes(); ++i )
                if( col->takeExtentLength( i ) > 0 ) return i;
            return -1;
        };
        const int headPref = ( origActive >= 0 && origActive < nT )
                                 ? headIdxOf.at( origActive ) : -1;
        const int tailPref = ( origActive >= 0 && origActive < nT )
                                 ? tailIdxOf.at( origActive ) : -1;
        stack2->setActiveTake( pickActive( stack2, tailPref ) );
        stack2->setCompMap( tailMap );
        stack->setActiveTake( pickActive( stack, headPref ) );
        // AFTER the head's own length: setCompMap emits durationChanged, and
        // the head must already be its new length when that reaches the track.
        stack->setCompMap( headMap );
        // ...and one publish for the head whatever happened above, because
        // setColumnDuration writes a field and emits nothing, setActiveTake
        // returns early when the index did not change, and an empty map is no
        // change to setCompMap. setDurationAll at the length it already has is
        // a geometric NO-OP (every take is inside it) whose only effect is the
        // durationChanged the track's resync needs.
        stack->setDurationAll( (length_t)inObjOffset );

        SLink *sl2 = new SLink(*stack2, NULL);
        sl2->setStartTime(startTime + inObjOffset);
        sl2->setParent(track);

        QList<int> firstPath = trackPath;
        firstPath.append(track->indexOfChild(link));
        QList<int> secondPath = trackPath;
        secondPath.append(track->indexOfChild(sl2));
        SUnsplitClipAction *inverse = new SUnsplitClipAction(
            firstPath, secondPath, fullDur, inObjOffset);
        if( !origMap.empty() ) inverse->setRestoreCompMap( origMap );
        // THE PER-TAKE RECORD, which is what makes this inverse MIXED (plan 53
        // §4.1): a moved take comes back by identity at its original index and
        // extent, a crossing take is re-grown in the head, and the tail's
        // clones die with the tail. None of that is derivable from the two
        // halves afterwards -- which half a take came from is exactly the
        // information the split consumed.
        for (int i = 0; i < nT; ++i) {
            SUnsplitClipAction::TakeRestore tr;
            tr.origIndex = i;
            tr.tStart    = recs[i].s;
            tr.tLen      = recs[i].len;
            tr.fromTail  = ( recs[i].where == TailOnly );
            tr.halfIndex = tr.fromTail ? tailIdxOf.at(i) : headIdxOf.at(i);
            restores.append(tr);
        }
        inverse->setTakeRestore( restores, origActive );
        return {true, inverse};
    }

    // Ensure the clip is a WINDOW (wrap raw content), replacing the link in
    // place. The window creates its OWN content link (+1 ref on obj0) — the
    // old adopting ctor took `link` itself as content_, and the `delete link`
    // below then left the window's content_ dangling (use-after-free on the
    // next getContent()). Delete the old placement link only AFTER the window
    // holds its ref, so obj0's refcount never touches zero (removeRef()'s
    // deleteLater() cannot be rescinded).
    //
    // Which window type that is comes from the CONTENT's kind (the wrap
    // factory), not from a class-name compare here — this line used to strcmp
    // obj0.metaObject()->className() against the audio window's class name,
    // which no second window type could ever have satisfied.
    SLink *cutLink = link;
    if (!SClipWindow::of(&obj0)) {
        SObject *parentObj = (SObject*)link->parent();
        SClipWindow *wrapped =
            SClipWindow::wrapContent((SProject*)obj0.parent(), obj0);
        if (!wrapped) {
            return {false, nullptr};
        }
        SLink *nlk = new SLink(wrapped->asObject());
        nlk->setStartTime(startTime);
        delete link;
        nlk->setParent(parentObj);
        cutLink = nlk;
    }
    SClipWindow *w1 = SClipWindow::of(&cutLink->getSObject());
    if (!w1) {
        return {false, nullptr};
    }
    length_t origDur = w1->durationBlocking();   // edit path — never stale (P19)

    // Second part: a new window over the same content, starting at the split
    // point. Offsets and durations live in the window's *output* (stretched)
    // frame domain — the domain inObjOffset is measured in — so the tail must
    // carry the head's time scaling; cloneWindowOver copies it (and the grain
    // params) faithfully, and setWindowExact then narrows the copy. Copying
    // through setGrainParams() instead would preserve-span-rescale the
    // duration and double-apply the factor.
    // A WINDOW OVER A TAKE COLUMN GIVES ITS TAIL ITS OWN COLUMN (proposal 42
    // M3). `cloneWindowOver` shares the content, which is right for a wave and
    // WRONG for a column: two placements of one `STakeStack` share its
    // `activeTake_`, so after the split, comping either half comped BOTH and
    // per-region comping — the entire point of splitting a take column — was
    // impossible on this shape. Measured before the fix: clicking take 1's
    // lane on the HEAD moved the TAIL's audio too.
    //
    // The head keeps the original column and the tail gets a deep copy; each
    // window already covers its own region of it (the split arithmetic below
    // is unchanged), so nothing about the takes' own windows has to move.
    //
    // The DIRECT shape never reaches here — the branch above builds a second
    // `STakeStack` itself, and additionally TRIMS every take, which a wrapper
    // must not do: the wrapper does the windowing, and trimming the takes as
    // well would window twice.
    SObject &w1Content = w1->windowContent();
    SClipWindow *w2 = nullptr;
    if( STakeStack *column = dynamic_cast<STakeStack *>( &w1Content ) ) {
        STakeStack *tailColumn = stakes::cloneColumn( project, *column );
        if( !tailColumn ) return {false, nullptr};
        w2 = w1->cloneWindowOverContent( project, *tailColumn );
    } else {
        w2 = w1->cloneWindowOver(project);
    }
    if (!w2) {
        return {false, nullptr};
    }
    // W1: exact rational, no floor, through the window's own map.
    w2->setWindowExact( w1->timelineToSourceExact(
                            Fraction( (int64_t)inObjOffset ) ),
                        origDur - inObjOffset, 0, w1->stretchOrRate() );
    w1->setDurationFromTimeline(inObjOffset);
    SLink *sl2 = new SLink(w2->asObject(), NULL);
    sl2->setStartTime(startTime + inObjOffset);
    sl2->setParent(track);

    // Inverse addresses both parts by their post-split indices.
    QList<int> firstPath = trackPath;  firstPath.append(track->indexOfChild(cutLink));
    QList<int> secondPath = trackPath; secondPath.append(track->indexOfChild(sl2));
    SAction *inverse = new SUnsplitClipAction(firstPath, secondPath, origDur, inObjOffset);
    return {true, inverse};
}

void SSplitClipAction::writeXml(QDomElement &elem) const
{
    elem.setAttribute("clip", qualifiedToString( pathRoot_, clipPath_ ));
    elem.setAttribute("splitTime", QString::fromStdString(Fraction(splitTime_, 1).toString()));
    elem.setAttribute("broadcast", broadcast_ ? 1 : 0);
}

bool SSplitClipAction::readXml(const QDomElement &elem, int /*version*/)
{
    clipPath_ = parseInto( pathRoot_, elem.attribute("clip") );
    splitTime_ = (offset_t)parseFractionOrDouble(elem.attribute("splitTime", "0").toStdString()).toDouble();
    broadcast_ = elem.attribute("broadcast", "1").toInt() != 0;
    return true;
}

static const bool s_reg_splitclip = (
    SActionRegistry::instance().registerType(
        QStringLiteral("split-clip"),
        []{ return new SSplitClipAction; }
    ), true
);
