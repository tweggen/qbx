#include "app/shell/ssamplerwiring.h"

#include "app/model/sarrangements.h"
#include "app/model/slink.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/objects/sampler/ssamplerprogram.h"
#include "app/objects/sampler/ssamplerslots.h"
#include "app/objects/track/spluginchain.h"
#include "app/objects/track/sinstrumenttracks.h"
#include "app/objects/track/spluginslot.h"
#include "app/objects/track/strack.h"
#include "app/servicesui/soptions.h"
#include "app/shell/sapplication.h"
#include "app/shell/slivemonitor.h"
#include "app/shell/ssettings.h"

#include "tw/core/twlog.h"

#include "tw/plugins/twplugindescriptor.h"
#include "tw/sampler/twsamplerregistry.h"

#include <QDebug>
#include <QPointer>
#include <QSet>

#include <algorithm>
#include <chrono>
#include <vector>

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
    prog.setInstaller( [sp]( const SSamplerProgram::InstanceInitializer &future,
                             const SSamplerProgram::InstanceInitializer &now ) {
        if( !sp ) return;
        // Future instances first, then the ones that exist: a rebuild between
        // the two calls would otherwise build from the previous initializer.
        sp->setInstanceInitializer( future );
        // A slot with no processor yet (the load path, before the track
        // declares its width) has no instances: the initializer above is all
        // it needs, and nothing above it has rendered to be staled.
        if( sp->getProcessor() ) sp->applyToInstances( now );
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
    // Q7.1: a new instance starts in the per-user default view (DESIGN §7).
    // Before the adopt, so nothing bound ever sees another view.
    p->setViewState( static_cast<SSamplerProgram::View>( ssamplerwiring::newInstanceView() ),
                     0, 0 );
    slot->adoptChild( *p );   // -> the child-adopted hook below binds it
    return p;
}

}  // namespace

