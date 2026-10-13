#include "app/objects/midi/smidirecordactions.h"

#include <algorithm>

#include <QDebug>
#include <QDomElement>

#include "app/actions/sactionregistry.h"
#include "app/actions/scompositeaction.h"
#include "app/model/sclipwindow.h"
#include "app/model/slink.h"
#include "app/model/sobjectpath.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/objects/midi/smidiactionsupport.h"
#include "app/objects/midi/smidiclipactions.h"
#include "app/objects/midi/smidicut.h"
#include "app/objects/midi/smidieventactions.h"
#include "app/objects/midi/seventpassverbs.h"
#include "app/objects/recplan/srecordplanner.h"

#include "tw/events/twtempomap.h"

using namespace strackpath;

namespace {

/** Every `<n .../>` / `<e .../>` child, sorted (the tables are kept sorted). */
std::vector<SEvent> readEvents( const QDomElement &elem )
{
    std::vector<SEvent> ev = smidiactions::readEventChildren( elem );
    std::stable_sort( ev.begin(), ev.end() );
    return ev;
}

// `<e .../>` children, NOT `<n .../>`: a recorded pass carries CCs, bends and
// pressure as well as notes, and the note spelling would read every one of
// them back as a NoteOn. `readEventChildren` accepts both, so a hand-written
// case may still use `<n>` for a pass that is only notes.
void writeEvents( QDomElement &elem, const std::vector<SEvent> &events )
{
    smidiactions::writeEventChildren( elem, events );
}

/**
 * The EVENT columns of a lane that overlap `[from, to)`, in timeline order.
 *
 * "Column" is the lane child, whatever it is: a plain `SMidiCut` placement or
 * a take stack of them. The kind test is `contentKind()`, which a stack
 * answers for its takes - so an audio column is never a candidate and a
 * recorded pass over one becomes a clip of its own beside it rather than a
 * refused action.
 */
struct Column {
    SLink   *link  = nullptr;
    offset_t start = 0;
    length_t dur   = 0;
    int      index = 0;
};

std::vector<Column> eventColumns( SObject *lane, offset_t from, offset_t to )
{
    std::vector<Column> out;
    if( !lane ) return out;
    for( int i = 0; i < lane->childCount(); ++i ) {
        SLink *lk = lane->childAt( i );
        if( !lk || lk->getSObject().isLane() ) continue;   // sub-track
        SObject &obj = lk->getSObject();
        if( !obj.hasDuration() ) continue;
        if( obj.contentKind() != SContentKind::Event ) continue;
        const offset_t s = lk->getStartTime();
        const length_t d = obj.getDurationBlocking();
        if( d == 0 || s + (offset_t) d <= from || s >= to ) continue;
        out.push_back( { lk, s, d, i } );
    }
    std::sort( out.begin(), out.end(),
               []( const Column &a, const Column &b )
               { return a.start < b.start; } );
    return out;
}

/** The window of a placement, or null when it is not one. */
SClipWindow *windowOf( SLink *link, int take )
{
    if( !link ) return nullptr;
    SObject &obj = link->getSObject();
    if( SClipWindow *w = obj.windowTakeAt( take ) ) return w;
    return SClipWindow::of( &obj );
}

}  // namespace

// ---------------------------------------------------------------------------
// add-midi-take
// ---------------------------------------------------------------------------

SAddMidiTakeAction::SAddMidiTakeAction( const QList<int> &clipPath,
                                        std::vector<SEvent> events,
                                        qint64 lengthTicks, const QString &name,
                                        int index, bool activate,
                                        qint64 atTicks, qint64 extentTicks )
    : clipPath_( clipPath ), events_( std::move( events ) ),
      lengthTicks_( lengthTicks ), takeName_( name ), index_( index ),
      activate_( activate ), atTicks_( atTicks ), extentTicks_( extentTicks )
{
}

