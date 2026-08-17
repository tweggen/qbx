#include "app/testkit/slivetestactions.h"

#include <cmath>
#include <vector>

#include <QDebug>
#include <QDomElement>

#include "app/actions/sactionregistry.h"
#include "app/model/sobjectpath.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/objects/track/strack.h"
#include "app/shell/sapplication.h"
#include "app/shell/slivemonitor.h"
#include "app/testkit/stestfilepath.h"
#include "tw/analysis/audio_analysis.h"
#include "tw/graph/tw_freeze_context.h"

using namespace strackpath;

namespace {

// Resolve a test artifact / fixture the way every other assert verb does.
bool resolveOrReject( const QString &name, const SProject *project,
                      const char *verb, QString &out )
{
    SApplication &app = SApplication::app();
    const QString dir = app.testOutputDir();
    if( dir.isEmpty() ) {
        qWarning() << verb << ": no --test-output-dir; refusing to guess";
        return false;
    }
    out = resolveTestFilePath( name, dir, project );
    return true;
}

}  // namespace

// --- assert-monitor-latency -------------------------------------------------

QStringList SAssertMonitorLatencyAction::knownAttributes() const
{
    return { QStringLiteral( "filename" ),   QStringLiteral( "inputFile" ),
             QStringLiteral( "channel" ),    QStringLiteral( "startFrame" ),
             QStringLiteral( "frameCount" ), QStringLiteral( "maxFrames" ),
             QStringLiteral( "searchFrames" ),
             QStringLiteral( "minCorrelation" ) };
}

SApplyResult SAssertMonitorLatencyAction::apply( SProject *project )
{
    if( filename_.isEmpty() || inputFile_.isEmpty() ) {
        qWarning() << "assert-monitor-latency: filename and inputFile are both required";
        return { false, nullptr };
    }
    QString capPath, inPath;
    if( !resolveOrReject( filename_, project, "assert-monitor-latency", capPath ) )
        return { false, nullptr };
    if( !resolveOrReject( inputFile_, project, "assert-monitor-latency", inPath ) )
        return { false, nullptr };

    std::string err;
    int capRate = 0, inRate = 0;
    std::vector<float> cap, in;
    if( !audio::readAudioRegion( capPath.toStdString(), startFrame_, frameCount_,
                                 channel_, cap, capRate, err )
        || cap.empty() ) {
        qWarning() << "assert-monitor-latency: cannot read capture window from"
                   << capPath << QString::fromStdString( err );
        return { false, nullptr };
    }
    // The input side has to cover the window PLUS the search range on both
    // sides: a monitored block is LATER than the input frame it carries, so
    // the match sits earlier in the file, and the whole file is short enough
    // to read outright.
    if( !audio::readAudioRegion( inPath.toStdString(), 0, -1, channel_, in, inRate, err )
        || in.empty() ) {
        qWarning() << "assert-monitor-latency: cannot read input file" << inPath
                   << QString::fromStdString( err );
        return { false, nullptr };
    }
    if( capRate != inRate ) {
        qWarning() << "assert-monitor-latency: rate mismatch, capture" << capRate
                   << "vs input" << inRate
                   << "- the live lane only runs at the project rate";
        return { false, nullptr };
    }

    // NORMALISED cross-correlation. The peak is then a SIMILARITY in [-1, 1]
    // rather than an energy, so `minCorrelation` means the same thing whatever
    // the level is - which matters because a monitored signal has been through
    // a fader and an insert.
    const std::size_t n = cap.size();
    double capEnergy = 0.0;
    for( float v : cap ) capEnergy += (double) v * v;
    if( capEnergy <= 0.0 ) {
        qWarning() << "assert-monitor-latency: the capture window is SILENT -"
                   << "nothing was monitored";
        return { false, nullptr };
    }

    double  bestCorr = -2.0;
    qint64  bestLag  = -1;
    // Lag L means "the capture frame at startFrame_ carries the input frame at
    // (startFrame_ - L)", i.e. the output is L frames behind the input. The
    // file is LOOPED by FileAudioInput, so the index wraps.
    const qint64 inLen = (qint64) in.size();
    for( qint64 lag = 0; lag <= searchFrames_; ++lag ) {
        qint64 base = ( startFrame_ - lag ) % inLen;
        if( base < 0 ) base += inLen;
        double dot = 0.0, inEnergy = 0.0;
        for( std::size_t i = 0; i < n; ++i ) {
            const float b = in[(std::size_t) ( ( base + (qint64) i ) % inLen )];
            dot      += (double) cap[i] * b;
            inEnergy += (double) b * b;
        }
        if( inEnergy <= 0.0 ) continue;
        const double corr = dot / std::sqrt( capEnergy * inEnergy );
        if( corr > bestCorr ) { bestCorr = corr; bestLag = lag; }
    }

    if( bestCorr < minCorrelation_ ) {
        qWarning() << "assert-monitor-latency: the capture does not correlate with"
                   << "the input at any lag - best" << bestCorr << "at" << bestLag
                   << "frames, needed" << minCorrelation_;
        return { false, nullptr };
    }
    if( bestLag > maxFrames_ ) {
        qWarning() << "assert-monitor-latency: monitored lag" << bestLag
                   << "frames exceeds the budget" << maxFrames_
                   << "(correlation" << bestCorr << ")";
        return { false, nullptr };
    }
    qDebug() << "assert-monitor-latency: lag" << bestLag << "frames ("
             << ( 1000.0 * (double) bestLag / (double) ( capRate ? capRate : 48000 ) )
             << "ms ), correlation" << bestCorr << ", budget" << maxFrames_;
    return { true, nullptr };
}

void SAssertMonitorLatencyAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "filename", filename_ );
    elem.setAttribute( "inputFile", inputFile_ );
    elem.setAttribute( "maxFrames", QString::number( maxFrames_ ) );
    if( channel_ != 0 )         elem.setAttribute( "channel", QString::number( channel_ ) );
    if( startFrame_ != 24000 )  elem.setAttribute( "startFrame", QString::number( startFrame_ ) );
    if( frameCount_ != 24000 )  elem.setAttribute( "frameCount", QString::number( frameCount_ ) );
    if( searchFrames_ != 24000 )elem.setAttribute( "searchFrames", QString::number( searchFrames_ ) );
    if( minCorrelation_ != 0.5 )
        elem.setAttribute( "minCorrelation", QString::number( minCorrelation_ ) );
}

bool SAssertMonitorLatencyAction::readXml( const QDomElement &elem, int )
{
    filename_       = elem.attribute( "filename", "" );
    inputFile_      = elem.attribute( "inputFile", "" );
    channel_        = elem.attribute( "channel", "0" ).toInt();
    startFrame_     = elem.attribute( "startFrame", "24000" ).toLongLong();
    frameCount_     = elem.attribute( "frameCount", "24000" ).toLongLong();
    maxFrames_      = elem.attribute( "maxFrames", "8192" ).toLongLong();
    searchFrames_   = elem.attribute( "searchFrames", "24000" ).toLongLong();
    minCorrelation_ = elem.attribute( "minCorrelation", "0.5" ).toDouble();
    return true;
}

// --- assert-audio-continuity ------------------------------------------------

QStringList SAssertAudioContinuityAction::knownAttributes() const
{
    return { QStringLiteral( "filename" ),   QStringLiteral( "channel" ),
             QStringLiteral( "startFrame" ), QStringLiteral( "frameCount" ),
             QStringLiteral( "maxGapFrames" ), QStringLiteral( "maxStep" ),
             QStringLiteral( "silenceLevel" ) };
}

