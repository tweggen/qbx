#include "app/testkit/ssamplertestactions.h"

#include "app/actions/sactionregistry.h"
#include "app/objects/track/spluginactionsupport.h"
#include "app/objects/track/spluginslot.h"
#include "app/shell/sapplication.h"
#include "app/testkit/stestfilepath.h"

#include "tw/analysis/audio_analysis.h"
#include "tw/plugins/twplugin.h"
#include "tw/sampler/twsamplerhost.h"
#include "tw/sampler/twsamplertypes.h"

#include <QDebug>
#include <QDomElement>
#include <QFile>

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
