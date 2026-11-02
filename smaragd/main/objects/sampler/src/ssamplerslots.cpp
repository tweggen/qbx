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

bool reachesRec( SObject *from, const SObject *to, QSet<const SObject *> &seen, QStringList &via )
{
    if( !from ) return false;
    if( from == to ) return true;
    if( seen.contains( from ) ) return false;   // shared, or (already) cyclic
    seen.insert( from );
    for( SLink *lk : from->childLinks() ) {
        if( lk && reachesRec( &lk->getSObject(), to, seen, via ) ) return true;
    }
    // A lane's instrument is not a child link: follow its sampler's asset links.
    if( from->isLane() && laneProgramsSlot() ) {
        for( SSamplerProgram *p : laneProgramsSlot()( from ) ) {
            if( !p ) continue;
            for( SObject *body : p->liveLinkTargets() ) {
                if( reachesRec( body, to, seen, via ) ) {
                    SProject *proj = body->getProjectSafe();
                    const QString n = proj ? proj->assetNameOf( body ) : QString();
                    via.prepend( QStringLiteral( "the sampler on \"%1\" links \"%2\"" )
                                     .arg( from->getSName(), n.isEmpty() ? body->getSName() : n ) );
                    return true;
                }
            }
        }
    }
    return false;
}
}  // namespace

void setLaneProgramsHook( LanePrograms hook ) { laneProgramsSlot() = std::move( hook ); }

bool assetReachesTrack( SObject *body, const SObject *track, QString *via )
{
    if( via ) via->clear();
    if( !body || !track ) return false;
    QSet<const SObject *> seen;
    QStringList hops;
    const bool r = reachesRec( body, track, seen, hops );
    if( r && via ) *via = hops.join( QStringLiteral( ", " ) );
    return r;
}

}  // namespace ssampler
