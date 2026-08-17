#include "app/shell/saudiorecorder.h"

#include <algorithm>

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTimer>
#include <QUndoStack>

#include "app/actions/sactionhistory.h"
#include "app/model/slink.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/model/sprojectprops.h"
#include "app/objects/cut/scut.h"
#include "app/objects/cut/splacerecordingaction.h"
#include "app/objects/track/strack.h"
#include "app/objects/wave/srecordingcontent.h"
#include "app/shell/sapplication.h"
#include "app/shell/slivemonitor.h"
#include "app/shell/ssettings.h"

#include "tw/core/twlog.h"
#include "tw/devices/audio_backend.h"
#include "tw/graph/tw303aenv.h"
#include "tw/playback/twspeaker.h"
#include "tw/record/capture_bridge.h"

namespace {

// The UI tick. 10 Hz is design D7's "growth emits durationChanged at ~10 Hz",
// and it is also fast enough that the anchor is taken within a tenth of a
// second of the first publication.
constexpr int kTickMs = 100;

// Depth-first, root-relative paths. The ORDER is the contract between the
// capture (one WAV sink per armed track, in this order) and the placement
// (one place-recording per armed track, in this order), so both sides call
// this one function -- exactly as SMainWindow's old helper did.
void collectArmed( SObject *container, const QList<int> &path,
                   std::vector<std::pair<STrack *, QList<int> > > &out )
{
    if( !container ) return;
    for( int i = 0; i < container->childCount(); ++i ) {
        SLink *lk = container->childAt( i );
        if( !lk ) continue;
        SObject *child = &lk->getSObject();
        if( !child->isPathContainer() ) continue;
        QList<int> p = path;
        p.append( i );
        if( STrack *t = dynamic_cast<STrack *>( child ) ) {
            // A MIDI-armed track belongs to SMidiRecorder (proposal 21 L4).
            // The split is by TRACK INPUT, not by two record buttons - and it
            // matters here in particular, because without it a MIDI-armed
            // track would get an audio WAV sink and a growing audio clip out
            // of an input device it never asked for.
            if( t->isArmedForRecording() && !t->hasMidiTrackInput() )
                out.push_back( { t, p } );
        }
        collectArmed( child, p, out );
    }
}

}  // namespace

SAudioRecorder::SAudioRecorder( SApplication *app, QObject *parent )
    : QObject( parent ), app_( app )
{
    timer_ = new QTimer( this );
    timer_->setInterval( kTickMs );
    connect( timer_, &QTimer::timeout, this, &SAudioRecorder::poll );
}

SAudioRecorder::~SAudioRecorder()
{
    if( active_ ) stop();
}

double SAudioRecorder::projectRate_() const
{
    const int r = ( app_ && app_->get303aEnvironment() )
                      ? app_->get303aEnvironment()->getSRate() : 48000;
    return r > 0 ? (double) r : 48000.0;
}

// ---------------------------------------------------------------- regions --

bool SAudioRecorder::cycleRegion_( offset_t &in, offset_t &out ) const
{
    SProject *p = app_ ? app_->getCurrentProject() : nullptr;
    if( !p ) return false;
    if( !p->prop( SProjectProps::Cycle, false ).toBool() ) return false;
    if( !p->prop( SProjectProps::RangeValid, false ).toBool() ) return false;
    in  = (offset_t) p->prop( SProjectProps::RangeStart, (qulonglong) 0 ).toULongLong();
    out = (offset_t) p->prop( SProjectProps::RangeEnd,   (qulonglong) 0 ).toULongLong();
    return out > in;
}

bool SAudioRecorder::punchRegion_( offset_t &in, offset_t &out ) const
{
    // The project has ONE range, and what it means to a take depends on the
    // cycle flag: with Cycle on it is the LOOP (one take per pass), with Cycle
    // off it is the PUNCH region (record only inside it). One marker pair, two
    // readings, no second UI -- and the same pair the render dialog's "time
    // selection" and the ruler already show.
    offset_t a = 0, b = 0;
    if( cycleRegion_( a, b ) ) return false;
    SProject *p = app_ ? app_->getCurrentProject() : nullptr;
    if( !p ) return false;
    if( !p->prop( SProjectProps::RangeValid, false ).toBool() ) return false;
    in  = (offset_t) p->prop( SProjectProps::RangeStart, (qulonglong) 0 ).toULongLong();
    out = (offset_t) p->prop( SProjectProps::RangeEnd,   (qulonglong) 0 ).toULongLong();
    return out > in;
}

