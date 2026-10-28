#include "app/shell/ssamplerwiring.h"

#include "app/model/slink.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/objects/sampler/ssamplerprogram.h"
#include "app/objects/sampler/ssamplerslots.h"
#include "app/objects/track/spluginchain.h"
#include "app/objects/track/spluginslot.h"
#include "app/objects/track/strack.h"

#include "tw/plugins/twplugindescriptor.h"
#include "tw/sampler/twsamplerregistry.h"

#include <QDebug>
#include <QPointer>

namespace {

bool hostsNassauDigital( const SPluginSlot &slot )
{
    const audio::twPluginDescriptor &d = slot.getDescriptor();
    return d.format == "tw" && d.uid == audio::kNassauDigitalUid;
}

SSamplerProgram *programChild( const SPluginSlot &slot )
{
    for( SLink *lk : slot.childLinks() )
        if( SSamplerProgram *p = qobject_cast<SSamplerProgram *>( &lk->getSObject() ) ) return p;
    return nullptr;
}

void bind( SPluginSlot &slot, SSamplerProgram &prog )
{
    QPointer<SPluginSlot> sp( &slot );
    prog.setInstaller( [sp]( const SSamplerProgram::InstanceInitializer &init ) {
        if( !sp ) return;
        // Future instances first, then the ones that exist: a rebuild between
        // the two calls would otherwise build from the previous initializer.
        sp->setInstanceInitializer( init );
        // A slot with no processor yet (the load path, before the track
        // declares its width) has no instances: the initializer above is all
        // it needs, and nothing above it has rendered to be staled.
        if( sp->getProcessor() ) sp->applyToInstances( init );
    } );
}

SSamplerProgram *resolve( SProject *project, const QString &pathRoot, const QString &trackPath,
                          int slotIndex, bool create, QString *why )
{
    auto fail = [why]( const QString &w ) -> SSamplerProgram * {
        if( why ) *why = w;
        return nullptr;
    };
    QString root = pathRoot;
    STrack *track = dynamic_cast<STrack *>( splacements::laneBySpec( project, trackPath, root ) );
    if( !track ) return fail( QStringLiteral( "no track at trackPath \"%1\"" ).arg( trackPath ) );
    SPluginChain *chain = track->getPluginChain();
    SPluginSlot  *slot  = chain ? chain->getSlotAt( slotIndex ) : nullptr;
    if( !slot )
        return fail( QStringLiteral( "track \"%1\" has no slot %2" ).arg( trackPath ).arg( slotIndex ) );
    if( !hostsNassauDigital( *slot ) )
        return fail( QStringLiteral( "slot %1 on track \"%2\" does not host NassauDigital (it is "
                                     "%3:%4)" )
                         .arg( slotIndex ).arg( trackPath )
                         .arg( QString::fromStdString( slot->getDescriptor().format ),
                               QString::fromStdString( slot->getDescriptor().uid ) ) );
    if( SSamplerProgram *p = programChild( *slot ) ) return p;
    if( !create )
        return fail( QStringLiteral( "slot %1 on track \"%2\" has no program yet (add a zone first)" )
                         .arg( slotIndex ).arg( trackPath ) );
    SSamplerProgram *p = new SSamplerProgram( project );
    slot->adoptChild( *p );   // -> the child-adopted hook below binds it
    return p;
}

}  // namespace

namespace ssamplerwiring {

void install()
{
    ssampler::setResolver( resolve );
    SPluginSlot::setChildAdoptedHook( []( SPluginSlot &slot, SObject &child ) {
        SSamplerProgram *prog = qobject_cast<SSamplerProgram *>( &child );
        if( !prog ) return;
        if( !hostsNassauDigital( slot ) ) {
            qWarning().noquote()
                << QStringLiteral( "NassauDigital: a program is attached to a slot that hosts "
                                   "%1:%2; it is kept but not bound" )
                       .arg( QString::fromStdString( slot.getDescriptor().format ),
                             QString::fromStdString( slot.getDescriptor().uid ) );
            return;
        }
        bind( slot, *prog );
    } );
}

}  // namespace ssamplerwiring
