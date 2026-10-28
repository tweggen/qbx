#include "app/objects/sampler/ssamplerslots.h"

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

}  // namespace ssampler
