#include "app/objects/track/slearnfeelflowaction.h"

#include "app/actions/sactionregistry.h"
#include "app/model/sappcontext.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/model/sprojectprops.h"
#include "app/objects/track/strack.h"
#include "app/objects/track/strackpath.h"

#include "tw/graph/tw303aenv.h"
#include "tw/sidecar/twgroove.h"
#include "tw/sidecar/twgrooveaspect.h"
#include "tw/sidecar/twgroovependulum.h"
#include "tw/sources/twsamplesource.h"

#include <QByteArray>
#include <QDebug>
#include <QDomElement>

#include <algorithm>
#include <memory>

using namespace strackpath;

namespace {

// Snapshot of the track's CURRENT trained structure, serialized (empty +
// hadPrior==false when there was none) -- the shape both apply() paths need
// to build a working inverse.
struct PriorSnapshot {
    bool                 hadPrior = false;
    std::vector<uint8_t> blob;
};

PriorSnapshot snapshotPrior( STrack *track )
{
    PriorSnapshot s;
    if( std::unique_ptr<twGrooveTrainedStructure> prev =
            track->copyFeelFlowTrainedStructure() ) {
        s.hadPrior = true;
        twGrooveTrainedStructureSerialize( *prev, s.blob );
    }
    return s;
}

SLearnFeelFlowAction *inverseFrom( const QList<int> &trackPath,
                                   const PriorSnapshot &prior )
{
    SLearnFeelFlowAction *inv = new SLearnFeelFlowAction( trackPath );
    inv->setRestore( prior.hadPrior, prior.blob );
    return inv;
}

} // namespace

