#include "app/objects/cut/smergecolumnsactions.h"
#include "app/objects/cut/stakestack.h"
#include "app/objects/cut/stakehelpers.h"
#include "app/model/sclipwindow.h"
#include "app/model/slink.h"
#include "app/model/sobject.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/model/sautomationlane.h"
#include "app/model/sobjectpath.h"
#include "app/actions/sactionregistry.h"
#include <QDomElement>
#include <QStringList>
#include <algorithm>
#include <utility>
#include <vector>

using namespace strackpath;

namespace {

/** The link attributes a placement carries, moved onto `to`. */
void carryLaneLinkAttrs( const SMergeColumnsAction::Operand &op, SLink *to )
{
    if( !to ) return;
    to->setStartTime( op.origStart );
    to->setTimebase( op.timebase );
    to->setStartTicks( op.startTicks );
    to->setEventChannelOverride( op.eventChannelOverride );
}

/**
 * THE WINDOW WHOSE PARAMETERS THE MIX READS for this placement — the same
 * `SClipWindow::parametersOf` call `STrack::refreshClipGainCurves` makes, so
 * the refusal below is decided on exactly the object the mix will consult and
 * cannot disagree with it.
 */
SObject *mixParamsOf( SObject &placementObject )
{
    if( SClipWindow *w = SClipWindow::parametersOf( placementObject ) )
        return &w->asObject();
    return &placementObject;
}

/** The take index in `col` holding `obj`, or -1. */
int takeIndexOfObject( STakeStack &col, SObject *obj )
{
    for( int i = 0; i < col.nTakes(); ++i )
        if( col.takeObjectAt( i ) == obj ) return i;
    return -1;
}

}  // namespace

SMergeColumnsAction::SMergeColumnsAction( const QList<QList<int>> &clipPaths )
    : clipPaths_( clipPaths )
{
}

SMergeColumnsAction::~SMergeColumnsAction()
{
    releasePins();
}

void SMergeColumnsAction::releasePins()
{
    for( Operand &op : operands_ ) {
        if( op.holdsColumnRef && op.column )
            op.column->removeRef();
        op.holdsColumnRef = false;
    }
}

void SMergeColumnsAction::dropStalePins()
{
    releasePins();
    operands_.clear();
}

