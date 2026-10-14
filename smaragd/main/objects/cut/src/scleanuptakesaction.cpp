#include "app/objects/cut/scleanuptakesaction.h"

#include <QDebug>
#include <QDomElement>
#include <QFile>

#include "tw/core/twlog.h"

#include "app/actions/sactionregistry.h"
#include "app/actions/scompositeaction.h"
#include "app/model/sclipwindow.h"
#include "app/model/slink.h"
#include "app/model/sobjectpath.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/model/srecordingfiles.h"
#include "app/model/stakenames.h"
#include "app/objects/cut/scompmapactions.h"
#include "app/objects/cut/sremovetakeaction.h"
#include "app/objects/cut/stakehelpers.h"
#include "app/objects/cut/stakestack.h"

namespace {

/// Every placement under `obj` that carries a take COLUMN, as a path from the
/// arrangement root. Depth-first, so a folder track's children are included.
void collectColumns( SObject *obj, const QList<int> &prefix,
                     QList<QList<int>> &out )
{
    if( !obj ) return;
    int i = 0;
    for( SLink *lk : obj->childLinks() ) {
        const int idx = i++;
        if( !lk ) continue;
        SObject *child = &lk->getSObject();
        QList<int> here = prefix;
        here.append( idx );
        if( child->isLane() ) {
            collectColumns( child, here, out );
            continue;
        }
        if( stakes::columnOfLink( lk ) ) out.append( here );
    }
}

/**
 * DOES THE COMP MAP SELECT TAKE `i` ANYWHERE IT HAS MATERIAL?
 *
 * §6.3's `takeHasMaterialAt` ∧ `takeIndexAt`, read as an interval question
 * rather than per frame: a segment naming the take covers [seg.at, nextAt),
 * the take has material over [extentStart, extentStart + extentLength), and
 * the take is USED iff those two overlap for some segment.
 *
 * Per frame would be the same answer at a few million times the cost, and
 * stating it as an overlap is also what makes the half-open ends explicit —
 * a segment that starts exactly where the take's material ENDS selects
 * nothing of it.
 */
bool compMapUsesTake( STakeStack *stack, int i )
{
    if( !stack ) return false;
    const std::vector<twCompSegment> &segs = stack->compMap().segments();
    if( segs.empty() ) return false;

    const offset_t ea = stack->takeExtentStart( i );
    const length_t el = stack->takeExtentLength( i );
    const offset_t colEnd = (offset_t) stack->getDurationBlocking();
    const offset_t eb = ( el < 0 ) ? colEnd : ea + (offset_t) el;

    for( size_t k = 0; k < segs.size(); ++k ) {
        if( segs[ k ].take != i ) continue;
        const offset_t a = (offset_t) segs[ k ].at;
        const offset_t b = ( k + 1 < segs.size() )
                               ? (offset_t) segs[ k + 1 ].at
                               : colEnd;
        if( ea < b && eb > a ) return true;
    }
    return false;
}

} // namespace

SCleanupTakesAction::SCleanupTakesAction( const QString &scope,
                                          const QString &clips,
                                          bool includeOriginals, bool files )
    : scope_( scope ), clips_( clips ), includeOriginals_( includeOriginals ),
      files_( files )
{
}

