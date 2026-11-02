#include "app/testkit/ssamplertestactions.h"

#include "app/actions/sactionregistry.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/objects/track/spluginactionsupport.h"
#include "app/objects/track/spluginslot.h"
#include "app/shell/sapplication.h"
#include "app/shell/ssamplerwiring.h"
#include "app/testkit/stestfilepath.h"
#include "app/objects/sampler/ssamplerprogram.h"
#include "app/objects/sampler/ssamplerslots.h"

#include "tw/analysis/audio_analysis.h"
#include "tw/graph/twcomponent.h"
#include "tw/plugins/twplugin.h"
#include "tw/plugins/twpluginslotproc.h"
#include "tw/sampler/twsamplerhost.h"
#include "tw/sampler/twsamplerprogrammodel.h"
#include "tw/sampler/twsamplertypes.h"

#include <QDebug>
#include <QDomElement>
#include <QDomNamedNodeMap>
#include <QFile>
#include <QHash>

#include <memory>
#include <string>
#include <vector>

namespace {

// The one-zone program the verb installs when no programFile is given: every
// key, the given root and play mode, one layer with the given id. The core's
// own grammar (nassau-digital nd_program_xml.h), written as text and parsed by
// the core: qbx never reads a program attribute (proposal 56 invariant 8).
std::string oneZoneProgram( int layer, int rootKey, const QString &mode )
{
    // The zone takes the id after the layer's, and nextId the one after that,
    // so neither id is ever handed out again (DESIGN §2).
    return "<NassauDigitalProgram version=\"1\" name=\"sampler-test-install\" nextId=\"" +
           std::to_string( layer + 2 ) + "\"><zone id=\"" +
           std::to_string( layer + 1 ) +
           "\" keyLo=\"0\" keyHi=\"127\" rootKey=\"" + std::to_string( rootKey ) +
           "\" mode=\"" + mode.toStdString() + "\"><layer id=\"" + std::to_string( layer ) +
           "\" src=\"test:sampler-test-install\"/></zone></NassauDigitalProgram>";
}

}  // namespace