SApplyResult SAddMidiTakeAction::apply( SProject *project )
{
    if( !project || clipPath_.isEmpty() ) return { false, nullptr };
    SObject *mixer = splacements::rootNamed( project, pathRoot_ );
    if( !mixer ) return { false, nullptr };

    QList<int> lanePath = clipPath_;
    const int  idx  = lanePath.takeLast();
    SObject   *lane = splacements::laneAt( mixer, lanePath );
    SLink     *link = lane ? lane->childAt( idx ) : nullptr;
    if( !link || link->getSObject().isLane() ) {
        qWarning() << "add-midi-take: no clip at" << qualifiedToString( pathRoot_, clipPath_ );
        return { false, nullptr };
    }

    // A plain placement becomes a single-take COLUMN first (take 0 = what was
    // there), through the model's registered factory - `objects/midi` never
    // names STakeStack. Only an EVENT placement qualifies: wrapping an audio
    // cut here would build a column the insert below is then bound to refuse,
    // and it would have moved the user's clip for nothing.
    if( link->getSObject().windowTakeCount() == 0 ) {
        if( link->getSObject().contentKind() != SContentKind::Event ) {
            qWarning() << "add-midi-take: the clip at"
                       << qualifiedToString( pathRoot_, clipPath_ ) << "is not an event clip";
            return { false, nullptr };
        }
        link = SClipWindow::wrapIntoTakeColumn( project, lane, link );
        if( !link || link->getSObject().windowTakeCount() == 0 ) {
            qWarning() << "add-midi-take: could not wrap"
                       << qualifiedToString( pathRoot_, clipPath_ ) << "into a take column";
            return { false, nullptr };
        }
    }
    SObject &column = link->getSObject();
    const int prevActive = column.activeWindowTakeIndex();

    // Blocking read (P19): the new take must adopt the column's CURRENT
    // duration, never the stale try-lock fallback - it is an edit path.
    const length_t colDur = column.getDurationBlocking();

    // THE EXTENT, ticks -> frames, ONCE (plan 53 §4.1 / D6 (a)). The frames
    // are what the column's own bookkeeping wants - a take's extent start IS
    // its link's startTime - and `insertWindowTake`'s defaults (0, -1) are
    // "the whole column", so an absent extent reaches `insertTake` as the
    // pre-extent call it always was.
    //
    // LENGTH CONVERSION, not position conversion, exactly as the derivation
    // below already does for the column's duration: a take's link is placed
    // relative to the COLUMN, and a column does not know where it sits on the
    // timeline, so there is no absolute position to look a tempo up at. A
    // tempo change INSIDE one column is therefore not modelled here - the same
    // approximation this verb has always made.
    const twTempoMap &tmap = project->tempoMap();
    const int         srate = project->getSRate();
    const offset_t extentStart =
        atTicks_ > 0
            ? (offset_t) tmap.ticksToFrames(
                  TickLen( Fraction( atTicks_, 1 ) ), srate ).floorToInt()
            : (offset_t) 0;
    const length_t extentLen =
        extentTicks_ > 0
            ? (length_t) tmap.ticksToFrames(
                  TickLen( Fraction( extentTicks_, 1 ) ), srate ).floorToInt()
            : (length_t) -1;

    qint64 lengthTicks = lengthTicks_;
    if( lengthTicks <= 0 ) {
        // Derive the sequence's musical length from the take's own extent when
        // it has one, and only otherwise from the column's frames - once,
        // through THE tempo map (POSITION_DOMAINS rule 7). A partial take whose
        // sequence ran the whole column's length would hold notes its window
        // can never show.
        lengthTicks = extentTicks_ > 0
            ? extentTicks_
            : tmap.framesToTickLen( (int64_t) colDur, srate )
                  .ticks().floorToInt();
    }

    SMidiSequence *seq = new SMidiSequence( project );
    seq->setLengthTicks( lengthTicks );
    seq->setOrigin( SMidiSequence::Origin::Recorded );
    if( !events_.empty() ) seq->setEvents( events_ );
    if( !takeName_.isEmpty() ) seq->setSName( takeName_ );

    SClipWindow *win = SClipWindow::wrapContent( project, *seq );
    if( !win ) { delete seq; return { false, nullptr }; }
    if( !takeName_.isEmpty() ) win->asObject().setSName( takeName_ );
    // THE WINDOW'S LENGTH IS THE TAKE'S EXTENT (invariant 1'), and with no
    // extent it is the column's duration - invariant 1 as this verb always
    // read it.
    //
    // SET IN TICKS WHERE WE HAVE TICKS. `setDurationFromTimeline` takes frames
    // and converts back, so an extent given as ticks would make the round trip
    // ticks -> frames -> ticks and come back off by one at any tempo where a
    // tick does not divide a frame. The window is freshly wrapped, so its
    // source anchor, loop and rate are at their identity values and naming
    // them here changes nothing.
    SMidiCut *mcut = dynamic_cast<SMidiCut *>( &win->asObject() );
    // ...AND THE EXTENT LENGTH IS THEN HANDED TO THE COLUMN AS -1, which means
    // "the window's own duration" -- the ONE spelling that leaves the exact
    // tick length alone. `insertTake` does `setDurationFromTimeline( tLen )`
    // for any tLen >= 0, so passing the converted frames would immediately
    // overwrite `lengthTicks_` with the frame value's own round trip.
    // Measured: a take added at 143 BPM with `extentTicks="3840"` came back at
    // 120 BPM 95999 frames long instead of 96000, one frame short, because
    // 3840 ticks is 80559.44 frames there and the trip back rounds to 3839.
    length_t insertLen = extentLen;
    if( extentTicks_ > 0 && mcut ) {
        mcut->setWindowTicks( Fraction( 0, 1 ),
                              Fraction( extentTicks_, 1 ),
                              Fraction( 0, 1 ), Fraction( 1, 1 ) );
        insertLen = -1;
    } else {
        win->setDurationFromTimeline( extentLen >= 0 ? extentLen : colDur );
    }

    const int at = ( index_ >= 0 && index_ <= column.windowTakeCount() )
                       ? index_ : column.windowTakeCount();
    if( !column.insertWindowTake( *win, at == column.windowTakeCount() ? -1 : at,
                                  extentStart, insertLen ) ) {
        // Homogeneity (proposal 37 D8b): the column holds audio takes. Reject
        // rather than build a column that plays notes or audio depending on
        // which lane is active.
        delete &win->asObject();
        return { false, nullptr };
    }
    if( activate_ ) column.setActiveWindowTake( at );

    QList<int> columnPath = lanePath;
    columnPath.append( lane->indexOfChild( link ) );
    return { true, new SRemoveMidiTakeAction( columnPath, at,
                                              activate_ ? prevActive : -2 ) };
}

void SAddMidiTakeAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "clip", qualifiedToString( pathRoot_, clipPath_ ) );
    elem.setAttribute( "index", index_ );
    elem.setAttribute( "activate", activate_ ? 1 : 0 );
    elem.setAttribute( "name", takeName_ );
    elem.setAttribute( "lengthTicks", QString::number( lengthTicks_ ) );
    // Written only when they say something (plan 53 §4.1, as `add-take at=`
    // does): `atTicks` 0 and no `extentTicks` IS "the whole column", so a
    // full-extent take serializes exactly as it did before extents existed.
    if( atTicks_ != 0 )
        elem.setAttribute( "atTicks", QString::number( atTicks_ ) );
    if( extentTicks_ >= 0 )
        elem.setAttribute( "extentTicks", QString::number( extentTicks_ ) );
    writeEvents( elem, events_ );
}

bool SAddMidiTakeAction::readXml( const QDomElement &elem, int )
{
    clipPath_    = parseInto( pathRoot_, elem.attribute( "clip" ) );
    index_       = elem.attribute( "index", "-1" ).toInt();
    activate_    = elem.attribute( "activate", "1" ).toInt() != 0;
    takeName_    = elem.attribute( "name", "" );
    lengthTicks_ = elem.attribute( "lengthTicks", "0" ).toLongLong();
    atTicks_     = elem.attribute( "atTicks", "0" ).toLongLong();
    extentTicks_ = elem.attribute( "extentTicks", "-1" ).toLongLong();
    events_      = readEvents( elem );
    return true;
}

static const bool s_reg_add_midi_take = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "add-midi-take" ),
        []{ return new SAddMidiTakeAction; } ), true );

// ---------------------------------------------------------------------------
// remove-midi-take (add-midi-take's inverse; created live)
// ---------------------------------------------------------------------------

