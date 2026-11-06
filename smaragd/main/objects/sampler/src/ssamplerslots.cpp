#include "app/objects/sampler/ssamplerslots.h"

#include "app/model/slink.h"
#include "app/model/sobject.h"
#include "app/model/sproject.h"
#include "app/objects/sampler/ssamplerprogram.h"

#include <QSet>

namespace ssampler {

namespace {
Resolver &resolverSlot()
{
    static Resolver r;
    return r;
}
}  // namespace

void setResolver( Resolver resolver ) { resolverSlot() = std::move( resolver ); }

bool hasResolver() { return static_cast<bool>( resolverSlot() ); }

SSamplerProgram *programAt( SProject *project, const QString &pathRoot, const QString &trackPath,
                            int slotIndex, bool create, QString *why )
{
    if( !resolverSlot() ) {
        if( why )
            *why = QStringLiteral( "no sampler slot resolver is installed (the shell installs "
                                   "it at startup: ssamplerwiring::install)" );
        return nullptr;
    }
    if( !project ) {
        if( why ) *why = QStringLiteral( "no project" );
        return nullptr;
    }
    return resolverSlot()( project, pathRoot, trackPath, slotIndex, create, why );
}

namespace {
LanePrograms &laneProgramsSlot()
{
    static LanePrograms h;
    return h;
}

LaneRoutingSources &laneRoutingSlot()
{
    static LaneRoutingSources h;
    return h;
}

bool reachesRec( SObject *from, const SObject *to, QSet<const SObject *> &seen, QStringList &via,
                 const sarrangements::PendingEdges *pending, bool &samplerHop )
{
    if( !from ) return false;
    if( from == to ) return true;
    if( seen.contains( from ) ) return false;   // shared, or (already) cyclic
    seen.insert( from );
    for( SLink *lk : from->childLinks() ) {
        if( lk && reachesRec( &lk->getSObject(), to, seen, via, pending, samplerHop ) ) return true;
    }
    // Edges the asking edit is about to add (QBX-267), as if child links.
    if( pending ) {
        for( const auto &e : *pending ) {
            if( e.first == from && reachesRec( e.second, to, seen, via, pending, samplerHop ) ) return true;
        }
    }
    // A lane's instrument is not a child link: follow its sampler's asset links.
    if( from->isLane() && laneProgramsSlot() ) {
        for( SSamplerProgram *p : laneProgramsSlot()( from ) ) {
            if( !p ) continue;
            for( SObject *body : p->liveLinkTargets() ) {
                if( reachesRec( body, to, seen, via, pending, samplerHop ) ) {
                    samplerHop = true;
                    SProject *proj = body->getProjectSafe();
                    const QString n = proj ? proj->assetNameOf( body ) : QString();
                    via.prepend( QStringLiteral( "the sampler on \"%1\" links \"%2\"" )
                                     .arg( from->getSName(), n.isEmpty() ? body->getSName() : n ) );
                    return true;
                }
            }
        }
    }
    // A lane's output may be ROUTED from a lane it does not contain (QBX-269):
    // a return's source, a keyed slot's key lane, a send lane's senders. The
    // model's edges are followed whether or not the mixer's pass wires them
    // today (a source outside the arrangement is unwired, and comes back the
    // moment a move brings it home), so a check never trusts a wiring state a
    // later edit can change.
    if( from->isLane() && laneRoutingSlot() ) {
        for( const RoutingSource &r : laneRoutingSlot()( from ) ) {
            if( r.source && reachesRec( r.source, to, seen, via, pending, samplerHop ) ) {
                via.prepend( r.hop );
                return true;
            }
        }
    }
    return false;
}
}  // namespace

void setLaneProgramsHook( LanePrograms hook ) { laneProgramsSlot() = std::move( hook ); }

void setLaneRoutingSourcesHook( LaneRoutingSources hook )
{
    laneRoutingSlot() = std::move( hook );
}

bool assetReachesTrack( SObject *body, const SObject *track, QString *via,
                        const sarrangements::PendingEdges *pending, bool *samplerHop )
{
    if( via ) via->clear();
    if( samplerHop ) *samplerHop = false;
    if( !body || !track ) return false;
    QSet<const SObject *> seen;
    QStringList hops;
    bool sampler = false;
    const bool r = reachesRec( body, track, seen, hops, pending, sampler );
    if( r && via ) *via = hops.join( QStringLiteral( ", " ) );
    if( r && samplerHop ) *samplerHop = sampler;
    return r;
}

namespace {
bool hearsRec( SObject *from, const SObject *to, QSet<const SObject *> &seen )
{
    if( !from ) return false;
    if( from == to ) return true;
    if( seen.contains( from ) ) return false;
    seen.insert( from );
    for( SLink *lk : from->childLinks() )
        if( lk && hearsRec( &lk->getSObject(), to, seen ) ) return true;
    // QBX-285: what the lane's output is routed FROM is frozen with it.
    if( from->isLane() && laneRoutingSlot() )
        for( const RoutingSource &r : laneRoutingSlot()( from ) )
            if( r.source && hearsRec( r.source, to, seen ) ) return true;
    return false;
}
}  // namespace

bool assetHears( SObject *body, const SObject *track )
{
    if( !body || !track ) return false;
    QSet<const SObject *> seen;
    return hearsRec( body, track, seen );
}

namespace {
LiveOwner &liveOwnerSlot()
{
    static LiveOwner h;
    return h;
}
}  // namespace

void setLiveOwnerHook( LiveOwner hook ) { liveOwnerSlot() = std::move( hook ); }

QString liveOwnerOf( SObject *body )
{
    if( !body || !liveOwnerSlot() ) return QString();
    return liveOwnerSlot()( body );
}

void liveOwnersMayHaveChanged()
{
    SSamplerProgram::recheckWaitingEverywhere();
}

}  // namespace ssampler