namespace ssamplerwiring {

void install()
{
    ssampler::setResolver( resolve );
    // The asset-link cycle walk (proposal 56 §4.7, Q3.q1) follows the sampler
    // links of every lane it passes; which programs sit on a lane is the
    // track's knowledge, so it is asked here.
    ssampler::setLaneProgramsHook( []( SObject *lane ) {
        QList<SSamplerProgram *> out;
        STrack *track = dynamic_cast<STrack *>( lane );
        SPluginChain *chain = track ? track->getPluginChain() : nullptr;
        if( !chain ) return out;
        for( int i = 0; i < chain->getSlotCount(); ++i ) {
            SPluginSlot *slot = chain->getSlotAt( i );
            if( !slot || !hostsNassauDigital( *slot ) ) continue;
            SSamplerProgram *p = programChild( *slot );
            if( p && !out.contains( p ) ) out.append( p );
        }
        return out;
    } );
    // ...and that same walk is the one every containment and placement verb
    // asks (QBX-267; proposal 56 §11 Q3): model's whole-graph guard
    // (sarrangements::reaches) cannot name objects/sampler, so it is handed
    // the walk here. reparent-track, move-clip, place-asset and the rest then
    // refuse an edit that would close a sampler asset-link cycle by the check
    // set-layer refuses a cycle-closing link with.
    sarrangements::setReachWalk( []( SObject *from, const SObject *to,
                                     const sarrangements::PendingEdges *pending, QString *via ) {
        return ssampler::assetReachesTrack( from, to, via, pending );
    } );
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

void installLiveOwnership( SApplication *app )
{
    QPointer<SApplication> ap( app );
    ssampler::setLiveOwnerHook( [ap]( SObject *body ) -> QString {
        SLiveMonitor *m = ap ? ap->liveMonitor() : nullptr;
        if( !m || !body ) return QString();
        // The PLAIN walk (child and content links): what a freeze of the
        // body's content reaches. Never the sampler hops of the cycle walk --
        // a second sampler inside the content plays its own installed
        // generation, its asset is not frozen by this bounce.
        for( STrack *t : m->liveOwnedTracks() ) {
            QSet<const SObject *> seen;
            if( !sarrangements::detail::reachesRec( body, t, seen, nullptr ) ) continue;
            SProject *proj = body->getProjectSafe();
            const QString spec = proj ? splacements::specOf( proj, t ) : QString();
            return spec.isEmpty() ? t->getSName() : spec;
        }
        return QString();
    } );
    if( app )
        QObject::connect( app, &SApplication::liveLanesMayHaveChanged, app,
                          [] { ssampler::liveOwnersMayHaveChanged(); } );
}

}  // namespace ssamplerwiring

namespace ssamplerwiring {

SSamplerProgram *recordTargetOf( STrack *track, int *slotIndex )
{
    SPluginChain *chain = track ? track->getPluginChain() : nullptr;
    if( !chain ) return nullptr;
    for( int i = 0; i < chain->getSlotCount(); ++i ) {
        SPluginSlot *slot = chain->getSlotAt( i );
        if( !slot || !hostsNassauDigital( *slot ) ) continue;
        SSamplerProgram *p = programChild( *slot );
        if( p && !p->recordInput().isEmpty() ) {
            if( slotIndex ) *slotIndex = i;
            return p;
        }
    }
    return nullptr;
}

QList<SSamplerProgram *> programsOf( STrack *track )
{
    QList<SSamplerProgram *> out;
    SPluginChain *chain = track ? track->getPluginChain() : nullptr;
    if( !chain ) return out;
    for( int i = 0; i < chain->getSlotCount(); ++i ) {
        SPluginSlot *slot = chain->getSlotAt( i );
        if( !slot || !hostsNassauDigital( *slot ) ) continue;
        if( SSamplerProgram *p = programChild( *slot ) ) out.append( p );
    }
    return out;
}

bool slotHostsNassauDigital( const SPluginSlot *slot )
{
    return slot && ::hostsNassauDigital( *slot );
}

QString nassauDigitalUid()
{
    return QString::fromLatin1( audio::kNassauDigitalUid );
}

SSamplerProgram *programOf( const SPluginSlot *slot )
{
    return slotHostsNassauDigital( slot ) ? programChild( *slot ) : nullptr;
}

int newInstanceView()
{
    const QString text = SSettings::instance()
                             .value( SOpt::SamplerDefaultView,
                                     SOpt::def( SOpt::SamplerDefaultView ) )
                             .toString()
                             .trimmed()
                             .toLower();
    SSamplerProgram::View v = SSamplerProgram::View::Key;
    if( !SSamplerProgram::viewFromText( text, v ) ) v = SSamplerProgram::View::Key;
    return static_cast<int>( v );
}

int settleBoundMs()
{
    int bound = 30000;
    const QByteArray env = qgetenv( "SMARAGD_SAMPLER_SETTLE_MS" );
    bool ok = false;
    const int v = env.toInt( &ok );
    if( ok && v >= 0 ) bound = v;
    return bound;
}

namespace {
int g_boundOverrideMs = -1;
}

void setSettleBoundOverrideMs( int ms )
{
    g_boundOverrideMs = ms;
}

bool settleBeforeRender( SProject *project, QString *why )
{
    if( !project ) return true;
    // check_root_resolution: master -- the same walk as SApplication::beginRun: an offline render renders the master
    SObject *root = project->masterRoot();
    if( !root ) return true;

    // THE SAME WALK AS THE BARRIER (objects/track inv. 14): the instrument
    // tracks under the master, never a registry.
    std::vector<STrack *> tracks;
    sinstruments::collectInstrumentTracks( root, tracks );

    const int bound = g_boundOverrideMs >= 0 ? g_boundOverrideMs : settleBoundMs();
    using clock = std::chrono::steady_clock;
    const clock::time_point t0 = clock::now();
    for( STrack *tr : tracks ) {
        SPluginSlot *slot = tr->instrumentSlot();
        if( !slot || !hostsNassauDigital( *slot ) ) continue;
        SSamplerProgram *prog = programChild( *slot );
        if( !prog ) continue;
        // An asset bounce the debounce still holds back (Q3) is outstanding
        // too: start it now, so this render hears the asset as it is.
        prog->flushDueBounces();
        // A layer WAITING for an armed track (QBX-274) is bounced here too:
        // startRender() suspended the live lane before this settle, so
        // nothing is live-owned now and the bounce hears the asset as it is
        // (the author, 2026-11-04, "render bounces fresh").
        if( prog->outstandingGenerations() == 0 ) continue;
        // ONE bound for the whole render, not one per track.
        const int left = std::max<qint64>(
            0, bound - std::chrono::duration_cast<std::chrono::milliseconds>( clock::now() - t0 )
                           .count() );
        int     n  = 0;
        qint64  ms = 0;
        QString late;
        const bool ok = prog->settle( left, &n, &ms, &late );
        const QString spec = splacements::specOf( project, tr );
        if( !ok ) {
            const QString w = QStringLiteral( "NassauDigital on track %1: %2 (bound %3 ms, "
                                              "SMARAGD_SAMPLER_SETTLE_MS)" )
                                  .arg( spec, late ).arg( bound );
            TW_LOGW( "app", "run barrier: REFUSED: %s", qPrintable( w ) );
            if( why ) *why = w;
            return false;
        }
        // GATES Q2.3 asserts this line; it is written only when a generation
        // WAS outstanding, so a render that had nothing to wait for cannot
        // produce it.
        TW_LOGI( "app", "run barrier: waited %lld ms for %d sampler generation(s) on track %s",
                 (long long)ms, n, qPrintable( spec ) );
    }
    return true;
}

}  // namespace ssamplerwiring
