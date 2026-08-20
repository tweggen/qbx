#include "app/testkit/slivetestactions.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
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
#include "app/testkit/smidiintestactions.h"
#include "app/testkit/stestfilepath.h"
#include "tw/analysis/audio_analysis.h"
#include "tw/devices/capture_backend.h"
#include "tw/graph/tw_freeze_context.h"
#include "tw/playback/twspeaker.h"

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
             QStringLiteral( "liveOwnedRefusals" ),
             QStringLiteral( "minLiveOwnedRefusals" ) };
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
    if( liveOwned < minLiveOwnedRefusals_ ) {
        qWarning() << "assert-render-policy: liveOwnedRefusals" << liveOwned
                   << "is below the required minimum" << minLiveOwnedRefusals_
                   << "- the ownership guard never fired, so this case proved"
                      " nothing about it";
        ok = false;
    }
    if( !ok ) return { false, nullptr };
    qDebug() << "assert-render-policy: liveThreadRefusals" << liveThread
             << "( <=" << maxLiveThreadRefusals_ << "), liveOwnedRefusals"
             << liveOwned << "( <=" << maxLiveOwnedRefusals_ << ", >="
             << minLiveOwnedRefusals_ << ")";
    return { true, nullptr };
}

void SAssertRenderPolicyAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "liveThreadRefusals", QString::number( maxLiveThreadRefusals_ ) );
    elem.setAttribute( "liveOwnedRefusals", QString::number( maxLiveOwnedRefusals_ ) );
    if( minLiveOwnedRefusals_ > 0 )
        elem.setAttribute( "minLiveOwnedRefusals",
                           QString::number( minLiveOwnedRefusals_ ) );
}

bool SAssertRenderPolicyAction::readXml( const QDomElement &elem, int )
{
    maxLiveThreadRefusals_ = elem.attribute( "liveThreadRefusals", "0" ).toLongLong();
    maxLiveOwnedRefusals_  = elem.attribute( "liveOwnedRefusals", "0" ).toLongLong();
    minLiveOwnedRefusals_  = elem.attribute( "minLiveOwnedRefusals", "0" ).toLongLong();
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
        SObject *mixer = splacements::rootNamed( project, pathRoot_ );
        track = dynamic_cast<STrack *>( splacements::laneAt( mixer, trackPath_ ) );
    }
    if( !track ) {
        qWarning() << "assert-input-meter: no track at" << qualifiedToString( pathRoot_, trackPath_ );
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
    elem.setAttribute( "trackPath", qualifiedToString( pathRoot_, trackPath_ ) );
    if( minPeak_ >= 0.0 ) elem.setAttribute( "minPeak", QString::number( minPeak_ ) );
    if( maxPeak_ >= 0.0 ) elem.setAttribute( "maxPeak", QString::number( maxPeak_ ) );
    if( !contains_.isEmpty() ) elem.setAttribute( "contains", contains_ );
}

bool SAssertInputMeterAction::readXml( const QDomElement &elem, int )
{
    trackPath_ = parseInto( pathRoot_, elem.attribute( "trackPath" ) );
    minPeak_   = elem.attribute( "minPeak", "-1" ).toDouble();
    maxPeak_   = elem.attribute( "maxPeak", "-1" ).toDouble();
    contains_  = elem.attribute( "contains", "" );
    return true;
}

// --- assert-audio-onset -----------------------------------------------------

QStringList SAssertAudioOnsetAction::knownAttributes() const
{
    return { QStringLiteral( "filename" ),   QStringLiteral( "channel" ),
             QStringLiteral( "startFrame" ), QStringLiteral( "threshold" ),
             QStringLiteral( "window" ),     QStringLiteral( "minFrame" ),
             QStringLiteral( "maxFrame" ),   QStringLiteral( "afterMidiIn" ) };
}