SApplyResult SLearnFeelFlowAction::apply( SProject *project )
{
    if( !project ) return { false, nullptr };
    SObject *root = splacements::rootContainer( project );
    SObject *lane = splacements::laneAt( root, trackPath_ );
    STrack *track = dynamic_cast<STrack *>( lane );
    if( !track ) {
        qWarning() << "learn-feel-flow: no track at path" << pathToString( trackPath_ );
        return { false, nullptr };
    }

    const PriorSnapshot prior = snapshotPrior( track );

    // -------------------------------------------------- restore form (undo) --
    if( restoreIsSet_ ) {
        if( !restoreHadPrior_ ) {
            track->setFeelFlowTrainedStructureInternal( nullptr );
        } else {
            auto structure = std::make_unique<twGrooveTrainedStructure>();
            if( !twGrooveTrainedStructureDeserialize(
                    restoreBlob_.data(), (uint64_t) restoreBlob_.size(),
                    *structure ) ) {
                qWarning() << "learn-feel-flow: malformed restore blob for track"
                           << pathToString( trackPath_ );
                return { false, nullptr };
            }
            track->setFeelFlowTrainedStructureInternal( std::move( structure ) );
        }
        return { true, inverseFrom( trackPath_, prior ) };
    }

    // ------------------------------------------------------------ forward form --
    if( !track->feelFlowHasResult() ) {
        qWarning() << "learn-feel-flow: track" << pathToString( trackPath_ )
                   << "has never been bounced -- run"
                      " feel-flow-analyze target=\"track\" first";
        return { false, nullptr };
    }
    if( !project->prop( SProjectProps::RangeValid, false ).toBool() ) {
        qWarning() << "learn-feel-flow: no in/out selection is set (rangeValid)";
        return { false, nullptr };
    }
    const uint64_t selStart =
        project->prop( SProjectProps::RangeStart, (qulonglong) 0 ).toULongLong();
    const uint64_t selEnd =
        project->prop( SProjectProps::RangeEnd, (qulonglong) 0 ).toULongLong();
    if( selEnd <= selStart ) {
        qWarning() << "learn-feel-flow: empty selection";
        return { false, nullptr };
    }

    const std::string bouncePath = track->feelFlowBouncePath();
    if( bouncePath.empty() ) {
        qWarning() << "learn-feel-flow: track" << pathToString( trackPath_ )
                   << "has no bounce file";
        return { false, nullptr };
    }
    tw303aEnvironment *env = SAppContext::get().get303aEnvironment();
    if( !env ) return { false, nullptr };

    twSampleSource src( *env, QString::fromStdString( bouncePath ) );
    if( !src.wasLoaded() ) {
        qWarning() << "learn-feel-flow: failed to decode bounce"
                   << QString::fromStdString( bouncePath );
        return { false, nullptr };
    }

    const uint32_t nCh   = (uint32_t) src.channels();
    const uint64_t total = (uint64_t) src.length();
    const uint64_t start = std::min<uint64_t>( selStart, total );
    const uint64_t end   = std::min<uint64_t>( selEnd, total );
    if( end <= start || nCh == 0 ) {
        qWarning() << "learn-feel-flow: selection lies outside the bounce audio"
                      " (bounce length" << (qulonglong) total << "frames)";
        return { false, nullptr };
    }
    const uint32_t rate = (uint32_t) src.sampleRate();

    // Offset each channel's pointer to the selection start -- the SAME
    // "slice by pointer arithmetic" a plain planar buffer always allows,
    // never a copy.
    std::vector<const float *> chans( nCh );
    for( uint32_t c = 0; c < nCh; c++ )
        chans[c] = src.channelData( (idx_t) c ) + start;

    // M1's default analysis-side params, verbatim -- the SAME defaults
    // twGrooveAnalysisParams's own members default to, so a structure
    // trained here is directly compatible with what an Adaptive-mode
    // analysis run would otherwise have used.
    const twGrooveFrontEndParams feParams;
    const twGroovePendulumParams pParams;

    const twGrooveField field = twGrooveAnalyzeFrontEnd(
        chans.data(), nCh, end - start, rate, feParams );
    if( field.nHops == 0 ) {
        qWarning() << "learn-feel-flow: selection has no usable audio";
        return { false, nullptr };
    }

    twGrooveTrainedStructure trained =
        twGroovePendulumTrainStructure( field, pParams );
    // twGroovePendulumTrainStructure ALWAYS returns a trainedHasRegion sized
    // to the front end's region count (twgroovependulum.cc), true failure
    // (no recoverable tatum, or nothing scored in ANY region) leaves every
    // entry false rather than the vector empty -- so "did this actually
    // learn something" is "at least one region has data", not "is the
    // vector non-empty" (that question -- "has this track EVER been
    // trained" -- is what feelFlowHasTrainedStructure()/the null check on
    // feelFlowTrainedStructure() answer instead, at the STrack level).
    const bool trainedOk = std::any_of( trained.trainedHasRegion.begin(),
                                        trained.trainedHasRegion.end(),
                                        []( bool b ) { return b; } );
    if( !trainedOk ) {
        qWarning() << "learn-feel-flow: selection has no recoverable tatum --"
                      " nothing to train";
        return { false, nullptr };
    }

    track->setFeelFlowTrainedStructureInternal(
        std::make_unique<twGrooveTrainedStructure>( std::move( trained ) ) );

    return { true, inverseFrom( trackPath_, prior ) };
}

QStringList SLearnFeelFlowAction::knownAttributes() const
{
    return { QStringLiteral( "trackPath" ), QStringLiteral( "restore" ),
             QStringLiteral( "data" ) };
}

void SLearnFeelFlowAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "trackPath", pathToString( trackPath_ ) );
    if( restoreIsSet_ ) {
        elem.setAttribute( "restore", "1" );
        if( restoreHadPrior_ ) {
            const QByteArray b64 = QByteArray(
                (const char *) restoreBlob_.data(),
                (int) restoreBlob_.size() ).toBase64();
            elem.setAttribute( "data", QString::fromLatin1( b64 ) );
        }
    }
}

bool SLearnFeelFlowAction::readXml( const QDomElement &elem, int /*version*/ )
{
    trackPath_ = stringToPath( elem.attribute( "trackPath", "0" ) );
    restoreIsSet_ = elem.attribute( "restore", "0" ) == "1";
    if( restoreIsSet_ ) {
        const QString dataAttr = elem.attribute( "data" );
        restoreHadPrior_ = !dataAttr.isEmpty();
        if( restoreHadPrior_ ) {
            const QByteArray blob = QByteArray::fromBase64( dataAttr.toLatin1() );
            restoreBlob_.assign( blob.constData(), blob.constData() + blob.size() );
        } else {
            restoreBlob_.clear();
        }
    }
    return true;
}

static const bool s_reg_learn_feel_flow = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "learn-feel-flow" ),
        [] { return new SLearnFeelFlowAction; } ), true );