// ------------------------------------------------------------------ start --

bool SAudioRecorder::collectArmed_()
{
    armed_.clear();
    SProject *project = app_ ? app_->getCurrentProject() : nullptr;
    if( !project ) return false;
    std::vector<std::pair<STrack *, QList<int> > > found;
    collectArmed( project->getRootComponent(), QList<int>(), found );
    for( const auto &pr : found ) {
        Armed a;
        a.track = pr.first;
        a.path  = pr.second;
        a.mask  = pr.first->getRecordingChannels();
        armed_.push_back( a );
    }
    return !armed_.empty();
}

bool SAudioRecorder::start()
{
    if( active_ ) return true;
    error_.clear();
    trimmed_ = 0;
    lastPassCount_ = 0;
    lastFiles_.clear();
    placement_ = SRecordPlacement();

    SProject *project = app_ ? app_->getCurrentProject() : nullptr;
    if( !project ) { error_ = QStringLiteral( "no project" ); return false; }
    if( !collectArmed_() ) {
        error_ = QStringLiteral( "no armed tracks" );
        TW_LOGW( "shell", "[REC] start refused: no armed tracks" );
        return false;
    }

    recordStart_ = app_->getGlobalLocatorPos();
    wasPlaying_  = app_->isPlaying();
    punch_ = punchRegion_( punchIn_, punchOut_ );
    cycle_ = cycleRegion_( cycleIn_, cycleOut_ );

    // 1. THE INPUT. One pump, borrowed from the monitor (design D7).
    //
    // THE DEVICE NAME IS RESOLVED THE WAY THE MONITOR RESOLVES IT, and in the
    // same order: the armed track's own `trackInput` device first, then
    // whatever the monitor already has open, then the settings default. Asking
    // for the settings default unconditionally is what the first draft did,
    // and it CLOSED AND REOPENED THE DEVICE at every record start on a track
    // whose input was named explicitly -- a monitoring gap in exactly the case
    // "one input pump" exists to prevent, plus a device-change fight with the
    // monitor for the rest of the take.
    SLiveMonitor *mon = app_->liveMonitor();
    deviceName_.clear();
    if( !armed_.empty() && armed_.front().track )
        deviceName_ = armed_.front().track->trackInputAudioDevice();
    if( deviceName_.isEmpty() && mon ) deviceName_ = mon->inputDeviceId();
    if( deviceName_.isEmpty() )
        deviceName_ = SSettings::instance().audioInputDeviceId();
    if( deviceName_.isEmpty() ) deviceName_ = QStringLiteral( "default" );
    bridge_ = mon ? mon->acquireBridge( deviceName_ ) : nullptr;
    if( !bridge_ ) {
        error_ = QStringLiteral( "input device would not open" );
        TW_LOGW( "shell", "[REC] start refused: %s",
                 error_.toStdString().c_str() );
        return false;
    }

    // 2. THE FILES: one per armed track, named by wall clock, written where a
    //    test can find them and where a -j run cannot collide (the test output
    //    dir when there is one, then the project's own directory).
    QString dir = app_->testOutputDir();
    if( dir.isEmpty() ) {
        const QString pf = project->projectFilePath();
        if( !pf.isEmpty() ) dir = QFileInfo( pf ).absolutePath();
    }
    if( dir.isEmpty() )
        dir = QStandardPaths::writableLocation( QStandardPaths::DocumentsLocation );
    QDir().mkpath( dir );

    const QString stamp =
        QDateTime::currentDateTime().toString( QStringLiteral( "yyyyMMdd_HHmmss_zzz" ) );
    std::vector<audio::CaptureWavSink> sinks;
    for( std::size_t i = 0; i < armed_.size(); ++i ) {
        armed_[i].file = QStringLiteral( "%1/%2_track%3.wav" )
                             .arg( dir ).arg( stamp ).arg( (int) i );
        audio::CaptureWavSink s;
        s.path        = armed_[i].file.toStdString();
        s.channelMask = armed_[i].mask;
        sinks.push_back( s );
    }
    if( !bridge_->beginCapture( sinks ) ) {
        error_ = QString::fromUtf8( bridge_->errorMessage() );
        TW_LOGW( "shell", "[REC] beginCapture failed: %s",
                 error_.toStdString().c_str() );
        if( mon ) mon->releaseBridge();
        bridge_ = nullptr;
        return false;
    }

    // 3. THE PLACEMENT TERMS that are known now. P0 and the output latency
    //    both arrive with the anchor (see tryAnchor_).
    placement_.inputLatencyProj = (std::int64_t)
        ( (double) bridge_->inputLatencyFrames() * projectRate_()
          / (double) ( bridge_->inputRate() ? bridge_->inputRate() : 48000u )
          + 0.5 );
    placement_.outputLatencyProj = 0;   // read with the anchor, see tryAnchor_
    placement_.setUserOffsetMs(
        SSettings::instance().recordingOffsetMs( deviceName_ ), projectRate_() );

    startPublishSeq_ = app_->locatorPublishSeq();

    // 4. THE GROWING CLIPS.
    content_ = new SRecordingContent( project, bridge_->source(), 0 );
    placeGrowingClips_();

    active_ = true;
    app_->setRecordingStartFrame( recordStart_ );

    // 5. THE TRANSPORT, through the ONE funnel (design D7). LAST, and after
    //    active_ is set, for two reasons: everything that cares that a run
    //    began — the MIDI-out pump's anchor, the automation recorder's pass
    //    bounds, the live monitor's feed policy — learns about it here and
    //    nowhere else (the old path set isPlaying_ directly and told none of
    //    them); and monitor AUTO is "input while stopped OR RECORDING" (design
    //    D9), so the plan has to be built with isRecordingActive() already
    //    true or Play would take the input away from the take.
    if( !wasPlaying_ ) app_->setPlaybackRunning( true );
    else               app_->liveLanesChanged();
    timer_->start();

    TW_LOGI( "shell", "[REC] started at %lld: %u track(s), inLat=%lld outLat=%lld "
                      "userOff=%lld frames%s%s",
             (long long) recordStart_, (unsigned) armed_.size(),
             (long long) placement_.inputLatencyProj,
             (long long) placement_.outputLatencyProj,
             (long long) placement_.userOffsetProj,
             cycle_ ? " [cycle]" : "", punch_ ? " [punch]" : "" );
    return true;
}