SApplyResult SAssertAudioOnsetAction::apply( SProject *project )
{
    if( filename_.isEmpty() ) {
        qWarning() << "assert-audio-onset: filename is required";
        return { false, nullptr };
    }
    QString path;
    if( !resolveOrReject( filename_, project, "assert-audio-onset", path ) )
        return { false, nullptr };

    std::string err;
    int rate = 0;
    std::vector<float> pcm;
    if( !audio::readAudioRegion( path.toStdString(), startFrame_, -1, channel_,
                                 pcm, rate, err )
        || pcm.empty() ) {
        qWarning() << "assert-audio-onset: cannot read" << path
                   << QString::fromStdString( err );
        return { false, nullptr };
    }

    const qint64 w = window_ > 0 ? window_ : 64;
    qint64 onset = -1;
    // A RUNNING window rather than a per-sample threshold: a single sample
    // above the line is a click or a dither bit, and an onset that a case can
    // reason about is where ENERGY begins.
    for( qint64 i = 0; i + w <= (qint64) pcm.size(); ++i ) {
        double sum = 0.0;
        for( qint64 k = 0; k < w; ++k ) sum += (double) pcm[i + k] * pcm[i + k];
        if( std::sqrt( sum / (double) w ) >= threshold_ ) { onset = i; break; }
    }
    if( onset < 0 ) {
        qWarning() << "assert-audio-onset:" << filename_
                   << "never reaches RMS" << threshold_
                   << "- no onset in" << (qint64) pcm.size() << "frames from"
                   << startFrame_;
        return { false, nullptr };
    }
    const qint64 absOnset = startFrame_ + onset;

    qint64 measured = absOnset;
    QString what = QStringLiteral( "onset" );
    if( afterMidiIn_ ) {
        const qint64 injected = smidiin::lastInjectedHostNs();
        if( injected == 0 ) {
            qWarning() << "assert-audio-onset: afterMidiIn=1 but nothing has been"
                          " injected in this run";
            return { false, nullptr };
        }
        std::shared_ptr<twSpeaker> speaker = SApplication::app().getSpeaker();
        audio::AudioBackend *backend = speaker ? speaker->getBackend() : nullptr;
        audio::CaptureBackend *cap =
            dynamic_cast<audio::CaptureBackend *>( backend );
        if( !cap ) {
            qWarning() << "assert-audio-onset: afterMidiIn=1 needs the CAPTURE"
                          " audio backend - there is no block log to map a host"
                          " time through. Set SMARAGD_AUDIO_BACKEND=capture.";
            return { false, nullptr };
        }
        // The dump IS what the device was handed, and frameAtHostTime answers
        // in exactly that domain, so no latency term belongs here: both sides
        // are delivery-frame indices.
        const qint64 refFrame = (qint64) cap->frameAtHostTime( injected );
        measured = absOnset - refFrame;
        what = QStringLiteral( "onset lag (note-on at capture frame %1)" )
                   .arg( refFrame );
    }

    const double ms = rate > 0 ? ( 1000.0 * (double) measured / rate ) : 0.0;
    qInfo().noquote() << QStringLiteral(
        "assert-audio-onset: %1 %2 = %3 frames (%4 ms at %5 Hz)" )
        .arg( filename_ ).arg( what ).arg( measured )
        .arg( QString::number( ms, 'f', 2 ) ).arg( rate );

    if( minFrame_ >= 0 && measured < minFrame_ ) {
        qWarning() << "assert-audio-onset:" << measured << "<" << minFrame_;
        return { false, nullptr };
    }
    if( maxFrame_ >= 0 && measured > maxFrame_ ) {
        qWarning() << "assert-audio-onset:" << measured << ">" << maxFrame_;
        return { false, nullptr };
    }
    return { true, nullptr };
}

void SAssertAudioOnsetAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "filename", filename_ );
    elem.setAttribute( "channel", channel_ );
    elem.setAttribute( "startFrame", QString::number( startFrame_ ) );
    elem.setAttribute( "threshold", QString::number( threshold_ ) );
    elem.setAttribute( "window", QString::number( window_ ) );
    if( minFrame_ >= 0 ) elem.setAttribute( "minFrame", QString::number( minFrame_ ) );
    if( maxFrame_ >= 0 ) elem.setAttribute( "maxFrame", QString::number( maxFrame_ ) );
    if( afterMidiIn_ ) elem.setAttribute( "afterMidiIn", "1" );
}