SRemoveMidiTakeAction::SRemoveMidiTakeAction( const QList<int> &columnPath,
                                              int takeIndex, int thenActivate )
    : columnPath_( columnPath ), takeIndex_( takeIndex ),
      thenActivate_( thenActivate )
{
}

SApplyResult SRemoveMidiTakeAction::apply( SProject *project )
{
    if( !project || columnPath_.isEmpty() ) return { false, nullptr };
    SObject *mixer = splacements::rootNamed( project, pathRoot_ );
    if( !mixer ) return { false, nullptr };

    QList<int> lanePath = columnPath_;
    const int  idx  = lanePath.takeLast();
    SObject   *lane = splacements::laneAt( mixer, lanePath );
    SLink     *link = lane ? lane->childAt( idx ) : nullptr;
    if( !link ) return { false, nullptr };
    SObject &column = link->getSObject();
    if( takeIndex_ < 0 || takeIndex_ >= column.windowTakeCount() )
        return { false, nullptr };

    // The take's whole state, captured BEFORE it is removed, so the inverse
    // restores the notes and not just an empty lane. A lost take is not an undo.
    SClipWindow *take = column.windowTakeAt( takeIndex_ );
    std::vector<SEvent> events;
    qint64  lengthTicks = 0;
    QString takeName;
    // ...AND ITS EXTENT, for the reason `remove-take` carries one on the audio
    // side: `add-midi-take`'s defaults mean "the whole column", so without
    // these two numbers undoing the removal of a PARTIAL take would give back
    // a FULL-extent one - a take that suddenly answers for bars it never
    // played. Read BEFORE the removal, which takes the take's link (and with
    // it its extent start) away.
    //
    // Both are read in TICKS wherever ticks exist: the window's length IS a
    // Fraction of ticks on the `SMidiCut`, so it needs no conversion at all.
    // Only the extent START has to come through the tempo map, because it
    // lives on the take's link as frames.
    qint64 atTicks = 0, extentTicks = -1;
    {
        const offset_t exStart = column.windowTakeExtentStart( takeIndex_ );
        if( exStart > 0 )
            atTicks = project->tempoMap()
                          .framesToTickLen( (int64_t) exStart,
                                            project->getSRate() )
                          .ticks().floorToInt();
    }
    if( take ) {
        takeName = take->asObject().getSName();
        if( SMidiCut *cut = dynamic_cast<SMidiCut *>( &take->asObject() ) ) {
            extentTicks = cut->getLengthTicks().floorToInt();
            if( SMidiSequence *seq = cut->sequence() ) {
                events      = seq->events();
                lengthTicks = seq->lengthTicks();
            }
        }
    }
    const bool wasActive = ( column.activeWindowTakeIndex() == takeIndex_ );

    column.removeWindowTake( takeIndex_ );
    if( thenActivate_ >= -1 ) column.setActiveWindowTake( thenActivate_ );

    // Take-stack invariant 3: a single remaining take collapses back to a
    // plain placement, so an undo leaves the tree exactly as it was found.
    SLink *result = link;
    if( column.windowTakeCount() == 1 ) {
        if( SLink *plain = SClipWindow::collapseTakeColumn( lane, link ) )
            result = plain;
    }

    QList<int> invPath = lanePath;
    invPath.append( lane->indexOfChild( result ) );
    return { true, new SAddMidiTakeAction( invPath, std::move( events ),
                                           lengthTicks, takeName, takeIndex_,
                                           wasActive, atTicks, extentTicks ) };
}

void SRemoveMidiTakeAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "clip", qualifiedToString( pathRoot_, columnPath_ ) );
    elem.setAttribute( "take", takeIndex_ );
    elem.setAttribute( "thenActivate", thenActivate_ );
}

bool SRemoveMidiTakeAction::readXml( const QDomElement &, int )
{
    return false;   // created live as add-midi-take's inverse
}

// ---------------------------------------------------------------------------
// place-midi-recording
// ---------------------------------------------------------------------------