SApplyResult SSamplerTestInstallAction::apply( SProject *project )
{
    SPluginSlot *slot = spluginaction::slotFor( project, trackPath_, slotIndex_ );
    if( !slot ) {
        qWarning() << "sampler-test-install: no slot" << slotIndex_ << "on track" << trackPath_;
        return { false, nullptr };
    }
    if( slot->getSlotState() != audio::twPluginSlotState::Active ) {
        qWarning() << "sampler-test-install: slot" << slotIndex_ << "on track" << trackPath_
                   << "is not Active";
        return { false, nullptr };
    }

    // --- the sample ----------------------------------------------------------
    const QString path = resolveTestFilePath( filePath_, SApplication::app().testOutputDir(),
                                              project );
    std::string err;
    audio::AudioFileInfo info{};
    if( !audio::readAudioFileInfo( path.toStdString(), info, err ) ) {
        qWarning() << "sampler-test-install: cannot read" << path << ":"
                   << QString::fromStdString( err );
        return { false, nullptr };
    }
    if( info.channelCount != 1 && info.channelCount != 2 ) {
        qWarning() << "sampler-test-install:" << path << "has" << info.channelCount
                   << "channels; a sample has 1 or 2";
        return { false, nullptr };
    }
    std::vector<std::vector<float>> pcm( (std::size_t)info.channelCount );
    int rate = 0;
    for( int c = 0; c < info.channelCount; ++c ) {
        if( !audio::readAudioRegion( path.toStdString(), 0, -1, c, pcm[(std::size_t)c], rate, err ) ) {
            qWarning() << "sampler-test-install: cannot read channel" << c << "of" << path << ":"
                       << QString::fromStdString( err );
            return { false, nullptr };
        }
    }
    std::vector<const float *> planar;
    for( const std::vector<float> &ch : pcm ) planar.push_back( ch.data() );
    std::shared_ptr<const audio::twSamplerSample> sample = audio::twSamplerSample::fromPlanar(
        info.channelCount, (double)rate, planar.data(), (std::int64_t)pcm[0].size(),
        (std::uint64_t)generation_, &err );
    if( !sample ) {
        qWarning() << "sampler-test-install: the core refused the sample:"
                   << QString::fromStdString( err );
        return { false, nullptr };
    }

    // --- the program ---------------------------------------------------------
    std::string xml;
    if( !programFile_.isEmpty() ) {
        const QString ppath = resolveTestFilePath( programFile_,
                                                   SApplication::app().testOutputDir(), project );
        QFile f( ppath );
        if( !f.open( QIODevice::ReadOnly ) ) {
            qWarning() << "sampler-test-install: cannot open program file" << ppath;
            return { false, nullptr };
        }
        xml = f.readAll().toStdString();
    } else {
        xml = oneZoneProgram( layer_, rootKey_, mode_ );
    }
    std::shared_ptr<const audio::twSamplerProgram> program =
        audio::twSamplerProgram::fromXml( xml, &err );
    if( !program ) {
        qWarning() << "sampler-test-install: the core refused the program:"
                   << QString::fromStdString( err );
        return { false, nullptr };
    }

    // --- the install: future instances, then the current ones -----------------
    const std::uint32_t layer = (std::uint32_t)layer_;
    auto install = [program, sample, layer]( audio::twPlugin &p ) {
        if( audio::twSamplerHost *h = dynamic_cast<audio::twSamplerHost *>( &p ) ) {
            h->setProgram( program );
            h->setSample( layer, sample );
        }
    };
    int hosts = 0;
    slot->setInstanceInitializer( install );
    slot->applyToInstances( [&]( audio::twPlugin &p ) {
        if( dynamic_cast<audio::twSamplerHost *>( &p ) ) ++hosts;
        install( p );
    } );
    if( hosts == 0 ) {
        slot->setInstanceInitializer( {} );
        qWarning() << "sampler-test-install: slot" << slotIndex_ << "on track" << trackPath_
                   << "holds no NassauDigital instance";
        return { false, nullptr };
    }

    qInfo().noquote() << QStringLiteral(
        "sampler-test-install: track %1 slot %2: %3 frames x %4 ch at %5 Hz as layer %6, "
        "program serial %7, on %8 instance(s)" )
        .arg( trackPath_ ).arg( slotIndex_ ).arg( (qlonglong)sample->frames() )
        .arg( sample->channels() ).arg( rate ).arg( layer_ )
        .arg( (qulonglong)program->serial() ).arg( hosts );
    return { true, nullptr };
}

void SSamplerTestInstallAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "trackPath", trackPath_ );
    elem.setAttribute( "slotIndex", slotIndex_ );
    elem.setAttribute( "filePath", filePath_ );
    elem.setAttribute( "layer", layer_ );
    elem.setAttribute( "rootKey", rootKey_ );
    elem.setAttribute( "mode", mode_ );
    elem.setAttribute( "generation", generation_ );
    if( !programFile_.isEmpty() ) elem.setAttribute( "programFile", programFile_ );
}

bool SSamplerTestInstallAction::readXml( const QDomElement &elem, int /*version*/ )
{
    trackPath_   = elem.attribute( "trackPath", "0" );
    slotIndex_   = elem.attribute( "slotIndex", "0" ).toInt();
    filePath_    = elem.attribute( "filePath" );
    layer_       = elem.attribute( "layer", "1" ).toInt();
    rootKey_     = elem.attribute( "rootKey", "60" ).toInt();
    mode_        = elem.attribute( "mode", "oneshot" );
    generation_  = elem.attribute( "generation", "1" ).toULongLong();
    programFile_ = elem.attribute( "programFile" );
    return !filePath_.isEmpty() && layer_ > 0;
}

static const bool s_reg_samplertestinstall =
    ( SActionRegistry::instance().registerType(
          QStringLiteral( "sampler-test-install" ),
          [] { return new SSamplerTestInstallAction; } ),
      true );