bool SAssertAudioOnsetAction::readXml( const QDomElement &elem, int )
{
    filename_    = elem.attribute( "filename", "" );
    channel_     = elem.attribute( "channel", "0" ).toInt();
    startFrame_  = elem.attribute( "startFrame", "0" ).toLongLong();
    threshold_   = elem.attribute( "threshold", "0.02" ).toDouble();
    window_      = elem.attribute( "window", "64" ).toLongLong();
    minFrame_    = elem.attribute( "minFrame", "-1" ).toLongLong();
    maxFrame_    = elem.attribute( "maxFrame", "-1" ).toLongLong();
    afterMidiIn_ = elem.attribute( "afterMidiIn", "0" ).toInt() != 0;
    return true;
}

// --- assert-metronome-clicks ------------------------------------------------

QStringList SAssertMetronomeClicksAction::knownAttributes() const
{
    return { QStringLiteral( "filename" ),      QStringLiteral( "channel" ),
             QStringLiteral( "startFrame" ),    QStringLiteral( "threshold" ),
             QStringLiteral( "window" ),        QStringLiteral( "minGapFrames" ),
             QStringLiteral( "count" ),         QStringLiteral( "minCount" ),
             QStringLiteral( "maxCount" ),      QStringLiteral( "accentEvery" ),
             QStringLiteral( "intervalFrames" ),
             QStringLiteral( "toleranceFrames" ), QStringLiteral( "accentRatio" ),
             QStringLiteral( "silenceMaxRms" ), QStringLiteral( "firstFrame" ),
             QStringLiteral( "firstTolerance" ) };
}