SPlaceMidiRecordingAction::SPlaceMidiRecordingAction(
    const QList<int> &trackPath, offset_t timePos, qint64 durationTicks,
    std::vector<SEvent> events, const QString &mode, const QString &quantize,
    const QString &name )
    : trackPath_( trackPath ), timePos_( timePos ),
      durationTicks_( durationTicks ), events_( std::move( events ) ),
      mode_( mode ), quantize_( quantize ), clipName_( name )
{
}

SPlaceMidiRecordingAction::~SPlaceMidiRecordingAction()
{
    delete plan_;
}

bool SPlaceMidiRecordingAction::isKnownMode( const QString &mode )
{
    return mode == QStringLiteral( "new-take" )
        || mode == QStringLiteral( "takes" )
        || mode == QStringLiteral( "takes-split" )
        || mode == QStringLiteral( "overdub" )
        || mode == QStringLiteral( "replace" );
}

QString SPlaceMidiRecordingAction::canonicalMode( const QString &mode )
{
    // `new-take` IS `takes`, and that is the whole mode reconciliation (plan
    // 53 P3). This verb was written against proposal 21 L4's three-value
    // vocabulary (`new-take | overdub | replace`); plan 53's is
    // `takes | takes-split | overdub | replace`, which is what
    // `SProjectProps::RecordMode` stores, what `srecmode::resolveMode`
    // answers and what `srecplan::Mode` parses. The two agreed on `overdub`
    // and `replace` already and disagreed on nothing else -- `new-take` named
    // the same intention `takes` names.
    //
    // THE OLD SPELLING KEEPS WORKING, as the INI's does
    // (`srecmodeseed.cpp`): a `.qxa`, a saved action and a user's settings
    // written before plan 53 must keep meaning what they meant. What CHANGED
    // under it is not the name but the behaviour: `takes` is now the union
    // column of §3.1, not "a new take on the first touched column".
    if( mode == QStringLiteral( "new-take" ) ) return QStringLiteral( "takes" );
    if( isKnownMode( mode ) ) return mode;
    return QStringLiteral( "takes" );
}

