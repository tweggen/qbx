#include "app/objects/mixer/sdissolvearrangementaction.h"
#include "app/objects/mixer/sextractarrangementaction.h"
#include "app/objects/mixer/sstdmixer.h"
#include "app/objects/track/strack.h"
#include "app/objects/track/strackpath.h"
#include "app/objects/cut/scut.h"
#include "app/model/sproject.h"
#include "app/model/splacements.h"
#include "app/model/sarrangements.h"
#include "app/model/sappcontext.h"
#include "app/actions/sactionregistry.h"
#include "tw/core/twlog.h"
#include <QDomElement>
#include <QHash>
#include <functional>

using namespace strackpath;

SDissolveArrangementAction::SDissolveArrangementAction( const QString &name,
                                                        const QString &restorePlan )
    : arrName_( name ), restorePlan_( restorePlan )
{
}

SApplyResult SDissolveArrangementAction::apply( SProject *project )
{
    if( !project || arrName_.isEmpty() ) return { false, nullptr };

    // check_root_resolution: master -- dissolve returns tracks TO the master; that is where they go by definition
    SObject *master = project->masterRoot();
    SStdMixer *masterMixer = dynamic_cast<SStdMixer *>( master );
    SObject *arrObj = project->arrangement( arrName_ );
    SStdMixer *arrRoot = dynamic_cast<SStdMixer *>( arrObj );
    if( !masterMixer || !arrRoot ) {
        TW_LOGW( "cut", "dissolve-arrangement: no arrangement named '%s'",
                 arrName_.toUtf8().constData() );
        return { false, nullptr };
    }

    // --- every asset over this root, and every placement of one -------------
    QStringList ownedAssets;
    for( const QString &an : project->assetNames() ) {
        SCut *c = dynamic_cast<SCut *>( project->asset( an ) );
        if( c && &c->getContent() == static_cast<SObject *>( arrRoot ) )
            ownedAssets << an;
    }

    // Collect the placement links in the master tree, per asset.
    QList<SLink *> placements;
    std::function<void(SObject *)> findPlacements = [&]( SObject *container ) {
        for( SLink *lk : container->childLinks() ) {
            if( !lk ) continue;
            SObject &child = lk->getSObject();
            if( !project->assetNameOf( &child ).isEmpty()
                && ownedAssets.contains( project->assetNameOf( &child ) ) ) {
                placements.append( lk );
                continue;
            }
            if( child.isLane() ) findPlacements( &child );
        }
    };
    findPlacements( master );

    // REFUSED above one placement: see the header. An inverse can never hit
    // this (extract makes exactly one), but a user who placed the asset again
    // and then dissolved would get one restored and the rest deleted.
    if( placements.size() > 1 ) {
        TW_LOGW( "cut", "dissolve-arrangement: refused, '%s' has %d placements; "
                        "remove the extra ones first",
                 arrName_.toUtf8().constData(), (int) placements.size() );
        return { false, nullptr };
    }

    // THE PLAN, read before anything is torn down (the cycle guard below asks
    // where each track will land). An UNDO carries the plan in its own
    // parameters. A user-invoked dissolve does not, so fall back to the one
    // the extraction recorded on the project (which is serialized with it).
    // With neither, the tracks are appended -- the honest answer when nothing
    // recorded where they used to be.
    QString planStr = restorePlan_;
    const QString planKey = QStringLiteral( "arrangement/restorePlan/" ) + arrName_;
    if( planStr.isEmpty() ) planStr = project->prop( planKey ).toString();
    const QStringList plan = planStr.split( ';', Qt::SkipEmptyParts );
    QList<STrack *> moving;
    for( SLink *lk : arrRoot->childLinks() ) {
        if( STrack *tk = dynamic_cast<STrack *>( &lk->getSObject() ) )
            moving.append( tk );
    }

    // QBX-267: THE CYCLE GUARD. Each track lands under a master container it
    // may never have been reachable from -- an UNPLACED arrangement's tracks
    // are reachable from nothing in the master, and a plan may name a folder
    // -- so a NassauDigital sampler on one of them that links an asset over
    // that container (proposal 56 Q3; linking it was no cycle while the track
    // sat in the arrangement) would render into its own sample. Asked with the
    // check set-layer makes a link under, over the graph the dissolve LEAVES:
    // every move is a pending edge, so two moves that close a cycle only
    // together are refused too. The edges the teardown removes are not
    // subtracted, and need not be: a path through them back into a moving
    // track would be a cycle the project already had.
    //
    // WHERE each track lands is decided HERE, before anything moves, and the
    // move loop below uses these answers rather than resolving the plan again:
    // the plan's paths are resolved against the tree as it is AT THAT TRACK'S
    // TURN -- after the teardown removed the placement's lane and after the
    // earlier tracks went back -- so a precheck resolving them against today's
    // tree could ask about a different container than the one the track then
    // lands in. The tree is replayed on virtual child lists instead (the same
    // append-then-reorder the loop performs), and the guard and the loop read
    // one answer.
    struct Landing { SObject *dest; int destIndex; };
    QList<Landing> landings;
    {
        QHash<const SObject *, QList<SObject *> > vkids;
        auto kids = [&]( SObject *o ) -> QList<SObject *> & {
            auto it = vkids.find( o );
            if( it == vkids.end() ) {
                QList<SObject *> l;
                for( SLink *lk : o->childLinks() ) l.append( lk ? &lk->getSObject() : nullptr );
                it = vkids.insert( o, l );
            }
            return *it;
        };
        auto resolve = [&]( const QList<int> &path ) -> SObject * {
            SObject *cur = master;
            for( int idx : path ) {
                if( !cur || idx == SPATH_INVALID ) return nullptr;
                if( spathIsSystemStep( idx ) ) { cur = cur->systemLaneAt( idx ); continue; }
                QList<SObject *> &l = kids( cur );
                if( idx < 0 || idx >= l.size() ) return nullptr;
                cur = l.at( idx );
            }
            return cur;
        };
        // The teardown: the placement goes, and its lane with it when that
        // leaves the lane empty and the lane is a top-level master lane.
        for( SLink *lk : placements ) {
            SObject *lane = dynamic_cast<SObject *>( lk->parent() );
            if( !lane ) continue;
            QList<SObject *> &l = kids( lane );
            const int at = lane->indexOfChild( lk );
            if( at >= 0 && at < l.size() ) l.removeAt( at );
            if( dynamic_cast<STrack *>( lane ) && l.isEmpty() ) kids( master ).removeOne( lane );
        }
        sarrangements::PendingEdges pending;
        for( int i = 0; i < moving.size(); ++i ) {
            SObject *dest = master;
            int destIndex = -1;
            if( i < plan.size() ) {
                const QString entry = plan.at( i );
                const int at = entry.lastIndexOf( QLatin1Char( '@' ) );
                if( at >= 0 ) {
                    SObject *d = resolve( stringToPath( entry.left( at ) ) );
                    if( d && d->isLane() ) dest = d;
                    destIndex = entry.mid( at + 1 ).toInt();
                }
            }
            QList<SObject *> &l = kids( dest );
            l.append( moving.at( i ) );
            const int landing = l.size() - 1;
            if( destIndex >= 0 && destIndex < landing ) l.move( landing, destIndex );
            landings.append( { dest, destIndex } );
            pending.append( qMakePair( static_cast<const SObject *>( dest ),
                                       static_cast<SObject *>( moving.at( i ) ) ) );
        }
        for( const auto &e : pending ) {
            QString via;
            if( sarrangements::placementWouldCycle( e.second, const_cast<SObject *>( e.first ),
                                                    &via, &pending ) ) {
                if( !via.isEmpty() ) {
                    TW_LOGW( "cut", "dissolve-arrangement: refused, moving '%s' into '%s' "
                                    "would close a sampler asset-link cycle (%s)",
                             e.second->getSName().toUtf8().constData(),
                             e.first->getSName().toUtf8().constData(),
                             via.toUtf8().constData() );
                } else {
                    TW_LOGW( "cut", "dissolve-arrangement: refused, moving '%s' into '%s' "
                                    "would close a reference cycle",
                             e.second->getSName().toUtf8().constData(),
                             e.first->getSName().toUtf8().constData() );
                }
                return { false, nullptr };
            }
        }
    }

    // Capture what the inverse needs BEFORE anything is torn down.
    offset_t winBegin = 0, winEnd = 0, placedAt = 0;
    if( SCut *primary = dynamic_cast<SCut *>( project->asset( arrName_ ) ) ) {
        winBegin = (offset_t) primary->getStartOffset().frames();
        winEnd   = winBegin + (offset_t) primary->getDurationBlocking();
    }
    if( !placements.isEmpty() && placements.first()->hasStartTime() )
        placedAt = placements.first()->getStartTime();

    // --- drop the placements, then the assets ------------------------------
    for( SLink *lk : placements ) {
        // The lane the placement sat on was created BY the extraction and holds
        // nothing else; it goes with the placement. A lane the user has since
        // put other clips on is kept.
        SObject *lane = dynamic_cast<SObject *>( lk->parent() );
        delete lk;
        if( STrack *laneTrack = dynamic_cast<STrack *>( lane ) ) {
            if( laneTrack->childCount() == 0 ) {
                for( int i = 0; i < masterMixer->getNTracks(); ++i ) {
                    if( &masterMixer->getTrackAt( i )->getSObject()
                        == static_cast<SObject *>( laneTrack ) ) {
                        masterMixer->removeTrack( i );
                        break;
                    }
                }
            }
        }
    }
    for( const QString &an : ownedAssets ) project->unregisterAsset( an );

    // --- move the tracks back ----------------------------------------------
    // Where each one lands was decided before the teardown (`landings`, see
    // the cycle guard): an UNDO carries the plan in its own parameters, a user
    // dissolve falls back to the one the extraction recorded, and with neither
    // the tracks are appended.

    for( int i = 0; i < moving.size(); ++i ) {
        STrack *tk = moving.at( i );
        SObject *dest = landings.at( i ).dest;
        const int destIndex = landings.at( i ).destIndex;

        SLink *ownLink = nullptr;
        for( SLink *lk : arrRoot->childLinks() ) {
            if( &lk->getSObject() == static_cast<SObject *>( tk ) ) { ownLink = lk; break; }
        }
        if( !ownLink ) continue;

        tk->addRef();                              // pin across the move
        arrRoot->removeTrack( *ownLink );
        QObject::disconnect( tk, nullptr, arrRoot, nullptr );
        if( SStdMixer *destMixer = dynamic_cast<SStdMixer *>( dest ) ) {
            destMixer->insertTrack( *tk );
            const int landing = destMixer->getNTracks() - 1;
            if( destIndex >= 0 && destIndex < landing )
                destMixer->reorderTrack( landing, destIndex );
        } else {
            SLink *nl = new SLink( *tk, nullptr );
            nl->setParent( dest );
            const int landing = dest->childCount() - 1;
            if( destIndex >= 0 && destIndex < landing )
                dest->moveChildToIndex( landing, destIndex );
        }
        tk->removeRef();
    }

    // --- retire the root ---------------------------------------------------
    project->unregisterArrangement( arrName_ );
    project->setProp( planKey, QVariant() );   // the plan died with the root

    // The ASSET LANE was removed from the master above once it had emptied,
    // and it may have been the selected one. Its QPointer would clear itself
    // eventually (nothing re-refs it, so it is deleteLater'd) but not before
    // the next read, so prune over the settled tree for the same reason the
    // extraction does. The arrangement root's own selection entry needs no
    // prune: it dies with the root. QBX-232.

    // The ASSET LANE was removed from the master above once it had emptied,
    // and it may have been the selected one. Its QPointer would clear itself
    // eventually (nothing re-refs it, so it is deleteLater'd) but not before
    // the next read, so prune over the settled tree for the same reason the
    // extraction does. The arrangement root's own selection entry needs no
    // prune: it dies with the root. QBX-232.
    masterMixer->pruneSelectionToTree();

    // Same reason as the extraction's: the master's summed pages predate the
    // tracks coming back.
    masterMixer->applyAudibility();
    masterMixer->notifyTreeChanged();
    masterMixer->bumpRenderChainEpoch();
    SAppContext::get().rewireSpeaker();

    TW_LOGI( "cut", "dissolve-arrangement '%s': %d track(s) restored, %d asset(s) "
                    "removed", arrName_.toUtf8().constData(),
             (int) moving.size(), (int) ownedAssets.size() );

    // The inverse re-extracts from where the tracks now are. Built from the
    // POST-move tree so it is immune to the index shifts this move caused.
    QStringList backPaths;
    for( STrack *tk : moving )
        backPaths << pathToString( pathOf( master, tk ) );

    SAction *inverse = new SExtractArrangementAction(
        backPaths.join( ';' ), winBegin, winEnd, arrName_,
        QStringLiteral( "range" ), placedAt );
    return { true, inverse };
}

void SDissolveArrangementAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "name", arrName_ );
    if( !restorePlan_.isEmpty() ) elem.setAttribute( "restorePlan", restorePlan_ );
}

bool SDissolveArrangementAction::readXml( const QDomElement &elem, int /*version*/ )
{
    arrName_     = elem.attribute( "name" );
    restorePlan_ = elem.attribute( "restorePlan" );
    return true;
}

static const bool s_reg_dissolvearrangement = (
    SActionRegistry::instance().registerType(
        QStringLiteral("dissolve-arrangement"),
        []{ return new SDissolveArrangementAction; }
    ), true
);