// ================================================== assert-sampler-program ===

namespace {

QHash<QString, audio::twSamplerProgramModel> &captures()
{
    static QHash<QString, audio::twSamplerProgramModel> c;
    return c;
}

const QStringList &checkNames()
{
    static const QStringList n = {
        QStringLiteral( "exists" ),       QStringLiteral( "readable" ),
        QStringLiteral( "zones" ),        QStringLiteral( "zoneIds" ),
        QStringLiteral( "nextId" ),       QStringLiteral( "equalsFile" ),
        QStringLiteral( "capture" ),      QStringLiteral( "equalsCaptured" ),
        QStringLiteral( "ignoreNextId" ), QStringLiteral( "differsFromCaptured" ),
        QStringLiteral( "scope" ),        QStringLiteral( "id" ),
        QStringLiteral( "attr" ),         QStringLiteral( "value" ),
        QStringLiteral( "xmlContains" ),  QStringLiteral( "selected" ),
        QStringLiteral( "installed" ),
    };
    return n;
}

QString idsText( const std::vector<std::uint32_t> &ids )
{
    QStringList parts;
    for( std::uint32_t id : ids ) parts << QString::number( id );
    return parts.join( QLatin1Char( ';' ) );
}

}  // namespace

SApplyResult SAssertSamplerProgramAction::apply( SProject *project )
{
    QStringList fails;
    const QString where = QStringLiteral( "assert-sampler-program: track %1 slot %2" )
                              .arg( trackPath_ ).arg( slotIndex_ );
    auto has = [this]( const char *k ) { return checks_.contains( QLatin1String( k ) ); };
    auto get = [this]( const char *k ) { return checks_.value( QLatin1String( k ) ); };

    bool anyCheck = false;
    for( const QString &k : checkNames() )
        if( checks_.contains( k ) && k != QLatin1String( "ignoreNextId" ) ) anyCheck = true;
    if( !anyCheck ) {
        qWarning().noquote() << where << ": checks NOTHING; REJECTED";
        return { false, nullptr };
    }

    QString why;
    SSamplerProgram *prog = ssampler::programAt( project, QString(), trackPath_, slotIndex_,
                                                 false, &why );
    if( has( "exists" ) ) {
        const bool want = get( "exists" ) == QLatin1String( "1" );
        if( want != ( prog != nullptr ) )
            fails << QStringLiteral( "exists: want %1, the slot %2 (%3)" )
                         .arg( want ? 1 : 0 )
                         .arg( prog ? QStringLiteral( "has a program" )
                                    : QStringLiteral( "has none" ) )
                         .arg( prog ? QString() : why );
    }
    if( !prog ) {
        if( !( has( "exists" ) && get( "exists" ) == QLatin1String( "0" ) && checks_.size() == 1 ) )
            fails << QStringLiteral( "no program: %1" ).arg( why );
        if( !fails.isEmpty() ) {
            for( const QString &f : fails ) qWarning().noquote() << where << "FAIL" << f;
            return { false, nullptr };
        }
        qInfo().noquote() << where << ": OK (no program object)";
        return { true, nullptr };
    }

    const audio::twSamplerProgramModel &m = prog->model();
    const QString xml = QString::fromStdString( m.toXml() );

    if( has( "readable" ) ) {
        const bool want = get( "readable" ) == QLatin1String( "1" );
        if( want != prog->isReadable() )
            fails << QStringLiteral( "readable: want %1, got %2 (%3)" )
                         .arg( want ? 1 : 0 ).arg( prog->isReadable() ? 1 : 0 )
                         .arg( prog->loadError() );
    }
    if( has( "zones" ) && get( "zones" ).toULongLong() != (qulonglong)m.zoneCount() )
        fails << QStringLiteral( "zones: want %1, got %2" ).arg( get( "zones" ) ).arg( m.zoneCount() );
    if( has( "zoneIds" ) && get( "zoneIds" ) != idsText( m.zoneIds() ) )
        fails << QStringLiteral( "zoneIds: want [%1], got [%2]" ).arg( get( "zoneIds" ), idsText( m.zoneIds() ) );
    if( has( "nextId" ) && get( "nextId" ).toULongLong() != (qulonglong)m.nextId() )
        fails << QStringLiteral( "nextId: want %1, got %2" ).arg( get( "nextId" ) ).arg( m.nextId() );

    if( has( "equalsFile" ) ) {
        const QString path = resolveTestFilePath( get( "equalsFile" ),
                                                  SApplication::app().testOutputDir(), project );
        QFile f( path );
        audio::twSamplerProgramModel want;
        std::string err;
        if( !f.open( QIODevice::ReadOnly ) ) {
            fails << QStringLiteral( "equalsFile: cannot open %1" ).arg( path );
        } else if( !audio::twSamplerProgramModel::fromXml( f.readAll().toStdString(), want, &err ) ) {
            fails << QStringLiteral( "equalsFile: the core refused %1: %2" )
                         .arg( path, QString::fromStdString( err ) );
        } else if( !( want == m ) ) {
            fails << QStringLiteral( "equalsFile: NOT structurally equal to %1.\n--- want\n%2\n--- got\n%3" )
                         .arg( path, QString::fromStdString( want.toXml() ), xml );
        }
    }
    if( has( "equalsCaptured" ) ) {
        const QString key = get( "equalsCaptured" );
        if( !captures().contains( key ) ) {
            fails << QStringLiteral( "equalsCaptured: nothing captured as \"%1\"" ).arg( key );
        } else {
            const audio::twSamplerProgramModel &want = captures()[key];
            const bool ignore = get( "ignoreNextId" ) == QLatin1String( "1" );
            const bool eq = ignore ? m.equalsIgnoringNextId( want ) : ( m == want );
            if( !eq )
                fails << QStringLiteral( "equalsCaptured \"%1\"%2: NOT equal.\n--- captured\n%3\n--- now\n%4" )
                             .arg( key, ignore ? QStringLiteral( " (nextId ignored)" ) : QString(),
                                   QString::fromStdString( want.toXml() ), xml );
        }
    }
    if( has( "differsFromCaptured" ) ) {
        const QString key = get( "differsFromCaptured" );
        if( !captures().contains( key ) )
            fails << QStringLiteral( "differsFromCaptured: nothing captured as \"%1\"" ).arg( key );
        else if( captures()[key] == m )
            fails << QStringLiteral( "differsFromCaptured \"%1\": the program is UNCHANGED" ).arg( key );
    }
    if( has( "attr" ) || has( "value" ) || has( "scope" ) || has( "id" ) ) {
        const QString sc = get( "scope" );
        audio::twSamplerScope scope = audio::twSamplerScope::Zone;
        bool scopeOk = true;
        if( sc == QLatin1String( "program" ) )    scope = audio::twSamplerScope::Program;
        else if( sc == QLatin1String( "zone" ) )  scope = audio::twSamplerScope::Zone;
        else if( sc == QLatin1String( "layer" ) ) scope = audio::twSamplerScope::Layer;
        else scopeOk = false;
        if( !scopeOk || !has( "attr" ) || !has( "value" ) ) {
            fails << QStringLiteral( "a value check needs scope=program|zone|layer, attr= and value=" );
        } else {
            std::string got;
            if( !m.value( scope, (std::uint32_t)get( "id" ).toULongLong(), get( "attr" ).toStdString(), &got ) )
                fails << QStringLiteral( "%1 %2 has no attribute \"%3\"" ).arg( sc, get( "id" ), get( "attr" ) );
            else if( QString::fromStdString( got ) != get( "value" ) )
                fails << QStringLiteral( "%1 %2 %3: want \"%4\", got \"%5\"" )
                             .arg( sc, get( "id" ), get( "attr" ), get( "value" ),
                                   QString::fromStdString( got ) );
        }
    }
    if( has( "xmlContains" ) && !xml.contains( get( "xmlContains" ) ) )
        fails << QStringLiteral( "xmlContains: \"%1\" is not in\n%2" ).arg( get( "xmlContains" ), xml );
    if( has( "selected" ) ) {
        std::vector<std::uint32_t> sel;
        for( std::uint32_t id : prog->selectedZones() ) sel.push_back( id );
        if( get( "selected" ) != idsText( sel ) )
            fails << QStringLiteral( "selected: want [%1], got [%2]" ).arg( get( "selected" ), idsText( sel ) );
    }
    if( has( "installed" ) ) {
        SPluginSlot *slot = spluginaction::slotFor( project, trackPath_, slotIndex_ );
        const std::shared_ptr<audio::twPluginSlotProcessor> proc = slot ? slot->getProcessor() : nullptr;
        int hosts = 0, current = 0;
        QStringList got;
        if( proc ) {
            for( audio::twPlugin *p : proc->plugins() ) {
                // stats() is the one twSamplerHost method callable outside
                // applyToInstances (twsamplerhost.h): it reads atomics.
                audio::twSamplerHost *h = p ? dynamic_cast<audio::twSamplerHost *>( p ) : nullptr;
                if( !h ) continue;
                ++hosts;
                const std::uint64_t s = h->stats().installedSerial;
                got << QString::number( s );
                if( s != 0 && s == prog->publishedSerial() ) ++current;
            }
        }
        if( hosts == 0 || current != hosts )
            fails << QStringLiteral( "installed: %1 of %2 instance(s) hold the published program "
                                     "(serial %3; instances hold [%4])" )
                         .arg( current ).arg( hosts ).arg( prog->publishedSerial() )
                         .arg( got.join( QLatin1Char( ',' ) ) );
    }

    if( has( "capture" ) ) captures().insert( get( "capture" ), m );

    if( !fails.isEmpty() ) {
        for( const QString &f : fails ) qWarning().noquote() << where << "FAIL" << f;
        return { false, nullptr };
    }
    qInfo().noquote() << where << ": OK (" << m.zoneCount() << "zone(s), nextId" << m.nextId() << ")";
    return { true, nullptr };
}

void SAssertSamplerProgramAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "trackPath", trackPath_ );
    elem.setAttribute( "slotIndex", slotIndex_ );
    for( auto it = checks_.constBegin(); it != checks_.constEnd(); ++it )
        elem.setAttribute( it.key(), it.value() );
}

bool SAssertSamplerProgramAction::readXml( const QDomElement &elem, int /*version*/ )
{
    trackPath_ = elem.attribute( "trackPath", "0" );
    slotIndex_ = elem.attribute( "slotIndex", "0" ).toInt();
    checks_.clear();
    for( const QString &k : checkNames() )
        if( elem.hasAttribute( k ) ) checks_.insert( k, elem.attribute( k ) );
    return true;
}

QStringList SAssertSamplerProgramAction::knownAttributes() const
{
    return QStringList{ QStringLiteral( "trackPath" ), QStringLiteral( "slotIndex" ) } + checkNames();
}

static const bool s_reg_assertsamplerprogram =
    ( SActionRegistry::instance().registerType(
          QStringLiteral( "assert-sampler-program" ),
          [] { return new SAssertSamplerProgramAction; } ),
      true );

// ================================================ set-sampler-render-gate ===

SApplyResult SSetSamplerRenderGateAction::apply( SProject *project )
{
    const QString where = QStringLiteral( "set-sampler-render-gate: track %1 slot %2 layer %3" )
                              .arg( trackPath_ ).arg( slotIndex_ ).arg( layer_ );
    if( hasBound_ ) ssamplerwiring::setSettleBoundOverrideMs( settleBoundMs_ );
    if( layer_ <= 0 ) {
        if( hasBound_ ) {
            qInfo().noquote() << QStringLiteral( "set-sampler-render-gate: settle bound %1 ms" )
                                     .arg( settleBoundMs_ );
            return { true, nullptr };
        }
        qWarning().noquote() << where << ": needs layer= (or settleBoundMs= alone); REJECTED";
        return { false, nullptr };
    }
    QString why;
    SSamplerProgram *prog = ssampler::programAt( project, pathRoot(), trackPath_, slotIndex_,
                                                 false, &why );
    if( !prog ) {
        qWarning().noquote() << where << ": REJECTED:" << why;
        return { false, nullptr };
    }
    if( !prog->model().hasLayer( (std::uint32_t)layer_ ) ) {
        qWarning().noquote() << where << ": REJECTED: no such layer";
        return { false, nullptr };
    }
    if( ready_ ) prog->releaseHeldJob( (std::uint32_t)layer_ );
    else         prog->holdNextJob( (std::uint32_t)layer_, holdMs_ );
    qInfo().noquote() << where << ( ready_ ? ": released"
                                           : QStringLiteral( ": the next job HOLDS (%1)" )
                                                 .arg( holdMs_ >= 0
                                                           ? QStringLiteral( "%1 ms" ).arg( holdMs_ )
                                                           : QStringLiteral( "until released" ) ) );
    return { true, nullptr };
}

void SSetSamplerRenderGateAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "trackPath", trackPath_ );
    elem.setAttribute( "slotIndex", slotIndex_ );
    elem.setAttribute( "layer", layer_ );
    elem.setAttribute( "ready", ready_ ? "true" : "false" );
    elem.setAttribute( "holdMs", holdMs_ );
    if( hasBound_ ) elem.setAttribute( "settleBoundMs", settleBoundMs_ );
}

bool SSetSamplerRenderGateAction::readXml( const QDomElement &elem, int /*version*/ )
{
    trackPath_ = elem.attribute( "trackPath", "0" );
    slotIndex_ = elem.attribute( "slotIndex", "0" ).toInt();
    layer_     = elem.attribute( "layer", "0" ).toInt();
    ready_     = elem.attribute( "ready", "false" ) == QLatin1String( "true" );
    holdMs_    = elem.attribute( "holdMs", "-1" ).toInt();
    hasBound_  = elem.hasAttribute( "settleBoundMs" );
    settleBoundMs_ = elem.attribute( "settleBoundMs", "-1" ).toInt();
    return true;
}

QStringList SSetSamplerRenderGateAction::knownAttributes() const
{
    return { QStringLiteral( "trackPath" ), QStringLiteral( "slotIndex" ),
             QStringLiteral( "layer" ),     QStringLiteral( "ready" ),
             QStringLiteral( "holdMs" ),    QStringLiteral( "settleBoundMs" ) };
}