SApplyResult SAssertAudioContinuityAction::apply( SProject *project )
{
    if( filename_.isEmpty() ) {
        qWarning() << "assert-audio-continuity: no filename given";
        return { false, nullptr };
    }
    QString path;
    if( !resolveOrReject( filename_, project, "assert-audio-continuity", path ) )
        return { false, nullptr };

    std::string err;
    int rate = 0;
    std::vector<float> x;
    if( !audio::readAudioRegion( path.toStdString(), startFrame_, frameCount_,
                                 channel_, x, rate, err ) ) {
        qWarning() << "assert-audio-continuity: cannot read" << path
                   << QString::fromStdString( err );
        return { false, nullptr };
    }
    if( x.empty() ) {
        qWarning() << "assert-audio-continuity: the region is EMPTY";
        return { false, nullptr };
    }

    qint64 gap = 0, worstGap = 0, worstGapAt = -1;
    double worstStep = 0.0;
    qint64 worstStepAt = -1;
    for( std::size_t i = 0; i < x.size(); ++i ) {
        const double a = std::fabs( (double) x[i] );
        if( a < silenceLevel_ ) {
            ++gap;
            if( gap > worstGap ) { worstGap = gap; worstGapAt = startFrame_ + (qint64) i - gap + 1; }
        } else {
            gap = 0;
        }
        if( i > 0 ) {
            const double step = std::fabs( (double) x[i] - (double) x[i - 1] );
            if( step > worstStep ) { worstStep = step; worstStepAt = startFrame_ + (qint64) i; }
        }
    }

    bool ok = true;
    if( maxGapFrames_ >= 0 && worstGap > maxGapFrames_ ) {
        qWarning() << "assert-audio-continuity: longest silent run" << worstGap
                   << "frames at" << worstGapAt << "exceeds" << maxGapFrames_;
        ok = false;
    }
    if( maxStep_ >= 0.0 && worstStep > maxStep_ ) {
        qWarning() << "assert-audio-continuity: largest sample step" << worstStep
                   << "at" << worstStepAt << "exceeds" << maxStep_;
        ok = false;
    }
    if( !ok ) return { false, nullptr };
    qDebug() << "assert-audio-continuity: longest gap" << worstGap
             << "frames, largest step" << worstStep << "over" << (qint64) x.size()
             << "frames from" << startFrame_;
    return { true, nullptr };
}

void SAssertAudioContinuityAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "filename", filename_ );
    if( channel_ != 0 )        elem.setAttribute( "channel", QString::number( channel_ ) );
    if( startFrame_ != 0 )     elem.setAttribute( "startFrame", QString::number( startFrame_ ) );
    if( frameCount_ != -1 )    elem.setAttribute( "frameCount", QString::number( frameCount_ ) );
    if( maxGapFrames_ >= 0 )   elem.setAttribute( "maxGapFrames", QString::number( maxGapFrames_ ) );
    if( maxStep_ >= 0.0 )      elem.setAttribute( "maxStep", QString::number( maxStep_ ) );
    if( silenceLevel_ != 0.001 )
        elem.setAttribute( "silenceLevel", QString::number( silenceLevel_ ) );
}

bool SAssertAudioContinuityAction::readXml( const QDomElement &elem, int )
{
    filename_     = elem.attribute( "filename", "" );
    channel_      = elem.attribute( "channel", "0" ).toInt();
    startFrame_   = elem.attribute( "startFrame", "0" ).toLongLong();
    frameCount_   = elem.attribute( "frameCount", "-1" ).toLongLong();
    maxGapFrames_ = elem.attribute( "maxGapFrames", "-1" ).toLongLong();
    maxStep_      = elem.attribute( "maxStep", "-1" ).toDouble();
    silenceLevel_ = elem.attribute( "silenceLevel", "0.001" ).toDouble();
    return true;
}

// --- assert-render-policy ---------------------------------------------------

QStringList SAssertRenderPolicyAction::knownAttributes() const
{
    return { QStringLiteral( "liveThreadRefusals" ),
             QStringLiteral( "liveOwnedRefusals" ) };
}

SApplyResult SAssertRenderPolicyAction::apply( SProject * )
{
    const qint64 liveThread = (qint64) twRtThreadGuard::liveThreadRefusals();
    const qint64 liveOwned  = (qint64) SLiveMonitor::liveOwnedRefusals();

    bool ok = true;
    if( liveThread > maxLiveThreadRefusals_ ) {
        qWarning() << "assert-render-policy: liveThreadRefusals" << liveThread
                   << "exceeds" << maxLiveThreadRefusals_
                   << "- the pump reached freezePage";
        ok = false;
    }
    if( liveOwned > maxLiveOwnedRefusals_ ) {
        qWarning() << "assert-render-policy: liveOwnedRefusals" << liveOwned
                   << "exceeds" << maxLiveOwnedRefusals_
                   << "- a freeze arrived at a live-owned processor";
        ok = false;
    }
    if( !ok ) return { false, nullptr };
    qDebug() << "assert-render-policy: liveThreadRefusals" << liveThread
             << "( <=" << maxLiveThreadRefusals_ << "), liveOwnedRefusals"
             << liveOwned << "( <=" << maxLiveOwnedRefusals_ << ")";
    return { true, nullptr };
}

void SAssertRenderPolicyAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "liveThreadRefusals", QString::number( maxLiveThreadRefusals_ ) );
    elem.setAttribute( "liveOwnedRefusals", QString::number( maxLiveOwnedRefusals_ ) );
}

bool SAssertRenderPolicyAction::readXml( const QDomElement &elem, int )
{
    maxLiveThreadRefusals_ = elem.attribute( "liveThreadRefusals", "0" ).toLongLong();
    maxLiveOwnedRefusals_  = elem.attribute( "liveOwnedRefusals", "0" ).toLongLong();
    return true;
}

// --- assert-input-meter -----------------------------------------------------

QStringList SAssertInputMeterAction::knownAttributes() const
{
    return { QStringLiteral( "trackPath" ), QStringLiteral( "minPeak" ),
             QStringLiteral( "maxPeak" ),   QStringLiteral( "contains" ) };
}

SApplyResult SAssertInputMeterAction::apply( SProject *project )
{
    SApplication &app = SApplication::app();
    SLiveMonitor *mon = app.liveMonitor();
    if( !mon ) {
        qWarning() << "assert-input-meter: no live monitor";
        return { false, nullptr };
    }

    STrack *track = nullptr;
    if( project ) {
        SObject *mixer = splacements::rootContainer( project );
        track = dynamic_cast<STrack *>( splacements::laneAt( mixer, trackPath_ ) );
    }
    if( !track ) {
        qWarning() << "assert-input-meter: no track at" << pathToString( trackPath_ );
        return { false, nullptr };
    }

    const double peak = mon->inputPeak( track );
    const QString desc = mon->describe();
    bool ok = true;
    if( minPeak_ >= 0.0 && peak < minPeak_ ) {
        qWarning() << "assert-input-meter: input peak" << peak << "below" << minPeak_
                   << "-" << desc;
        ok = false;
    }
    if( maxPeak_ >= 0.0 && peak > maxPeak_ ) {
        qWarning() << "assert-input-meter: input peak" << peak << "above" << maxPeak_
                   << "-" << desc;
        ok = false;
    }
    if( !contains_.isEmpty() && !desc.contains( contains_ ) ) {
        qWarning() << "assert-input-meter: live state" << desc
                   << "does not contain" << contains_;
        ok = false;
    }
    if( !ok ) return { false, nullptr };
    qDebug() << "assert-input-meter: peak" << peak << "-" << desc;
    return { true, nullptr };
}

void SAssertInputMeterAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "trackPath", pathToString( trackPath_ ) );
    if( minPeak_ >= 0.0 ) elem.setAttribute( "minPeak", QString::number( minPeak_ ) );
    if( maxPeak_ >= 0.0 ) elem.setAttribute( "maxPeak", QString::number( maxPeak_ ) );
    if( !contains_.isEmpty() ) elem.setAttribute( "contains", contains_ );
}

bool SAssertInputMeterAction::readXml( const QDomElement &elem, int )
{
    trackPath_ = stringToPath( elem.attribute( "trackPath" ) );
    minPeak_   = elem.attribute( "minPeak", "-1" ).toDouble();
    maxPeak_   = elem.attribute( "maxPeak", "-1" ).toDouble();
    contains_  = elem.attribute( "contains", "" );
    return true;
}

static const bool s_reg_live_test_actions = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "assert-monitor-latency" ),
        []{ return new SAssertMonitorLatencyAction; } ),
    SActionRegistry::instance().registerType(
        QStringLiteral( "assert-audio-continuity" ),
        []{ return new SAssertAudioContinuityAction; } ),
    SActionRegistry::instance().registerType(
        QStringLiteral( "assert-render-policy" ),
        []{ return new SAssertRenderPolicyAction; } ),
    SActionRegistry::instance().registerType(
        QStringLiteral( "assert-input-meter" ),
        []{ return new SAssertInputMeterAction; } ),
    true );