SApplyResult SCleanupTakesAction::apply( SProject *project )
{
    if( !project ) return { false, nullptr };
    removedTakes_  = 0;
    keptOriginals_ = 0;

    SObject *root = splacements::rootNamed( project, pathRoot_ );
    if( !root ) return { false, nullptr };

    // ---- WHICH COLUMNS ----------------------------------------------------
    QList<QList<int>> columns;
    if( !clips_.isEmpty() ) {
        for( const QString &one : clips_.split( QLatin1Char( ';' ),
                                                Qt::SkipEmptyParts ) ) {
            bool ok = false;
            const QList<int> p = strackpath::stringToPath( one.trimmed(), &ok );
            if( ok && !p.isEmpty() ) columns.append( p );
        }
    } else if( scope_ == QLatin1String( "project" ) ) {
        collectColumns( root, QList<int>(), columns );
    } else {
        qWarning() << "cleanup-takes: scope" << scope_
                   << "needs a clips= list (see the header: this slice cannot"
                      " see a selection)";
        return { false, nullptr };
    }

    // ---- WHAT TO REMOVE, DECIDED BEFORE ANYTHING MOVES -------------------
    //
    // The whole plan is computed first, then applied. Deciding and removing in
    // one pass would read indices that the previous removal had already
    // renumbered -- `removeTake` shifts every take above the one it drops, and
    // so does the comp map.
    SCompositeAction *macro = new SCompositeAction;

    for( const QList<int> &clipPath : columns ) {
        QList<int> lanePath = clipPath;
        const int idx = lanePath.takeLast();
        SObject *lane = splacements::laneAt( root, lanePath );
        SLink *link = lane ? lane->childAt( idx ) : nullptr;
        STakeStack *stack = link ? stakes::columnOfLink( link ) : nullptr;
        if( !stack ) continue;

        const int active = stack->activeTakeIndex();
        QList<int> doomed;
        for( int t = 0; t < stack->nTakes(); ++t ) {
            if( t == active ) continue;               // what sounds, stays
            if( compMapUsesTake( stack, t ) ) continue;  // what is comped, stays
            SClipWindow *take = stack->takeAt( t );
            const QString nm = take ? take->asObject().getSName() : QString();
            if( !stakenames::isRecorded( nm ) ) {
                // THE USER'S OWN CLIP. Exempt by D5 (a) unless asked for: it is
                // their arrangement, not a recording artefact, and W1 turns it
                // into an inactive take without the user doing anything.
                // Counted only when it is actually KEPT: with
                // `includeOriginals` the same take is removed, and reporting
                // it as kept would make the confirmation line contradict what
                // the sweep did.
                if( !includeOriginals_ ) { ++keptOriginals_; continue; }
            }
            doomed.append( t );
        }
        // HIGHEST INDEX FIRST, so each removal leaves the lower indices -- and
        // therefore every index still queued -- meaning what it meant when the
        // plan was made.
        for( int k = doomed.size() - 1; k >= 0; --k ) {
            macro->append( new SRemoveTakeAction( clipPath, doomed[ k ] ) );
            ++removedTakes_;
        }
    }

    if( macro->count() == 0 ) {
        // Nothing to do is a SUCCESS with no inverse, not a refusal: a user who
        // runs clean-up on a tidy project has not made a mistake. A composite
        // with no children fails its own apply(), so it must not be submitted.
        delete macro;
        TW_LOGI( "cut", "cleanup-takes: nothing to remove (%d original clip(s) "
                        "kept)", keptOriginals_ );
        if( files_ ) binOrphanRecordings_( project );
        return { true, nullptr };
    }

    const SApplyResult r = macro->apply( project );
    if( !r.applied ) {
        delete macro;
        return { false, nullptr };
    }
    // The macro STAYS on this action: its children hold the pins the inverses
    // read back (see the member's comment). A stale one from a previous apply
    // goes now, not before -- the plan above was built against the model the
    // old pins are not part of.
    delete macro_;
    macro_ = macro;

    TW_LOGI( "cut", "cleanup-takes: removed %d recorded take(s), kept %d "
                    "original clip(s)", removedTakes_, keptOriginals_ );

    // ---- THE FILE HALF, AFTER the model half and NOT on the undo stack ---
    if( files_ ) binOrphanRecordings_( project );

    // The composite's own inverse undoes every removal in reverse, which is
    // what makes the whole sweep ONE undo step (§6.3's "as ONE undoable
    // macro"). The children are `remove-take`s, and since QBX-184 each of them
    // has a real inverse even for a take with no file behind it -- without
    // that, one event take in the sweep would have made the entire macro
    // non-undoable.
    return { true, r.inverse };
}

SCleanupTakesAction::~SCleanupTakesAction()
{
    delete macro_;
}

void SCleanupTakesAction::binOrphanRecordings_( SProject *project )
{
    // §6.3's step 4, and the one part of this verb that is NOT undoable --
    // which is why it runs last, after the model half has succeeded, and why
    // the confirmation dialog says so in one sentence before it is reached.
    //
    // MOVED TO THE TRASH, NEVER DELETED IN PLACE. A sweep that could destroy a
    // recording outright would be a feature nobody dares run; the trash means
    // the worst case is an inconvenience.
    const QList<srecfiles::Orphan> orphans = srecfiles::scanUnused( project );
    int moved = 0, failed = 0;
    for( const srecfiles::Orphan &o : orphans ) {
        if( o.path.isEmpty() ) continue;
        if( QFile::moveToTrash( o.path ) ) ++moved;
        else {
            ++failed;
            TW_LOGW( "cut", "cleanup-takes: could not bin %s",
                     o.path.toStdString().c_str() );
        }
    }
    TW_LOGI( "cut", "cleanup-takes: binned %d orphan recording(s), %d could "
                    "not be moved", moved, failed );
}

void SCleanupTakesAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "scope", scope_ );
    if( !clips_.isEmpty() )    elem.setAttribute( "clips", clips_ );
    if( includeOriginals_ )    elem.setAttribute( "includeOriginals", "1" );
    if( files_ )               elem.setAttribute( "files", "1" );
}

bool SCleanupTakesAction::readXml( const QDomElement &elem, int )
{
    scope_ = elem.attribute( "scope", "project" );
    clips_ = elem.attribute( "clips", "" );
    includeOriginals_ =
        elem.attribute( "includeOriginals", "0" ) != QLatin1String( "0" );
    files_ = elem.attribute( "files", "0" ) != QLatin1String( "0" );
    return true;
}

static const bool s_reg_cleanup_takes = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "cleanup-takes" ),
        []{ return new SCleanupTakesAction; } ), true );