void SAudioRecorder::placeGrowingClips_()
{
    SProject *project = app_->getCurrentProject();
    SObject *mixer = splacements::rootContainer( project );
    for( Armed &a : armed_ ) {
        SObject *lane = splacements::laneAt( mixer, a.path );
        if( !lane ) continue;
        SCut *cut = new SCut( project, *content_ );
        cut->setSName( QStringLiteral( "Recording" ) );
        SLink *link = new SLink( *cut, NULL );
        link->setStartTime( recordStart_ );
        link->setParent( lane );
        a.cut  = cut;
        a.link = link;
    }
}

void SAudioRecorder::removeGrowingClips_()
{
    for( Armed &a : armed_ ) {
        if( !a.link ) continue;
        // setParent(nullptr) first: ~SLink must not run its childRemoved
        // notification through a half-torn vtable (the delete-a-clip freeze).
        a.link->setParent( nullptr );
        delete a.link;
        a.link = nullptr;
        a.cut  = nullptr;
    }
    content_ = nullptr;   // the last cut's content link released it
}

// ------------------------------------------------------------------- tick --

void SAudioRecorder::tryAnchor_()
{
    if( placement_.anchored || !bridge_ ) return;
    const std::int64_t h0 = bridge_->captureStartHostNs();
    if( h0 == 0 ) return;                       // no frames yet

    std::shared_ptr<twSpeaker> spk = app_->getSpeaker();
    if( !spk ) return;
    const twEnginePosition anchor = spk->engineClock().read();
    // A publication from THIS run. The counter is process-global and monotone,
    // so a stale one from the previous run would anchor on a playhead that no
    // longer exists -- the same trap SMidiOutPump::resetRun records.
    if( !anchor.valid() || anchor.seq <= startPublishSeq_ ) return;

    // THE OUTPUT LATENCY, read HERE and not at start(): a take begun from a
    // STOPPED transport opens the device as part of starting, so at start()
    // there is no backend to ask and the term would silently be 0. By the time
    // the clock has published, the device is open by construction. The scaling
    // is exactly meterLatencyFrames()'s (proposal 34) -- restated rather than
    // called, because that accessor answers 0 unless isPlaying_ and reads the
    // number for a different purpose.
    if( audio::AudioBackend *b = spk->getBackend() ) {
        const std::uint32_t devLat  = b->getLatencyFrames();
        const std::uint32_t devRate = b->getConfig().sampleRate;
        placement_.outputLatencyProj =
            ( devRate > 0 )
                ? (std::int64_t)( (double) devLat * projectRate_()
                                  / (double) devRate + 0.5 )
                : (std::int64_t) devLat;
    }

    placement_.p0 = srecord::projectFrameAtHostNs( anchor, h0, projectRate_() );
    placement_.anchored = true;

    // The frames whose placement lands before THE TRANSPORT START are TRIMMED
    // (design D6). See `wasPlaying_`: the floor is the record start only when
    // this take started the transport — in which case it is normal for the
    // trim to be large, because every frame captured while the readahead
    // primed lands before the playhead began to move.
    const std::int64_t floor = wasPlaying_ ? 0 : (std::int64_t) recordStart_;
    const std::int64_t first = placement_.placementFrame( 0 );
    trimmed_ = ( first < floor ) ? ( floor - first ) : 0;

    TW_LOGI( "shell", "[REC] anchored: P0=%lld (anchor frame=%lld host=%lld, "
                      "capture host=%lld) comp=%lld trim=%lld",
             (long long) placement_.p0, (long long) anchor.deliveredFrame,
             (long long) anchor.hostNs, (long long) h0,
             (long long) placement_.compensationFrames(),
             (long long) trimmed_ );
    reseatClips_();
}

