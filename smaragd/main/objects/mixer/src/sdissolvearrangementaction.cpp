#include "app/objects/mixer/sdissolvearrangementaction.h"
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

    // --- PIN what the inverse restores, BEFORE anything is torn down --------
    // QBX-284: the undo re-registers THESE objects (see the header). Each pin
    // is taken before the reference it protects is dropped, so no refcount
    // passes through zero (a 1->0 transition is a deleteLater that cannot be
    // rescinded).
    dropPins();
    if( pinsForUndo_ ) {
        held_.root = arrRoot;
        arrRoot->addRef();
        for( const QString &an : ownedAssets ) {
            SObject *body = project->asset( an );
            body->addRef();
            held_.assets.append( qMakePair( an, body ) );
        }
        for( STrack *tk : moving ) held_.tracks.append( QPointer<STrack>( tk ) );
        held_.plan = project->prop( planKey );   // restored as it was, unset included
        holds_ = true;
    }

    // --- drop the placements, then the assets ------------------------------
    for( SLink *lk : placements ) {
        // The lane the placement sat on was created BY the extraction and holds
        // nothing else; it goes with the placement. A lane the user has since
        // put other clips on is kept.
        SObject *lane = dynamic_cast<SObject *>( lk->parent() );
        // The placement is rebuilt by the undo from EVERY field an SLink
        // carries (remove-asset-placement's recipe, widened to the ticks, the
        // timebase and the channel override), at its own index on its lane.
        if( holds_ ) {
            held_.placed = true;
            held_.placedBody = &lk->getSObject();
            held_.placementLane = lane;
            held_.placementIndex = lane ? lane->indexOfChild( lk ) : -1;
            held_.startTime = lk->getStartTime();
            held_.startTicks = lk->getStartTicks();
            held_.timebase = lk->getTimebase();
            held_.eventChannelOverride = lk->getEventChannelOverride();
        }
        delete lk;
        if( STrack *laneTrack = dynamic_cast<STrack *>( lane ) ) {
            if( laneTrack->childCount() == 0 ) {
                for( int i = 0; i < masterMixer->getNTracks(); ++i ) {
                    if( &masterMixer->getTrackAt( i )->getSObject()
                        == static_cast<SObject *>( laneTrack ) ) {
                        // Pinned, so the undo puts back THIS lane, with
                        // whatever is on it (inserts, fader, automation).
                        if( holds_ ) {
                            laneTrack->addRef();
                            held_.lane = laneTrack;
                            held_.laneRemoved = true;
                            held_.laneIndex = i;
                        }
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
    // and it may have been the selected one. Its QPointer would not clear
    // (the lane is pinned for the undo now, and even unpinned it would only
    // be deleteLater'd), so prune over the settled tree for the same reason
    // the extraction does. The arrangement root's own selection entry needs
    // no prune: the root is in no tree any more. QBX-232.
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

    // As extract-arrangement's own inverse this dissolve keeps nothing: that
    // command's redo re-extracts, so a pin here would only keep the dead
    // root alive until the command is discarded. See setPinsForUndo().
    if( !holds_ ) return { true, nullptr };
    return { true, new SRestoreDissolvedArrangementAction( this, arrName_, restorePlan_ ) };
}

SDissolveArrangementAction::~SDissolveArrangementAction()
{
    dropPins();
}

void SDissolveArrangementAction::dropPins()
{
    if( holds_ ) {
        // Assets first: each windows the root, so the root outlives them.
        for( const auto &a : held_.assets ) if( a.second ) a.second->removeRef();
        if( held_.root ) held_.root->removeRef();
        if( held_.laneRemoved && held_.lane ) held_.lane->removeRef();
    }
    held_ = Held();
    holds_ = false;
}

void SDissolveArrangementAction::releaseHeld()
{
    dropPins();
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

// --- restore-dissolved-arrangement ------------------------------------------

SRestoreDissolvedArrangementAction::SRestoreDissolvedArrangementAction(
        SDissolveArrangementAction *owner, const QString &name,
        const QString &restorePlan )
    : owner_( owner ), name_( name ), restorePlan_( restorePlan )
{
}

SApplyResult SRestoreDissolvedArrangementAction::apply( SProject *project )
{
    if( !project || !owner_ || !owner_->holdsPins() ) return { false, nullptr };
    const SDissolveArrangementAction::Held &h = owner_->held();
    // check_root_resolution: master -- the dissolve moved the tracks into the master; the undo takes them back out of it
    SStdMixer *masterMixer = dynamic_cast<SStdMixer *>( project->masterRoot() );
    SStdMixer *arrRoot = dynamic_cast<SStdMixer *>( h.root );
    if( !masterMixer || !arrRoot ) return { false, nullptr };

    // REFUSE BEFORE ANYTHING MOVES. A name that came back while we were
    // undone is not ours to overwrite, and a track that has gone cannot be
    // put back.
    if( project->hasArrangement( name_ ) ) {
        TW_LOGW( "cut", "restore-dissolved-arrangement: refused, name '%s' is in use",
                 name_.toUtf8().constData() );
        return { false, nullptr };
    }
    for( const auto &a : h.assets ) {
        if( project->hasAsset( a.first ) ) {
            TW_LOGW( "cut", "restore-dissolved-arrangement: refused, asset name "
                            "'%s' is in use", a.first.toUtf8().constData() );
            return { false, nullptr };
        }
    }
    QList<QPair<STrack *, QPair<SObject *, SLink *> > > found;
    for( const QPointer<STrack> &tp : h.tracks ) {
        STrack *tk = tp.data();
        SObject *parent = nullptr;
        SLink *ownLink = nullptr;
        std::function<bool(SObject *)> findLink = [&]( SObject *container ) -> bool {
            for( SLink *lk : container->childLinks() ) {
                if( !lk ) continue;
                if( &lk->getSObject() == static_cast<SObject *>( tk ) ) {
                    parent = container; ownLink = lk; return true;
                }
                SObject &child = lk->getSObject();
                if( child.isLane() && findLink( &child ) ) return true;
            }
            return false;
        };
        if( !tk || !findLink( masterMixer ) ) {
            TW_LOGW( "cut", "restore-dissolved-arrangement: refused, a track of '%s' "
                            "is no longer in the master", name_.toUtf8().constData() );
            return { false, nullptr };
        }
        found.append( qMakePair( tk, qMakePair( parent, ownLink ) ) );
    }
    SObject *placementLane = h.laneRemoved ? h.lane : h.placementLane.data();
    if( h.placed && !placementLane ) {
        TW_LOGW( "cut", "restore-dissolved-arrangement: refused, the lane that held "
                        "'%s''s placement is gone", name_.toUtf8().constData() );
        return { false, nullptr };
    }

    // --- the root and its assets, the ORIGINAL objects ----------------------
    project->registerArrangement( name_, arrRoot );
    for( const auto &a : h.assets ) project->registerAsset( a.first, a.second );

    // --- the tracks, out of the master and back into the root, in order -----
    for( const auto &f : found ) {
        STrack *tk = f.first;
        SObject *parent = f.second.first;
        SLink *ownLink = f.second.second;
        tk->addRef();                          // pin across the move
        if( SStdMixer *srcMixer = dynamic_cast<SStdMixer *>( parent ) ) {
            srcMixer->removeTrack( *ownLink );
            QObject::disconnect( tk, nullptr, srcMixer, nullptr );
        } else {
            delete ownLink;
            QObject::disconnect( tk, nullptr, parent, nullptr );
        }
        arrRoot->insertTrack( *tk );
        tk->removeRef();
    }

    // --- the asset lane, at its index, and the placement on it -------------
    // After the tracks left, the master is the pre-dissolve master less this
    // lane, so its recorded index means what it meant then.
    if( h.laneRemoved ) {
        masterMixer->insertTrack( *h.lane );
        const int landing = masterMixer->getNTracks() - 1;
        if( h.laneIndex >= 0 && h.laneIndex < landing )
            masterMixer->reorderTrack( landing, h.laneIndex );
    }
    if( h.placed ) {
        SLink *nl = new SLink( *h.placedBody, nullptr );
        nl->setTimebase( h.timebase );
        nl->setStartTime( h.startTime );
        if( h.timebase == SLink::Timebase::Beats ) nl->setStartTicks( h.startTicks );
        nl->setEventChannelOverride( h.eventChannelOverride );
        nl->setParent( placementLane );
        const int landing = placementLane->childCount() - 1;
        if( h.placementIndex >= 0 && h.placementIndex < landing )
            placementLane->moveChildToIndex( landing, h.placementIndex );
    }

    const QString planKey = QStringLiteral( "arrangement/restorePlan/" ) + name_;
    project->setProp( planKey, h.plan );

    // The registries and the master hold their own references now, so ours
    // go: holding both would make the arrangement undestroyable for the rest
    // of the session. A redo re-pins through the dissolve's apply().
    owner_->releaseHeld();

    masterMixer->pruneSelectionToTree();
    arrRoot->applyAudibility();
    arrRoot->notifyTreeChanged();
    arrRoot->bumpRenderChainEpoch();
    masterMixer->applyAudibility();
    masterMixer->notifyTreeChanged();
    masterMixer->bumpRenderChainEpoch();
    SAppContext::get().rewireSpeaker();

    TW_LOGI( "cut", "restore-dissolved-arrangement '%s': %d track(s) moved back",
             name_.toUtf8().constData(), (int) found.size() );

    return { true, new SDissolveArrangementAction( name_, restorePlan_ ) };
}

void SRestoreDissolvedArrangementAction::writeXml( QDomElement &elem ) const
{
    // Undo-stack only: it restores by pointer, so there is nothing portable
    // to write and nothing a script could re-create from a file.
    elem.setAttribute( "name", name_ );
}

bool SRestoreDissolvedArrangementAction::readXml( const QDomElement &elem, int )
{
    name_ = elem.attribute( "name" );
    return true;
}

static const bool s_reg_dissolvearrangement = (
    SActionRegistry::instance().registerType(
        QStringLiteral("dissolve-arrangement"),
        []{ return new SDissolveArrangementAction; }
    ), true
);