SApplyResult SAssertMetronomeClicksAction::apply( SProject *project )
{
    if( filename_.isEmpty() ) {
        qWarning() << "assert-metronome-clicks: filename is required";
        return { false, nullptr };
    }
    QString path;
    if( !resolveOrReject( filename_, project, "assert-metronome-clicks", path ) )
        return { false, nullptr };

    std::string err;
    int rate = 0;
    std::vector<float> pcm;
    if( !audio::readAudioRegion( path.toStdString(), startFrame_, -1, channel_,
                                 pcm, rate, err ) ) {
        qWarning() << "assert-metronome-clicks: cannot read" << path
                   << QString::fromStdString( err );
        return { false, nullptr };
    }

    const qint64 w   = window_ > 0 ? window_ : 64;
    const qint64 gap = minGapFrames_ > 0 ? minGapFrames_ : 1;
    const qint64 n   = (qint64) pcm.size();

    // ONE PASS, running-window RMS, with a dead time after each hit so a 20 ms
    // click is one event rather than a few hundred.
    std::vector<qint64>  onsets;
    std::vector<double>  peaks;
    for( qint64 i = 0; i + w <= n; ++i ) {
        double sum = 0.0;
        for( qint64 k = 0; k < w; ++k ) sum += (double) pcm[i + k] * pcm[i + k];
        if( std::sqrt( sum / (double) w ) < threshold_ ) continue;
        onsets.push_back( startFrame_ + i );
        double pk = 0.0;
        for( qint64 k = i; k < std::min( n, i + gap ); ++k )
            pk = std::max( pk, (double) std::fabs( pcm[(std::size_t) k] ) );
        peaks.push_back( pk );
        i += gap;                       // the dead time
    }

    QStringList report;
    for( std::size_t k = 0; k < onsets.size(); ++k )
        report << QStringLiteral( "%1@%2(pk %3)" )
                      .arg( k ).arg( onsets[k] )
                      .arg( QString::number( peaks[k], 'f', 4 ) );
    qInfo().noquote() << QStringLiteral(
        "assert-metronome-clicks: %1 -> %2 click(s) at %3 Hz: %4" )
        .arg( filename_ ).arg( onsets.size() ).arg( rate )
        .arg( report.join( QStringLiteral( ", " ) ) );

    if( count_ >= 0 && (int) onsets.size() != count_ ) {
        qWarning() << "assert-metronome-clicks: expected" << count_
                   << "click(s), found" << onsets.size();
        return { false, nullptr };
    }
    if( minCount_ >= 0 && (int) onsets.size() < minCount_ ) {
        qWarning() << "assert-metronome-clicks:" << onsets.size() << "click(s) <"
                   << minCount_;
        return { false, nullptr };
    }
    if( maxCount_ >= 0 && (int) onsets.size() > maxCount_ ) {
        qWarning() << "assert-metronome-clicks:" << onsets.size() << "click(s) >"
                   << maxCount_;
        return { false, nullptr };
    }
    if( onsets.empty() ) return { true, nullptr };

    if( firstFrame_ >= 0 && firstTolerance_ >= 0 ) {
        const qint64 d = std::llabs( onsets[0] - firstFrame_ );
        qInfo().noquote() << QStringLiteral(
            "assert-metronome-clicks: first onset %1, expected %2, error %3 frames" )
            .arg( onsets[0] ).arg( firstFrame_ ).arg( d );
        if( d > firstTolerance_ ) {
            qWarning() << "assert-metronome-clicks: first onset off by" << d
                       << ">" << firstTolerance_;
            return { false, nullptr };
        }
    }

    // THE GRID, anchored on the first onset (see the header for why).
    if( intervalFrames_ > 0 && onsets.size() >= 2 ) {
        qint64 worst = 0;
        QStringList errs;
        for( std::size_t k = 1; k < onsets.size(); ++k ) {
            const qint64 ideal = onsets[0] + (qint64) k * intervalFrames_;
            const qint64 e     = onsets[k] - ideal;
            errs << QString::number( e );
            worst = std::max( worst, std::llabs( e ) );
        }
        qInfo().noquote() << QStringLiteral(
            "assert-metronome-clicks: grid errors (frames) = [%1], worst |%2| "
            "against %3" )
            .arg( errs.join( QStringLiteral( ", " ) ) )
            .arg( worst ).arg( tolerance_ );
        if( worst > tolerance_ ) {
            qWarning() << "assert-metronome-clicks: worst grid error" << worst
                       << ">" << tolerance_;
            return { false, nullptr };
        }
    }

    // THE BAR ACCENT. The phase is SEARCHED rather than assumed to be 0: which
    // beat of the bar the first SUMMED ring entry carries depends on when the
    // device started draining, which is the box's business. What is the CODE's
    // business, and is what this asserts, is that one click in every `every` is
    // louder than every other one by at least `accentRatio`.
    if( accentRatio_ > 0.0 && accentEvery_ > 1
        && (int) onsets.size() >= accentEvery_ + 1 ) {
        double best = -1.0;
        int    bestPhase = -1;
        for( int ph = 0; ph < accentEvery_; ++ph ) {
            double minAcc = 1e30, maxOth = 0.0;
            int    nAcc = 0, nOth = 0;
            // FROM 1: the first click of a live-lane session sits inside the
            // RT's fade-in ramp and is attenuated by construction.
            for( std::size_t k = 1; k < peaks.size(); ++k ) {
                if( (int) ( k % (std::size_t) accentEvery_ ) == ph ) {
                    minAcc = std::min( minAcc, peaks[k] ); ++nAcc;
                } else {
                    maxOth = std::max( maxOth, peaks[k] ); ++nOth;
                }
            }
            if( nAcc == 0 || nOth == 0 || maxOth <= 0.0 ) continue;
            const double r = minAcc / maxOth;
            if( r > best ) { best = r; bestPhase = ph; }
        }
        qInfo().noquote() << QStringLiteral(
            "assert-metronome-clicks: accent ratio = %1 at phase %2 (>= %3)" )
            .arg( QString::number( best, 'f', 4 ) ).arg( bestPhase )
            .arg( QString::number( accentRatio_, 'f', 4 ) );
        if( best < accentRatio_ ) {
            qWarning() << "assert-metronome-clicks: best accent ratio" << best
                       << "<" << accentRatio_;
            return { false, nullptr };
        }
    }

    // SILENCE BETWEEN THE CLICKS. Without it "four clicks" is satisfied by a
    // continuous tone with four louder moments in it.
    if( silenceMaxRms_ > 0.0 && onsets.size() >= 2 ) {
        double worst = 0.0;
        for( std::size_t k = 0; k + 1 < onsets.size(); ++k ) {
            const qint64 a = onsets[k] + gap - startFrame_;
            const qint64 b = onsets[k + 1] - startFrame_;
            if( b - a < 2 * w ) continue;
            // The MIDDLE of the gap: the click's own decay tail lives at the
            // start of it, and a release ramp is not a failure.
            const qint64 from = a + ( b - a ) / 3;
            const qint64 to   = b - ( b - a ) / 8;
            double sum = 0.0;
            qint64 cnt = 0;
            for( qint64 i = from; i < to && i < n; ++i, ++cnt )
                sum += (double) pcm[(std::size_t) i] * pcm[(std::size_t) i];
            if( cnt <= 0 ) continue;
            worst = std::max( worst, std::sqrt( sum / (double) cnt ) );
        }
        qInfo().noquote() << QStringLiteral(
            "assert-metronome-clicks: loudest inter-click RMS = %1 (< %2)" )
            .arg( QString::number( worst, 'f', 6 ) )
            .arg( QString::number( silenceMaxRms_, 'f', 6 ) );
        if( worst >= silenceMaxRms_ ) {
            qWarning() << "assert-metronome-clicks: inter-click RMS" << worst
                       << ">=" << silenceMaxRms_;
            return { false, nullptr };
        }
    }
    return { true, nullptr };
}

void SAssertMetronomeClicksAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "filename", filename_ );
    elem.setAttribute( "channel", channel_ );
    elem.setAttribute( "startFrame", QString::number( startFrame_ ) );
    elem.setAttribute( "threshold", QString::number( threshold_ ) );
    elem.setAttribute( "window", QString::number( window_ ) );
    elem.setAttribute( "minGapFrames", QString::number( minGapFrames_ ) );
    if( count_ >= 0 )         elem.setAttribute( "count", count_ );
    if( minCount_ >= 0 )      elem.setAttribute( "minCount", minCount_ );
    if( maxCount_ >= 0 )      elem.setAttribute( "maxCount", maxCount_ );
    if( accentEvery_ > 0 )    elem.setAttribute( "accentEvery", accentEvery_ );
    if( intervalFrames_ > 0 ) elem.setAttribute( "intervalFrames",
                                                 QString::number( intervalFrames_ ) );
    elem.setAttribute( "toleranceFrames", QString::number( tolerance_ ) );
    if( accentRatio_ > 0.0 )   elem.setAttribute( "accentRatio",
                                                  QString::number( accentRatio_ ) );
    if( silenceMaxRms_ > 0.0 ) elem.setAttribute( "silenceMaxRms",
                                                  QString::number( silenceMaxRms_ ) );
    if( firstFrame_ >= 0 )     elem.setAttribute( "firstFrame",
                                                  QString::number( firstFrame_ ) );
    if( firstTolerance_ >= 0 ) elem.setAttribute( "firstTolerance",
                                                  QString::number( firstTolerance_ ) );
}

bool SAssertMetronomeClicksAction::readXml( const QDomElement &elem, int )
{
    filename_       = elem.attribute( "filename", "" );
    channel_        = elem.attribute( "channel", "0" ).toInt();
    startFrame_     = elem.attribute( "startFrame", "0" ).toLongLong();
    threshold_      = elem.attribute( "threshold", "0.05" ).toDouble();
    window_         = elem.attribute( "window", "64" ).toLongLong();
    minGapFrames_   = elem.attribute( "minGapFrames", "4800" ).toLongLong();
    count_          = elem.attribute( "count", "-1" ).toInt();
    minCount_       = elem.attribute( "minCount", "-1" ).toInt();
    maxCount_       = elem.attribute( "maxCount", "-1" ).toInt();
    accentEvery_    = elem.attribute( "accentEvery", "0" ).toInt();
    intervalFrames_ = elem.attribute( "intervalFrames", "0" ).toLongLong();
    tolerance_      = elem.attribute( "toleranceFrames", "1024" ).toLongLong();
    accentRatio_    = elem.attribute( "accentRatio", "0" ).toDouble();
    silenceMaxRms_  = elem.attribute( "silenceMaxRms", "0" ).toDouble();
    firstFrame_     = elem.attribute( "firstFrame", "-1" ).toLongLong();
    firstTolerance_ = elem.attribute( "firstTolerance", "-1" ).toLongLong();
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
    SActionRegistry::instance().registerType(
        QStringLiteral( "assert-audio-onset" ),
        []{ return new SAssertAudioOnsetAction; } ),
    SActionRegistry::instance().registerType(
        QStringLiteral( "assert-metronome-clicks" ),
        []{ return new SAssertMetronomeClicksAction; } ),
    true );