static const bool s_reg_setsamplerrendergate =
    ( SActionRegistry::instance().registerType(
          QStringLiteral( "set-sampler-render-gate" ),
          [] { return new SSetSamplerRenderGateAction; } ),
      true );

// =================================================== assert-sampler-layer ===

namespace {
const QStringList &layerCheckNames()
{
    static const QStringList n = { QStringLiteral( "generation" ), QStringLiteral( "rendering" ),
                                   QStringLiteral( "pending" ),    QStringLiteral( "installs" ),
                                   QStringLiteral( "src" ),        QStringLiteral( "captureTrackEpoch" ),
                                   QStringLiteral( "trackEpochAdvancedSince" ),
                                   // Q3 (QBX-266)
                                   QStringLiteral( "renders" ),    QStringLiteral( "asset" ) };
    return n;
}

// The TRACK's content epoch: the counter its pages are stamped with. A bump
// makes every page stamped before it STALE (twcomponent.h: a page is stale
// when page->contentEpoch < contentEpochNow()), so "the epoch advanced across
// an install" is exactly "the install staled the track's pages", whether or
// not any page happens to be cached at that moment.
QHash<QString, uint64_t> &epochCaptures()
{
    static QHash<QString, uint64_t> c;
    return c;
}
}  // namespace

SApplyResult SAssertSamplerLayerAction::apply( SProject *project )
{
    const QString where = QStringLiteral( "assert-sampler-layer: track %1 slot %2 layer %3" )
                              .arg( trackPath_ ).arg( slotIndex_ ).arg( layer_ );
    if( checks_.isEmpty() ) {
        qWarning().noquote() << where << ": checks NOTHING; REJECTED";
        return { false, nullptr };
    }
    QString why;
    SSamplerProgram *prog = ssampler::programAt( project, pathRoot(), trackPath_, slotIndex_,
                                                 false, &why );
    if( !prog ) {
        qWarning().noquote() << where << "FAIL no program:" << why;
        return { false, nullptr };
    }
    const SSamplerProgram::LayerState st = prog->layerState( (std::uint32_t)layer_ );
    // The asset the layer's link targets NOW, by the registry's current name
    // (Q3.4: a rename moves the name, never the link).
    QString assetNow;
    {
        std::string src;
        if( prog->model().value( audio::twSamplerScope::Layer, (std::uint32_t)layer_, "src", &src ) &&
            src.compare( 0, 5, "link:" ) == 0 ) {
            SObject *body = prog->linkTarget( (std::uint32_t)QString::fromStdString( src.substr( 5 ) )
                                                  .toULongLong() );
            assetNow = body ? project->assetNameOf( body ) : QStringLiteral( "<no link>" );
        }
    }
    const QString got = QStringLiteral( "generation=%1 rendering=%2 pending=%3 installs=%4 src=%5 "
                                        "renders=%6 asset=%7" )
                            .arg( (qulonglong)st.installedGeneration )
                            .arg( st.rendering() ? 1 : 0 )
                            .arg( (qulonglong)st.pendingGeneration )
                            .arg( st.installs )
                            .arg( st.installedSrc )
                            .arg( st.renders )
                            .arg( assetNow );
    QStringList fails;
    auto num = [&]( const char *k, qulonglong have ) {
        if( checks_.contains( QLatin1String( k ) ) &&
            checks_.value( QLatin1String( k ) ).toULongLong() != have )
            fails << QStringLiteral( "%1: want %2" ).arg( QLatin1String( k ),
                                                          checks_.value( QLatin1String( k ) ) );
    };
    num( "generation", st.installedGeneration );
    num( "rendering", st.rendering() ? 1 : 0 );
    num( "pending", st.pendingGeneration );
    num( "installs", (qulonglong)st.installs );
    num( "renders", (qulonglong)st.renders );
    if( checks_.contains( QStringLiteral( "asset" ) ) && checks_.value( "asset" ) != assetNow )
        fails << QStringLiteral( "asset: want \"%1\"" ).arg( checks_.value( "asset" ) );
    if( checks_.contains( QStringLiteral( "src" ) ) && checks_.value( "src" ) != st.installedSrc )
        fails << QStringLiteral( "src: want \"%1\"" ).arg( checks_.value( "src" ) );
    if( checks_.contains( QStringLiteral( "captureTrackEpoch" ) ) ||
        checks_.contains( QStringLiteral( "trackEpochAdvancedSince" ) ) ) {
        QString root = pathRoot();
        SObject *track = splacements::laneBySpec( project, trackPath_, root );
        std::shared_ptr<twComponent> comp = track ? track->getRootComponent() : nullptr;
        if( !comp ) {
            fails << QStringLiteral( "no track component at %1" ).arg( trackPath_ );
        } else {
            const uint64_t now = comp->contentEpochNow();
            const QString since = checks_.value( QStringLiteral( "trackEpochAdvancedSince" ) );
            if( !since.isEmpty() ) {
                if( !epochCaptures().contains( since ) )
                    fails << QStringLiteral( "trackEpochAdvancedSince: nothing captured as \"%1\"" )
                                 .arg( since );
                else if( now <= epochCaptures().value( since ) )
                    fails << QStringLiteral( "trackEpochAdvancedSince \"%1\": the track's content "
                                             "epoch is still %2 (captured %3): its pages were NOT "
                                             "staled" )
                                 .arg( since ).arg( now ).arg( epochCaptures().value( since ) );
                else
                    qInfo().noquote() << where << QStringLiteral( ": track epoch %1 -> %2" )
                                                      .arg( epochCaptures().value( since ) ).arg( now );
            }
            const QString cap = checks_.value( QStringLiteral( "captureTrackEpoch" ) );
            if( !cap.isEmpty() ) epochCaptures().insert( cap, now );
        }
    }
    if( !fails.isEmpty() ) {
        for( const QString &f : fails ) qWarning().noquote() << where << "FAIL" << f << "; got" << got;
        return { false, nullptr };
    }
    qInfo().noquote() << where << ": OK (" << got << ")";
    return { true, nullptr };
}

void SAssertSamplerLayerAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "trackPath", trackPath_ );
    elem.setAttribute( "slotIndex", slotIndex_ );
    elem.setAttribute( "layer", layer_ );
    for( auto it = checks_.constBegin(); it != checks_.constEnd(); ++it )
        elem.setAttribute( it.key(), it.value() );
}

bool SAssertSamplerLayerAction::readXml( const QDomElement &elem, int /*version*/ )
{
    trackPath_ = elem.attribute( "trackPath", "0" );
    slotIndex_ = elem.attribute( "slotIndex", "0" ).toInt();
    layer_     = elem.attribute( "layer", "0" ).toInt();
    checks_.clear();
    for( const QString &k : layerCheckNames() )
        if( elem.hasAttribute( k ) ) checks_.insert( k, elem.attribute( k ) );
    return true;
}

QStringList SAssertSamplerLayerAction::knownAttributes() const
{
    return QStringList{ QStringLiteral( "trackPath" ), QStringLiteral( "slotIndex" ),
                        QStringLiteral( "layer" ) } + layerCheckNames();
}

static const bool s_reg_assertsamplerlayer =
    ( SActionRegistry::instance().registerType(
          QStringLiteral( "assert-sampler-layer" ),
          [] { return new SAssertSamplerLayerAction; } ),
      true );