SApplyResult SMergeColumnsAction::apply( SProject *project )
{
    // FIRST, before anything can fail or return: release pins from a previous
    // apply (SRemoveSampleAction::dropStalePin, same reason — a redo
    // re-applies this against NEW objects).
    dropStalePins();

    if( !project || clipPaths_.size() < 2 ) {
        qWarning( "merge-columns: REFUSED — %lld operand(s); merging needs at "
                  "least two clips on one lane.",
                  (long long) clipPaths_.size() );
        return { false, nullptr };
    }

    SObject *root = splacements::rootNamed( project, pathRoot_ );
    if( !root ) return { false, nullptr };

    // ---- resolve, and establish that this is ONE lane ----------------------
    QList<SLink *> links;
    SObject *lane = nullptr;
    for( const QList<int> &p : clipPaths_ ) {
        SLink *lk = splacements::placementAt( root, p );
        if( !lk ) {
            qWarning( "merge-columns: REFUSED — no placement at %s.",
                      qPrintable( qualifiedToString( pathRoot_, p ) ) );
            return { false, nullptr };
        }
        SObject *parent = dynamic_cast<SObject *>( lk->parent() );
        if( !parent ) return { false, nullptr };
        if( !lane ) lane = parent;
        else if( lane != parent ) {
            // One lane at a time (§6.1): a selection spanning lanes is run
            // once per lane by the UI, because the result is ONE column and a
            // column belongs to exactly one placement on one lane.
            qWarning( "merge-columns: REFUSED — the operands are on different "
                      "lanes; merge one lane at a time." );
            return { false, nullptr };
        }
        if( !lk->hasStartTime() ) {
            qWarning( "merge-columns: REFUSED — %s has no start time.",
                      qPrintable( qualifiedToString( pathRoot_, p ) ) );
            return { false, nullptr };
        }
        links.append( lk );
    }

    // Operands must be DISTINCT. The same clip named twice would move its
    // window into the column once and then look for it on the lane again.
    for( int i = 0; i < links.size(); ++i )
        for( int j = i + 1; j < links.size(); ++j )
            if( links[ i ] == links[ j ] ) {
                qWarning( "merge-columns: REFUSED — the same clip is named "
                          "more than once." );
                return { false, nullptr };
            }

    // ---- classify and refuse ----------------------------------------------
    SContentKind kind = SContentKind::Audio;
    bool kindSeen = false;
    for( int i = 0; i < links.size(); ++i ) {
        SLink   *lk  = links[ i ];
        SObject &obj = lk->getSObject();
        const QString where = qualifiedToString( pathRoot_, clipPaths_[ i ] );

        if( obj.isLane() ) {
            qWarning( "merge-columns: REFUSED — %s is a lane, not a clip.",
                      qPrintable( where ) );
            return { false, nullptr };
        }
        if( obj.isLiveRecording() ) {
            qWarning( "merge-columns: REFUSED — %s is still recording.",
                      qPrintable( where ) );
            return { false, nullptr };
        }
        STakeStack *col = stakes::columnOfLink( lk );
        if( col && !stakes::isDirectColumn( lk ) ) {
            // A WRAPPED column (SLink -> SCut -> STakeStack). Folding it would
            // either drop the wrapper's own window — its slip, stretch, loop
            // and pan, which is why normalizeColumns refuses to fold it — or
            // nest a column inside a column, which proposal 42 M3 forbids.
            qWarning( "merge-columns: REFUSED — %s is a WRAPPED take column; "
                      "unwrap it first (the wrapper's own window cannot become "
                      "a take).", qPrintable( where ) );
            return { false, nullptr };
        }
        if( !col && !SClipWindow::of( &obj ) ) {
            qWarning( "merge-columns: REFUSED — %s is neither a windowed clip "
                      "nor a take column.", qPrintable( where ) );
            return { false, nullptr };
        }
        if( col && col->nTakes() <= 0 ) {
            qWarning( "merge-columns: REFUSED — %s is an empty take column.",
                      qPrintable( where ) );
            return { false, nullptr };
        }
        // HOMOGENEITY, the same rule insertTake enforces per take: a column is
        // a set of alternatives for one region, so audio and events cannot
        // share one.
        const SContentKind k = obj.contentKind();
        if( !kindSeen ) { kind = k; kindSeen = true; }
        else if( k != kind ) {
            qWarning( "merge-columns: REFUSED — mixed content kinds; %s is "
                      "%s and an earlier operand is %s.", qPrintable( where ),
                      k == SContentKind::Event ? "events" : "audio",
                      kind == SContentKind::Event ? "events" : "audio" );
            return { false, nullptr };
        }
    }

    // ---- THE PARAMETER REFUSAL (see the header) ---------------------------
    //
    // A merged column has ONE set of clip parameters, read from its active
    // take, and the mix applies them across the whole column. Where the
    // operands disagree — or carry anything POSITIONAL, which a longer clip
    // re-bases — the merge cannot preserve what plays, and plan 53's
    // acceptance criterion for this row is that it must.
    {
        double v0 = 0.0, p0 = 0.0;
        bool first = true;
        for( int i = 0; i < links.size(); ++i ) {
            SObject *par = mixParamsOf( links[ i ]->getSObject() );
            const QString where = qualifiedToString( pathRoot_, clipPaths_[ i ] );
            if( !par ) continue;
            SAutomationLane *gain =
                par->automationLane( QStringLiteral( "cut:Gain" ) );
            if( gain && !gain->isEmpty() ) {
                qWarning( "merge-columns: REFUSED — %s carries a cut:Gain "
                          "envelope. A gain curve is POSITIONAL, so it would "
                          "mean something else in a longer clip; flatten it "
                          "first (plan 53 §6.1, QBX-185).", qPrintable( where ) );
                return { false, nullptr };
            }
            if( !par->clipFade().none() ) {
                qWarning( "merge-columns: REFUSED — %s carries a clip fade. A "
                          "fade's length is measured against the clip's own "
                          "end, so it would move once the clip is longer; "
                          "remove it first (plan 53 §6.1).",
                          qPrintable( where ) );
                return { false, nullptr };
            }
            if( first ) { v0 = par->getVolume(); p0 = par->getPan(); first = false; }
            else if( par->getVolume() != v0 || par->getPan() != p0 ) {
                qWarning( "merge-columns: REFUSED — %s plays at %.3f dB / pan "
                          "%.3f and an earlier operand at %.3f dB / pan %.3f. "
                          "A merged column has ONE level for every take "
                          "(QBX-185), so merging would change what plays.",
                          qPrintable( where ), par->getVolume(), par->getPan(),
                          v0, p0 );
                return { false, nullptr };
            }
        }
    }

    // THE LANE'S PATH, taken from an operand's path rather than searched for:
    // a clip path IS `lanePath + childIndex`, and the tree is
    // SObject -> SLink -> SObject with only the links pointing down, so there
    // is no upward walk to ask (the same fact that makes srecplan::snapshot
    // take its path as an argument).
    lanePath_ = clipPaths_[ 0 ];
    lanePath_.removeLast();

    // ---- the new column's geometry ----------------------------------------
    offset_t newStart = links[ 0 ]->getStartTime();
    offset_t maxEnd   = newStart;
    for( SLink *lk : links ) {
        const offset_t s = lk->getStartTime();
        const offset_t e = s + (offset_t) lk->getSObject().getDurationBlocking();
        if( s < newStart ) newStart = s;
        if( e > maxEnd )   maxEnd   = e;
    }

    // ---- capture the recipe, THEN move ------------------------------------
    //
    // Captured before anything moves: every number below is read off an object
    // the moves are about to change.
    for( int i = 0; i < links.size(); ++i ) {
        SLink *lk = links[ i ];
        Operand op;
        op.origIndex  = lane->indexOfChild( lk );
        op.origStart  = lk->getStartTime();
        op.origDur    = lk->getSObject().getDurationBlocking();
        op.timebase   = lk->getTimebase();
        op.startTicks = lk->getStartTicks();
        op.eventChannelOverride = lk->getEventChannelOverride();
        if( STakeStack *col = stakes::columnOfLink( lk ) ) {
            op.wasColumn    = true;
            op.column       = col;
            op.columnMap    = col->compMap();
            op.columnActive = col->activeTakeIndex();
            for( int t = 0; t < col->nTakes(); ++t ) {
                op.takeObjects.append( col->takeObjectAt( t ) );
                op.takeStarts.append( col->takeExtentStart( t ) );
                op.takeLens.append( col->takeExtentLength( t ) );
            }
        } else {
            SClipWindow *w = SClipWindow::of( &lk->getSObject() );
            op.takeObjects.append( &w->asObject() );
            op.takeStarts.append( 0 );
            op.takeLens.append( op.origDur );
        }
        operands_.append( op );
    }

    STakeStack *merged = new STakeStack( project );

    // THE COMBINED MAP, PAINTED IN OPERAND ORDER (§6.1: "the LATER clip in the
    // list winning where two overlap"). Each operand paints its own answer
    // over its own extent and the previous answer RE-ENTERS at its end, which
    // is the same shape `select-take` uses for an extent (§4.3) — one way of
    // saying "this, here, and what was there before everywhere else".
    std::vector<twCompSegment> segs;
    const auto answerAt = [&segs]( offset_t pos ) -> int {
        int found = -1;
        for( const twCompSegment &s : segs )
            if( (offset_t) s.at <= pos ) found = s.take; else break;
        return found;
    };

    int activeOut = -1;
    for( int i = 0; i < operands_.size(); ++i ) {
        Operand &op = operands_[ i ];
        const offset_t shift  = op.origStart - newStart;
        const offset_t opEnd  = shift + (offset_t) op.origDur;
        const int      base   = merged->nTakes();

        // What was in force at this operand's end BEFORE it paints; it has to
        // resume there, or this operand's answer would run on past its own
        // material.
        const int prevAtEnd = answerAt( opEnd );

        // ---- the takes move, SAME OBJECTS --------------------------------
        QList<int> newIndexOf;              // this operand's take j -> merged
        if( op.wasColumn ) {
            STakeStack *col = op.column;
            // The column object is PINNED for the inverse (see the header):
            // an STakeStack is an SObject and carries its own volume, pan,
            // mute, solo and recordingChannels, which no recipe here records.
            col->addRef();
            op.holdsColumnRef = true;
            for( int j = 0; j < op.takeObjects.size(); ++j ) {
                // ALWAYS INDEX 0: removeTake shifts the rest down, so taking
                // the front repeatedly walks the column in its own take order
                // without any index arithmetic to get wrong.
                if( !stakes::moveTakeBetweenColumns(
                        *col, 0, *merged, -1,
                        op.takeStarts[ j ] + shift, op.takeLens[ j ] ) ) {
                    newIndexOf.append( -1 );
                    continue;
                }
                newIndexOf.append( merged->nTakes() - 1 );
            }
        } else {
            SClipWindow *w = SClipWindow::of( &links[ i ]->getSObject() );
            SLink *tl = w ? merged->insertTake( *w, -1, shift, op.origDur )
                          : nullptr;
            if( !tl ) {
                qWarning( "merge-columns: REFUSED — %s could not become a "
                          "take.", qPrintable(
                              qualifiedToString( pathRoot_, clipPaths_[ i ] ) ) );
                // Nothing is published yet and the operand links are all still
                // on the lane; the half-built column is dropped.
                releasePins();
                delete merged;
                operands_.clear();
                return { false, nullptr };
            }
            newIndexOf.append( merged->nTakes() - 1 );
        }

        // ---- ...and its answer paints over its extent ---------------------
        std::vector<twCompSegment> kept;
        for( const twCompSegment &s : segs )
            if( (offset_t) s.at < shift || (offset_t) s.at >= opEnd )
                kept.push_back( s );

        if( op.wasColumn && !op.columnMap.empty() ) {
            // Its own map, shifted into the merged column's domain and
            // re-indexed onto the new take numbering. A segment before the
            // extent's start is pulled UP to it: the material it selected
            // begins here, not where the operand's own domain began.
            bool opened = false;
            for( const twCompSegment &s : op.columnMap.segments() ) {
                const offset_t at = (offset_t) s.at + shift;
                if( at >= opEnd ) continue;
                const int t = ( s.take >= 0 && s.take < newIndexOf.size() )
                                  ? newIndexOf[ s.take ] : -1;
                if( t < 0 ) continue;
                if( at <= shift ) {
                    kept.push_back( twCompSegment{ (int64_t) shift, t, 0 } );
                    opened = true;
                } else {
                    kept.push_back( twCompSegment{ (int64_t) at, t, s.xfade } );
                }
            }
            if( !opened ) {
                // Before its first segment the operand answered its own
                // activeTake_, which the merged column's activeTake_ will not
                // be. Spell it out.
                const int a = ( op.columnActive >= 0
                                && op.columnActive < newIndexOf.size() )
                                  ? newIndexOf[ op.columnActive ] : -1;
                if( a >= 0 )
                    kept.push_back( twCompSegment{ (int64_t) shift, a, 0 } );
            }
        } else {
            // A plain clip, or a column with no map: ONE answer over the whole
            // extent — the take it became, or its active take.
            int a = -1;
            if( op.wasColumn ) {
                a = ( op.columnActive >= 0 && op.columnActive < newIndexOf.size() )
                        ? newIndexOf[ op.columnActive ] : -1;
                if( a < 0 && !newIndexOf.isEmpty() ) a = newIndexOf[ 0 ];
            } else if( !newIndexOf.isEmpty() ) {
                a = newIndexOf[ 0 ];
            }
            if( a >= 0 )
                kept.push_back( twCompSegment{ (int64_t) shift, a, 0 } );
        }

        // ...and the previous answer resumes at this operand's end.
        if( opEnd < (offset_t) ( maxEnd - newStart ) && prevAtEnd >= 0 )
            kept.push_back( twCompSegment{ (int64_t) opEnd, prevAtEnd, 0 } );

        segs = twCompMap( std::move( kept ) ).normalized().segments();

        // The LAST operand's active take is the merged column's (§6.1).
        if( op.wasColumn ) {
            activeOut = ( op.columnActive >= 0
                          && op.columnActive < newIndexOf.size() )
                            ? newIndexOf[ op.columnActive ] : activeOut;
        } else if( !newIndexOf.isEmpty() && newIndexOf[ 0 ] >= 0 ) {
            activeOut = newIndexOf[ 0 ];
        }
        (void) base;
    }

    if( merged->nTakes() <= 0 ) {
        releasePins();
        delete merged;
        operands_.clear();
        qWarning( "merge-columns: REFUSED — no operand produced a take." );
        return { false, nullptr };
    }

    merged->setCompMap( twCompMap( std::move( segs ) ) );
    merged->setActiveTake( activeOut >= 0 ? activeOut : 0 );
    // THE COLUMN'S OWN LENGTH, not max_i( tEnd_i ): an operand may have been
    // LONGER than its takes reach (§4.2's one-take placeholder), and the merged
    // extent is [min start, max end) over the OPERANDS, not over their takes.
    merged->setColumnDuration( (length_t) ( maxEnd - newStart ) );

    // ---- swap the lane's children ----------------------------------------
    //
    // The new link is parented only AFTER every window it needs is already
    // referenced by the merged column (above), so no operand's object is ever
    // unreferenced: `removeRef()`'s deleteLater() cannot be rescinded.
    const int firstIndex = operands_[ 0 ].origIndex;
    // DESCENDING, so no removal invalidates an index still to be used.
    QList<int> order;
    for( int i = 0; i < links.size(); ++i ) order.append( i );
    std::sort( order.begin(), order.end(), [&]( int a, int b ) {
        return operands_[ a ].origIndex > operands_[ b ].origIndex;
    } );
    for( int i : order ) {
        // The broad disconnect form (plan 53 B1): the lane wired
        // durationChanged AND eventsChanged on the object, and
        // trackChildWasRemoved severs neither.
        QObject::disconnect( &links[ i ]->getSObject(), nullptr, lane, nullptr );
        delete links[ i ];
    }

    SLink *mergedLink = new SLink( *merged, nullptr );
    carryLaneLinkAttrs( operands_[ 0 ], mergedLink );
    mergedLink->setStartTime( newStart );
    mergedLink->setParent( lane );
    lane->moveChildToIndex( lane->indexOfChild( mergedLink ),
                            std::min( firstIndex, lane->childCount() - 1 ) );
    stakes::publishColumnChange( mergedLink, merged );

    return { true, new SUnmergeColumnsAction( this ) };
}