void SAudioRecorder::reseatClips_()
{
    const offset_t at = (offset_t) std::max<std::int64_t>(
        placement_.placementFrame( trimmed_ ), 0 );
    for( Armed &a : armed_ )
        if( a.link ) a.link->setStartTime( at );
}

void SAudioRecorder::poll()
{
    if( !active_ ) return;

    tryAnchor_();

    if( content_ ) {
        const length_t len = content_->publishGrowth();
        const length_t shown =
            ( len > (length_t) trimmed_ ) ? ( len - (length_t) trimmed_ ) : 0;
        for( Armed &a : armed_ )
            if( a.cut && shown > 0 ) a.cut->setDuration( shown );
    }

    // PUNCH-OUT. The take ends by itself once its placement passes the out
    // point; there is no reason to keep filling pages that will be discarded.
    if( punch_ && placement_.anchored && content_ ) {
        const std::int64_t end =
            placement_.placementFrame( (std::int64_t) content_->availableFrames() );
        if( end >= (std::int64_t) punchOut_ ) {
            TW_LOGI( "shell", "[REC] punch-out at %lld", (long long) punchOut_ );
            stop();
        }
    }
}

double SAudioRecorder::recordedSeconds() const
{
    if( !content_ ) return 0.0;
    return (double) content_->availableFrames() / projectRate_();
}

// -------------------------------------------------------------------- stop --

void SAudioRecorder::stop()
{
    if( !active_ ) return;
    active_ = false;
    timer_->stop();

    // The capture ends FIRST: endCapture() finalises every file out of the
    // pages (tw/record invariant 3), so the paths place-recording is about to
    // resolve are complete files by the time it looks at them.
    if( bridge_ ) bridge_->endCapture();
    if( content_ ) content_->freeze();
    if( !placement_.anchored ) {
        // Never published: the device produced nothing, or the run was shorter
        // than a device buffer. Place at the record start and say so.
        TW_LOGW( "shell", "[REC] stopped with no clock anchor; placing at the "
                          "record start" );
        placement_.p0 = (std::int64_t) recordStart_
                      - placement_.compensationFrames();
        placement_.anchored = true;
        trimmed_ = 0;
    }

    commitPlacement_();

    if( SLiveMonitor *mon = app_->liveMonitor() ) mon->releaseBridge();
    bridge_ = nullptr;

    if( app_->isPlaying() ) app_->setPlaybackRunning( false );
    // Line the playhead up with what was just placed (it advanced during the
    // take).
    app_->setGlobalLocatorPos( recordStart_ );
}