// =============================================== wait-sampler-generations ===

SApplyResult SWaitSamplerGenerationsAction::apply( SProject *project )
{
    const QString where = QStringLiteral( "wait-sampler-generations: track %1 slot %2" )
                              .arg( trackPath_ ).arg( slotIndex_ );
    QString why;
    SSamplerProgram *prog = ssampler::programAt( project, pathRoot(), trackPath_, slotIndex_,
                                                 false, &why );
    if( !prog ) {
        qWarning().noquote() << where << ": REJECTED:" << why;
        return { false, nullptr };
    }
    int     n  = 0;
    qint64  ms = 0;
    QString late;
    if( !prog->settle( timeoutMs_, &n, &ms, &late ) ) {
        qWarning().noquote() << where << "FAIL:" << late;
        return { false, nullptr };
    }
    qInfo().noquote() << where << QStringLiteral( ": %1 generation(s) settled in %2 ms" )
                                      .arg( n ).arg( ms );
    return { true, nullptr };
}

void SWaitSamplerGenerationsAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "trackPath", trackPath_ );
    elem.setAttribute( "slotIndex", slotIndex_ );
    elem.setAttribute( "timeoutMs", timeoutMs_ );
}

bool SWaitSamplerGenerationsAction::readXml( const QDomElement &elem, int /*version*/ )
{
    trackPath_ = elem.attribute( "trackPath", "0" );
    slotIndex_ = elem.attribute( "slotIndex", "0" ).toInt();
    timeoutMs_ = elem.attribute( "timeoutMs", "15000" ).toInt();
    if( timeoutMs_ < 0 ) timeoutMs_ = 15000;
    return true;
}

QStringList SWaitSamplerGenerationsAction::knownAttributes() const
{
    return { QStringLiteral( "trackPath" ), QStringLiteral( "slotIndex" ),
             QStringLiteral( "timeoutMs" ) };
}

static const bool s_reg_waitsamplergenerations =
    ( SActionRegistry::instance().registerType(
          QStringLiteral( "wait-sampler-generations" ),
          [] { return new SWaitSamplerGenerationsAction; } ),
      true );