void SMergeColumnsAction::writeXml( QDomElement &elem ) const
{
    QStringList parts;
    for( const QList<int> &p : clipPaths_ )
        parts << qualifiedToString( pathRoot_, p );
    elem.setAttribute( "clips", parts.join( ';' ) );
}

bool SMergeColumnsAction::readXml( const QDomElement &elem, int /*version*/ )
{
    clipPaths_.clear();
    const QString all = elem.attribute( "clips" );
    // An empty list is reported by apply(), not here: a readXml that fails
    // makes the action undeserializable and breaks the round-trip audit, which
    // feeds every verb a DEFAULT instance through write->read->write.
    for( const QString &one : all.split( ';', Qt::SkipEmptyParts ) )
        clipPaths_.append( parseInto( pathRoot_, one.trimmed() ) );
    return true;
}

// ---------------------------------------------------------------------------
// unmerge-columns
// ---------------------------------------------------------------------------

SUnmergeColumnsAction::SUnmergeColumnsAction( SMergeColumnsAction *owner )
    : owner_( owner )
{
}

SApplyResult SUnmergeColumnsAction::apply( SProject *project )
{
    if( !project || !owner_ ) return { false, nullptr };
    const QList<SMergeColumnsAction::Operand> &ops = owner_->operands();
    if( ops.isEmpty() ) return { false, nullptr };

    SObject *root = splacements::rootNamed( project, owner_->pathRoot() );
    SObject *lane = splacements::containerAt( root, owner_->lanePath() );
    if( !lane ) return { false, nullptr };

    // The merged column sits at the first operand's index — where apply() put
    // it. Found by OBJECT rather than trusted by index: anything the user did
    // between the merge and this undo could have moved it.
    SLink      *mergedLink = nullptr;
    STakeStack *merged     = nullptr;
    for( SLink *lk : lane->childLinks() ) {
        STakeStack *c = stakes::columnOfLink( lk );
        if( !c ) continue;
        bool holdsAll = true;
        for( const SMergeColumnsAction::Operand &op : ops )
            for( SObject *o : op.takeObjects )
                if( takeIndexOfObject( *c, o ) < 0 ) { holdsAll = false; break; }
        if( holdsAll ) { mergedLink = lk; merged = c; break; }
    }
    if( !merged ) {
        qWarning( "unmerge-columns: the merged column is no longer on the "
                  "lane, or no longer holds every take the merge moved into "
                  "it; refusing rather than guessing." );
        return { false, nullptr };
    }

    // ---- put every operand back, ASCENDING original index ------------------
    QList<int> order;
    for( int i = 0; i < ops.size(); ++i ) order.append( i );
    std::sort( order.begin(), order.end(), [&]( int a, int b ) {
        return ops[ a ].origIndex < ops[ b ].origIndex;
    } );

    QList<SLink *> restored;
    for( int i : order ) {
        const SMergeColumnsAction::Operand &op = ops[ i ];
        SLink *newLink = nullptr;

        if( op.wasColumn && op.column ) {
            STakeStack *col = op.column;
            for( int j = 0; j < op.takeObjects.size(); ++j ) {
                const int at = takeIndexOfObject( *merged, op.takeObjects[ j ] );
                if( at < 0 ) continue;
                stakes::moveTakeBetweenColumns( *merged, at, *col, -1,
                                                op.takeStarts[ j ],
                                                op.takeLens[ j ] );
            }
            col->setCompMap( op.columnMap );
            col->setActiveTake( op.columnActive );
            col->setColumnDuration( op.origDur );
            newLink = new SLink( *col, nullptr );
        } else {
            SObject *obj = op.takeObjects.isEmpty() ? nullptr
                                                    : op.takeObjects[ 0 ];
            const int at = obj ? takeIndexOfObject( *merged, obj ) : -1;
            if( at < 0 || !obj ) continue;
            SClipWindow *w = SClipWindow::of( obj );
            if( !w ) continue;
            // THE PIN, exactly as moveTakeBetweenColumns does it: removeTake
            // releases the only reference the merged column held, and
            // removeRef()'s deleteLater() cannot be rescinded once posted.
            SLink *pin = new SLink( *obj, nullptr );
            QObject::disconnect( obj, nullptr, merged, nullptr );
            merged->removeTake( at );
            (void) w;
            newLink = new SLink( *obj, nullptr );
            delete pin;
        }
        if( !newLink ) continue;
        carryLaneLinkAttrs( op, newLink );
        newLink->setParent( lane );
        restored.append( newLink );
        lane->moveChildToIndex( lane->indexOfChild( newLink ),
                                std::min( op.origIndex,
                                          lane->childCount() - 1 ) );
    }

    // ---- the merged column goes ------------------------------------------
    SObject *mergedObj = &mergedLink->getSObject();
    QObject::disconnect( mergedObj, nullptr, lane, nullptr );
    delete mergedLink;
    if( mergedObj->refCount() <= 0 ) delete mergedObj;

    // ...and the pins are released only now that every column is re-linked.
    owner_->releasePins();

    for( SLink *lk : restored ) {
        if( STakeStack *c = stakes::columnOfLink( lk ) )
            stakes::publishColumnChange( lk, c );
    }
    return { true, nullptr };
}

void SUnmergeColumnsAction::writeXml( QDomElement & ) const
{
    // LIVE-ONLY: it carries a pointer to a live action and live SObjects, which
    // no XML can express (SRestoreTakeColumnAction, same reason).
}

bool SUnmergeColumnsAction::readXml( const QDomElement &, int )
{
    return false;
}

static const bool s_reg_merge_columns = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "merge-columns" ),
        []{ return new SMergeColumnsAction; } ),
    true );