SApplyResult SPlaceMidiRecordingAction::apply( SProject *project )
{
    if( !project ) return { false, nullptr };
    SObject *mixer = splacements::rootNamed( project, pathRoot_ );
    SObject *lane  = splacements::placementLaneAt( mixer, trackPath_, "place-midi-recording" );
    if( !lane ) {
        qWarning() << "place-midi-recording: no lane at"
                   << qualifiedToString( pathRoot_, trackPath_ );
        return { false, nullptr };
    }
    const QString mode = canonicalMode( mode_ );

    const twTempoMap &map  = project->tempoMap();
    const int         rate = project->getSRate();
    qint64 durationTicks = durationTicks_;
    if( durationTicks <= 0 )
        durationTicks = (qint64) map.ppq() * 4 * map.numerator() / map.denominator();
    // The pass window in FRAMES, derived once, here.
    const length_t durationFrames = (length_t)
        map.ticksToFrames( TickLen( Fraction( durationTicks, 1 ) ), rate )
           .floorToInt();

    // HELD, NOT STACK-LOCAL -- see the note on `plan_`. `merge-columns`'
    // inverse borrows the merge action itself, so the children must outlive
    // the inverse this returns.
    delete plan_;                       // a redo re-plans from scratch
    plan_ = new SCompositeAction();
    SCompositeAction &composite = *plan_;
    QList<int>       clipPath;      // what `quantize` will address

    // ----------------------------------------------------------------------
    // `takes` AND `takes-split` GO THROUGH THE SHARED PLANNER (plan 53 P3).
    //
    // One planner, two content kinds: `objects/recplan` decides the union
    // geometry, the operand order, the pre-flight and the index bookkeeping,
    // and `SEventPassVerbs` says the result in this module's verbs. What dies
    // with this branch is everything below it that only ever half-worked for a
    // recording:
    //
    //   * `eventColumns`' "FIRST COLUMN ONLY" rule (`cols.front()`). A pass
    //     spanning two event clips used to land on the earlier one and leave
    //     the later one alone, so the performance over its bars was simply not
    //     there. Mode A folds every touched child into ONE column.
    //   * THE MISSING PREDATES RULE. A column starting BEFORE the pass cannot
    //     receive it as a take -- a take is an alternative for the same
    //     window, so it would need material from before the recording began --
    //     and nothing here said so: the pass went on as a take anyway, at the
    //     column's own origin, i.e. EARLIER THAN IT WAS PLAYED. The audio side
    //     fixed this in B5 (`place_recording_over_earlier_clip`); the event
    //     side never had it.
    //   * X16, the ticket's own case: a pass over bars 8-16 of a 32-bar clip.
    //
    // `overdub` and `replace` do NOT come here, and that is the plan rather
    // than an omission: §3.3 says for MIDI they keep their existing meanings
    // ("MERGED into the active take", "notes inside the pass window are
    // deleted ... CCs survive"), which are not the audio planners' meanings.
    // Their bodies are below, unchanged.
    // ----------------------------------------------------------------------
    if( mode == QStringLiteral( "takes" )
        || mode == QStringLiteral( "takes-split" ) ) {
        SEventPassVerbs verbs( project, pathRoot_, durationTicks, events_,
                               clipName_.isEmpty()
                                   ? QStringLiteral( "MIDI Recording" )
                                   : clipName_ );

        srecplan::Span span;
        span.a = timePos_;
        span.b = timePos_ + (offset_t) durationFrames;
        // `Span::extent` stays 0 (= `[a, b)`). It exists for a CYCLE pass the
        // AUDIO recorder clamped to the material it captured; the MIDI
        // recorder places each pass over the whole pass window already, so
        // there is nothing wider to say.

        QList<QList<int>> landed;
        srecplan::Context ctx;
        ctx.mode   = srecplan::modeFromString( mode );
        ctx.armed  = true;
        ctx.kind   = SContentKind::Event;
        ctx.verbs  = &verbs;
        ctx.landed = &landed;

        QString why;
        if( !srecplan::plannerFor( ctx.mode )
                 .plan( srecplan::snapshot( lane, trackPath_ ), span, ctx,
                        composite, &why ) ) {
            qWarning( "place-midi-recording: mode '%s' refused this lane: %s",
                      qPrintable( mode ), qPrintable( why ) );
            return { false, nullptr };
        }

        // INPUT QUANTISE, inside this verb's own composite -- one undo entry
        // for one recording. EVERY clip the pass landed on gets one: under
        // `takes-split` the pass is spread over the pieces it covers and the
        // gaps between them, and quantising only one of them would leave the
        // rest of the same performance unquantised. `take = -1` is the ACTIVE
        // take, which after the plan is the pass on a column and the clip
        // itself on a plain placement -- i.e. exactly the notes just recorded.
        if( !quantize_.isEmpty() && quantize_ != QStringLiteral( "off" )
            && SQuantizeNotesAction::gridTicks( quantize_,
                                                project->tempoMap().ppq() ) > 0 ) {
            for( const QList<int> &target : landed )
                composite.append( new SQuantizeNotesAction( target, quantize_,
                                                            1.0, 0.0, -1,
                                                            false ) );
        }
        return composite.apply( project );
    }

    const std::vector<Column> cols =
        eventColumns( lane, timePos_, timePos_ + (offset_t) durationFrames );

    if( cols.empty() ) {
        // Nothing here: a new clip carrying the pass. `insert-midi-clip` gives
        // it `timebase=beats` like every event placement, so the take moves
        // with the bar under a later tempo edit.
        clipPath = trackPath_;
        clipPath.append( lane->childCount() );   // insert-midi-clip appends
        composite.append( new SInsertMidiClipAction( trackPath_, timePos_,
                                                     durationTicks, clipName_,
                                                     events_ ) );
    } else {
        const Column &col = cols.front();
        clipPath = trackPath_;
        clipPath.append( col.index );

        // WHERE THE PASS SITS IN THE CLIP'S CONTENT, exactly. The window owns
        // the map (slip + rate), so the base tick is asked for rather than
        // computed here - two callers converting independently is how a
        // rounding difference becomes an off-by-one note.
        SClipWindow *win = windowOf( col.link, -1 );
        Fraction     baseTicks( 0, 1 );
        Fraction     invRate( 1, 1 );
        if( win ) {
            baseTicks = win->timelineToSourceExact(
                Fraction( (int64_t) timePos_ - (int64_t) col.start, 1 ) );
            const Fraction r = win->stretchOrRate();
            if( r.numerator != 0 ) invRate = Fraction( 1, 1 ) / r;
        }
        const qint64 base = baseTicks.floorToInt();
        // Pass-window ticks -> the content's ticks.
        auto rebase = [&]( qint64 t ) -> qint64
        { return base + ( Fraction( t, 1 ) * invRate ).floorToInt(); };
        const qint64 winEnd =
            base + ( Fraction( durationTicks, 1 ) * invRate ).floorToInt();

        std::vector<SEvent> pass = events_;
        for( SEvent &e : pass ) {
            const qint64 endT = e.endTick();
            e.t   = rebase( e.t );
            if( e.kind == twEventKind::NoteOn )
                e.dur = std::max<qint64>( 0, rebase( endT ) - e.t );
        }

        {
            // Overdub / replace both END as one absolute `set-events` on the
            // column's ACTIVE take, which is the universal inverse every event
            // edit already uses (37 3.4) - so undo is the previous table,
            // verbatim, whichever mode was used.
            std::vector<SEvent> merged;
            smidiactions::ClipRef ref =
                smidiactions::resolveClip( project, pathRoot_, clipPath, -1 );
            if( !ref.valid() ) {
                qWarning() << "place-midi-recording: the column at"
                           << pathToString( clipPath )
                           << "holds no event take to merge into";
                return { false, nullptr };
            }
            merged = ref.seq->events();
            if( mode == QStringLiteral( "replace" ) ) {
                // Only NOTES inside the pass window go; CCs and everything
                // outside survive, exactly as `set-notes` promises.
                merged.erase(
                    std::remove_if( merged.begin(), merged.end(),
                                    [&]( const SEvent &e ) {
                                        return e.kind == twEventKind::NoteOn
                                            && e.t >= base && e.t < winEnd;
                                    } ),
                    merged.end() );
            }
            merged.insert( merged.end(), pass.begin(), pass.end() );
            std::stable_sort( merged.begin(), merged.end() );
            composite.append( new SSetEventsAction( clipPath, merged, -1 ) );
        }
    }

    // INPUT QUANTISE, inside this verb's own composite: one undo entry covers
    // the placement AND the quantise, rather than leaving the user two steps
    // to undo for one recording.
    if( !quantize_.isEmpty() && quantize_ != QStringLiteral( "off" )
        && SQuantizeNotesAction::gridTicks( quantize_, project->tempoMap().ppq() ) > 0 ) {
        composite.append( new SQuantizeNotesAction( clipPath, quantize_, 1.0,
                                                    0.0, -1, false ) );
    }

    return composite.apply( project );
}

void SPlaceMidiRecordingAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "trackPath", qualifiedToString( pathRoot_, trackPath_ ) );
    elem.setAttribute( "timePos", QString::fromStdString(
                           Fraction( (int64_t) timePos_, 1 ).toString() ) );
    elem.setAttribute( "durationTicks", QString::number( durationTicks_ ) );
    elem.setAttribute( "mode", mode_ );
    elem.setAttribute( "quantize", quantize_ );
    elem.setAttribute( "name", clipName_ );
    writeEvents( elem, events_ );
}

bool SPlaceMidiRecordingAction::readXml( const QDomElement &elem, int )
{
    trackPath_     = parseInto( pathRoot_, elem.attribute( "trackPath" ) );
    timePos_       = (offset_t) parseFractionOrDouble(
        elem.attribute( "timePos", "0" ).toStdString() ).toDouble();
    durationTicks_ = elem.attribute( "durationTicks", "0" ).toLongLong();
    mode_          = elem.attribute( "mode", "new-take" );
    quantize_      = elem.attribute( "quantize", "off" );
    clipName_      = elem.attribute( "name", "" );
    events_        = readEvents( elem );
    return true;
}

static const bool s_reg_place_midi_recording = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "place-midi-recording" ),
        []{ return new SPlaceMidiRecordingAction; } ), true );