void SAudioRecorder::commitPlacement_()
{
    SProject *project = app_ ? app_->getCurrentProject() : nullptr;
    const std::int64_t total =
        content_ ? (std::int64_t) content_->availableFrames() : 0;
    const std::int64_t placeable = total - trimmed_;

    // The growing clips go FIRST and outside the macro's undo record: they are
    // transient UI state (like the arm flag), they are about to be replaced by
    // the real thing, and leaving them in place would make place-recording see
    // them as covering columns and stack a take on them.
    removeGrowingClips_();

    lastFiles_.clear();
    for( const Armed &a : armed_ ) lastFiles_.append( a.file );

    if( !project || placeable <= 0 ) {
        lastPassCount_ = 0;
        TW_LOGW( "shell", "[REC] nothing to place (captured %lld, trimmed %lld)",
                 (long long) total, (long long) trimmed_ );
        for( Armed &a : armed_ )
            if( a.track ) a.track->setArmedForRecording( false );
        armed_.clear();
        return;
    }

    // The take's span in PROJECT frames, unwrapped: capture frame
    // `trimmed_ + i` lands at `firstPlace + i`.
    const std::int64_t firstPlace = placement_.placementFrame( trimmed_ );

    // Plan the segments: (project position, source offset, length). One
    // segment normally; one per LOOP PASS when cycling (design D7: "loop record
    // = one take per pass ... proposal 17's phase 5 falls out"); the intersection
    // with the punch region when punching.
    struct Seg { offset_t at; offset_t srcOffset; length_t len; };
    std::vector<Seg> segs;

    if( cycle_ ) {
        const std::int64_t loopLen = (std::int64_t) ( cycleOut_ - cycleIn_ );
        std::int64_t consumed = 0;
        while( consumed < placeable && loopLen > 0 ) {
            const std::int64_t unwrapped = firstPlace + consumed;
            std::int64_t rel = unwrapped - (std::int64_t) cycleIn_;
            if( rel < 0 ) rel = 0;              // before the loop: one flat pass
            const std::int64_t inLoop = rel % loopLen;
            const std::int64_t at     = (std::int64_t) cycleIn_ + inLoop;
            const std::int64_t len =
                std::min<std::int64_t>( placeable - consumed, loopLen - inLoop );
            if( len <= 0 ) break;
            segs.push_back( { (offset_t) at,
                              (offset_t) ( trimmed_ + consumed ),
                              (length_t) len } );
            consumed += len;
        }
    } else if( punch_ ) {
        const std::int64_t a = std::max<std::int64_t>( firstPlace, punchIn_ );
        const std::int64_t b = std::min<std::int64_t>( firstPlace + placeable,
                                                       punchOut_ );
        if( b > a )
            segs.push_back( { (offset_t) a,
                              (offset_t) ( trimmed_ + ( a - firstPlace ) ),
                              (length_t) ( b - a ) } );
    } else {
        segs.push_back( { (offset_t) std::max<std::int64_t>( firstPlace, 0 ),
                          (offset_t) trimmed_, (length_t) placeable } );
    }
    lastPassCount_ = (int) segs.size();

    // ONE UNDO STEP for the whole pass. place-recording plans each segment
    // against the lane it lands on -- a take on every covered column, plain
    // cuts for the gaps -- so a second loop pass over the first one's clip
    // becomes a take by the verb's own rules and needs no special case here.
    QUndoStack *undo = app_->actionHistory() ? app_->actionHistory()->undoStack()
                                             : nullptr;
    if( undo ) undo->beginMacro( QStringLiteral( "Recording" ) );
    for( const Armed &a : armed_ ) {
        if( !QFileInfo( a.file ).exists() ) {
            TW_LOGW( "shell", "[REC] no file for track: %s",
                     a.file.toStdString().c_str() );
            continue;
        }
        for( const Seg &s : segs )
            app_->submitAction( new SPlaceRecordingAction(
                a.path, a.file, s.at, s.srcOffset, s.len ) );
    }
    if( undo ) undo->endMacro();

    for( Armed &a : armed_ )
        if( a.track ) a.track->setArmedForRecording( false );

    TW_LOGI( "shell", "[REC] placed %u segment(s) x %u track(s) from frame %lld "
                      "(captured %lld, trimmed %lld)",
             (unsigned) segs.size(), (unsigned) armed_.size(),
             (long long) firstPlace, (long long) total, (long long) trimmed_ );
    armed_.clear();
}

QString SAudioRecorder::describe() const
{
    return QStringLiteral( "rec=%1 start=%2 p0=%3 anchored=%4 inLat=%5 outLat=%6 "
                           "userOff=%7 comp=%8 trim=%9 passes=%10" )
        .arg( active_ ? "on" : "off" )
        .arg( (qlonglong) recordStart_ )
        .arg( (qlonglong) placement_.p0 )
        .arg( placement_.anchored ? 1 : 0 )
        .arg( (qlonglong) placement_.inputLatencyProj )
        .arg( (qlonglong) placement_.outputLatencyProj )
        .arg( (qlonglong) placement_.userOffsetProj )
        .arg( (qlonglong) placement_.compensationFrames() )
        .arg( (qlonglong) trimmed_ )
        .arg( lastPassCount_ );
}
