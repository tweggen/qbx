#include "app/shell/saudiorecorder.h"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

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
#include "app/objects/track/srecordmode.h"
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
        if( !child->isLane() ) continue;
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
    // THE SAME NAME KEYS THE USER'S CALIBRATION, further down: the offset is
    // read as `audio/recordingOffsetMs/<deviceName_>`, which is exactly what
    // SOptionsDialog::applyAudioPage() writes under (the input combo's device
    // id). Log it -- a lookup that misses returns 0.0, which is
    // indistinguishable from a calibration of zero, so without this line a
    // take that silently used the wrong key looks exactly like an uncalibrated
    // device.
    TW_LOGI( "shell", "[REC] input device '%s' (recordingOffsetMs key)",
             deviceName_.toStdString().c_str() );
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
    if( dir.isEmpty() ) dir = project->projectDir();
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
        // `placementLaneAt`, not `laneAt`, FOR SYMMETRY WITH THE VERB (plan 53
        // B6). `place-recording` already refuses a lane that does not accept
        // clips, and announces it; the growing clip is the same question asked
        // half a second earlier, so asking it the same way means the preview
        // and the placement can never disagree about where a pass may land.
        //
        // NO REACHABLE GATE, and it is worth saying rather than inventing one:
        // `arm-track` already refuses a system lane
        // (`splacements::refuseSystemLane`, proposal 45 AC5.2), so `armed_`
        // cannot hold one, and `acceptsClips()` is true for every other lane.
        // This is correctness against the next lane kind, not a fix for
        // something a case can reach today.
        SObject *lane = splacements::placementLaneAt( mixer, a.path,
                                                      "place-recording" );
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
    // Issue g (fix/loop-behaviour): this is the RECORD-START anchor, and it
    // is valid only for the non-cycle case. While cycling, every growing
    // clip's timeline position is CYCLE_IN — poll()'s per-tick pass window
    // below is what seats it there, once anchored, and reseating here to
    // placementFrame(trimmed_) would yank it back to the (pre-loop) record
    // start on the very tick the anchor lands.
    if( cycle_ ) return;
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

        // Issue g (fix/loop-behaviour): while recording a loop, the growing
        // preview must never overshoot the cycle region -- it wraps and
        // restarts at cycleIn_ instead, redrawing the CURRENT pass in place
        // of the previous one (self-overwriting: one link per armed track,
        // reused for every pass, exactly as the non-cycle path already
        // reuses it). This mirrors, LIVE, the SAME pass arithmetic
        // commitPlacement_() applies once at stop() -- loopLen = cycleOut_ -
        // cycleIn_, rel = (firstPlace + consumed) - cycleIn_ clamped to >= 0,
        // inLoop = rel mod loopLen -- so at every instant the growing clip
        // shows exactly what THIS pass would commit as if the take stopped
        // right now: a leading silence (via a possibly-negative source
        // offset) for a pass that started mid-cycle (the record_loop_late_
        // start shape, G5), filling in with real audio as it streams in.
        //
        // Only reachable once ANCHORED: firstPlace needs P0. Before that,
        // the clip stays at recordStart_ (placeGrowingClips_'s seat) and
        // grows plainly, same as the non-cycle path -- there is no pass to
        // compute yet.
        //
        // cycleIn_/cycleOut_ are READ ONCE AT start() and never refreshed
        // here, matching this class's existing policy for punch_ (see the
        // header comment on punchIn_/punchOut_): a cycle-marker edit mid-take
        // must not move the goalposts under a take already in flight.
        if( cycle_ && placement_.anchored && cycleOut_ > cycleIn_ ) {
            const std::int64_t loopLen  = (std::int64_t) ( cycleOut_ - cycleIn_ );
            const std::int64_t firstPlace = placement_.placementFrame( trimmed_ );
            std::int64_t rel = firstPlace + (std::int64_t) shown
                              - (std::int64_t) cycleIn_;
            if( rel < 0 ) rel = 0;
            std::int64_t inLoop = rel % loopLen;
            if( inLoop < 0 ) inLoop = 0;                 // defensive
            if( inLoop > loopLen ) inLoop = loopLen;      // defensive (see above)
            const offset_t passSrcOffset = (offset_t) ( (std::int64_t) trimmed_
                                                        + (std::int64_t) shown
                                                        - inLoop );
            for( Armed &a : armed_ ) {
                if( !a.link || !a.cut ) continue;
                if( a.link->getStartTime() != (offset_t) cycleIn_ )
                    a.link->setStartTime( (offset_t) cycleIn_ );
                if( a.cut->getSrcStart() != Fraction( passSrcOffset ) )
                    a.cut->setStartOffset( passSrcOffset );
                if( (length_t) inLoop != a.cut->getDuration() )
                    a.cut->setDuration( (length_t) inLoop );
            }
        } else if( shown > 0 ) {
            for( Armed &a : armed_ )
                if( a.cut ) a.cut->setDuration( shown );
        }
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

// -------------------------------------------------------- the pass-wide snap --

std::int64_t SAudioRecorder::snapWindowFrames_() const
{
    // 10 ms is the floor, and one device buffer the other candidate: a take
    // cannot be placed more accurately than the buffer the device hands over
    // in one go, so anything inside it is quantisation noise rather than
    // intent. Never a setting (plan 53 §3.5) -- a user who could widen it
    // could make the snap swallow real material.
    const double rate = projectRate_();
    std::int64_t n = (std::int64_t)( rate * 0.010 + 0.5 );
    if( std::shared_ptr<twSpeaker> spk = app_ ? app_->getSpeaker() : nullptr ) {
        if( audio::AudioBackend *b = spk->getBackend() ) {
            const std::uint32_t devBuf  = b->getConfig().bufferFrames;
            const std::uint32_t devRate = b->getConfig().sampleRate;
            // Scaled to PROJECT frames, exactly as the output-latency term is.
            const std::int64_t buf =
                ( devRate > 0 )
                    ? (std::int64_t)( (double) devBuf * rate / (double) devRate
                                      + 0.5 )
                    : (std::int64_t) devBuf;
            n = std::max( n, buf );
        }
    }
    return n;
}

void SAudioRecorder::snapPassWide_( std::int64_t *a, std::int64_t *b ) const
{
    if( !a || !b ) return;
    const std::int64_t n = snapWindowFrames_();
    if( n <= 0 ) return;

    SProject *project = app_ ? app_->getCurrentProject() : nullptr;
    SObject *mixer = splacements::rootContainer( project );
    if( !mixer ) return;

    // Every child edge on every ARMED lane. In takes modes this set also
    // covers the un-armed members of an armed lane's edit group -- that is
    // P4's (plan 53 §8 M5), and until the recorder enumerates them there is
    // nothing to add here.
    std::vector<std::int64_t> edges;
    for( const Armed &arm : armed_ ) {
        SObject *lane = splacements::laneAt( mixer, arm.path );
        if( !lane ) continue;
        for( int i = 0; i < lane->childCount(); ++i ) {
            SLink *lk = lane->childAt( i );
            if( !lk ) continue;
            SObject &obj = lk->getSObject();
            if( obj.isLane() ) continue;
            // A GROWING CLIP IS NOT AN EDGE, whichever recorder owns it. Ours
            // are gone by now (removeGrowingClips_ runs first); another
            // recorder's are not.
            if( obj.isLiveRecording() ) continue;
            if( !obj.hasDuration() ) continue;
            const std::int64_t s = (std::int64_t) lk->getStartTime();
            const std::int64_t d = (std::int64_t) obj.getDurationBlocking();
            if( d == 0 ) continue;
            edges.push_back( s );
            edges.push_back( s + d );
        }
    }
    if( edges.empty() ) return;

    // One pass per endpoint, and the two are decided SEPARATELY: a pass whose
    // head agrees and whose tail does not gets its head snapped.
    const char *names[2] = { "a", "b" };
    std::int64_t *vals[2] = { a, b };
    for( int k = 0; k < 2; ++k ) {
        std::vector<std::int64_t> near;
        for( std::int64_t e : edges )
            if( std::llabs( e - *vals[k] ) <= n ) near.push_back( e );
        if( near.empty() ) continue;
        const std::int64_t first = near.front();
        bool unanimous = true;
        for( std::int64_t e : near ) if( e != first ) { unanimous = false; break; }
        if( !unanimous ) {
            // Logged with the candidates, because "no snap" and "nothing was
            // near" are different answers and only one of them is a lane
            // disagreeing with another.
            std::string list;
            for( size_t i = 0; i < near.size(); ++i ) {
                if( i ) list += ", ";
                list += std::to_string( (long long) near[i] );
            }
            TW_LOGI( "shell", "[REC] no snap for %s: candidates %s",
                     names[k], list.c_str() );
            continue;
        }
        if( first == *vals[k] ) continue;      // already there; nothing to say
        // A SNAP MAY NEVER EMPTY THE PASS. `a` and `b` are decided
        // independently, and on a pass shorter than N an edge near `b` can sit
        // at or below `a` -- the segment would then be dropped for having no
        // length and the recorded audio would be GONE, which is not something
        // a recorder may do. Refusing the snap leaves a sliver; applying it
        // loses the take.
        if( k == 1 && first <= *vals[0] ) {
            TW_LOGI( "shell", "[REC] no snap for b: %lld would not leave a "
                              "pass after a=%lld",
                     (long long) first, (long long) *vals[0] );
            continue;
        }
        TW_LOGI( "shell", "[REC] snap %s by %+lld to %lld (%u candidate(s) agree)",
                 names[k], (long long)( first - *vals[k] ), (long long) first,
                 (unsigned) near.size() );
        *vals[k] = first;
    }
}

void SAudioRecorder::commitPlacement_()
{
    // PER PASS, like snapDeltaA_: the report describes the pass that just
    // ended, never an accumulation over the session. A punch-out reaches here
    // through its own path and gets its own report.
    skipped_.clear();

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
    // Reset per pass: a cycle or punch pass is never snapped and must report
    // 0 rather than the previous plain pass's delta.
    snapDeltaA_ = 0;

    if( !project || placeable <= 0 ) {
        lastPassCount_ = 0;
        TW_LOGW( "shell", "[REC] nothing to place (captured %lld, trimmed %lld)",
                 (long long) total, (long long) trimmed_ );
        // Arm state is untouched (item e): a take placing nothing is not a
        // reason to disarm the track the user still has armed. The growing
        // clip is already gone (removeGrowingClips_() above), which is what
        // clears the "recording in progress" visual — the head's translucent
        // overlay is gated on SApplication::isRecordingActive(), already
        // false by the time stop() reaches here, not on arm state.
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
    // `extent` is the TAKE's extent where the recorder knows it is wider than
    // the material (plan 53 §3.1 step 5, QBX-180): the cycle length, so a pass
    // that stopped mid-cycle still covers the same bars as the full passes
    // beside it. 0 everywhere else, which is "the placed span".
    struct Seg { offset_t at; offset_t srcOffset; length_t len;
                 length_t extent = 0; };
    std::vector<Seg> segs;

    if( cycle_ ) {
        // ONE TAKE PER PASS, AND EVERY TAKE SPANS THE WHOLE CYCLE REGION.
        //
        // The cycle region is the take region -- what Reaper, Logic and Cubase
        // all do -- so a pass is the unit and the takes on the resulting column
        // are ALTERNATIVES for the same bars, which is the only thing comping
        // between them can mean.
        //
        // IT USED TO CHOP THE PERFORMANCE AT EVERY WRAP and place each piece at
        // its wrapped position, so pieces landing on the same spot became
        // "takes" that were really CONSECUTIVE FRAGMENTS OF ONE PERFORMANCE.
        // Reported from a real session and reproduced: a 4-bar cycle recorded
        // from its LAST bar for just over one pass gave a 2-take column in that
        // last bar -- "one time from the beginning plus the tail" -- and the
        // middle of the performance as a PLAIN CLIP that could not be comped at
        // all. Recording from the loop START never showed it, because there
        // every pass is a whole one; that is the case the suite had.
        //
        // A pass that starts mid-cycle carries a NEGATIVE source offset, which
        // is leading silence (proposal 23), and one that ends early simply runs
        // past the end of the capture, which is silence too. Both halves of a
        // partial pass are therefore the same mechanism.
        const std::int64_t loopLen = (std::int64_t) ( cycleOut_ - cycleIn_ );
        std::int64_t consumed = 0;
        while( consumed < placeable && loopLen > 0 ) {
            const std::int64_t unwrapped = firstPlace + consumed;
            std::int64_t rel = unwrapped - (std::int64_t) cycleIn_;
            if( rel < 0 ) rel = 0;              // before the loop: one flat pass
            const std::int64_t inLoop = rel % loopLen;
            segs.push_back( { (offset_t) cycleIn_,
                              (offset_t) ( trimmed_ + consumed - inLoop ),
                              (length_t) loopLen, (length_t) loopLen } );
            consumed += loopLen - inLoop;       // this pass runs to the wrap
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
        // THE PLAIN PASS IS THE ONLY ONE THAT IS SNAPPED (plan 53 §3.5 rule
        // 3): a punch or cycle span is the markers, and those are exact.
        //
        // The snap moves the WINDOW, so both terms follow from the moved
        // endpoints and nothing re-reads the capture: `a` later by d means d
        // more frames of the file stay hidden in the head handle
        // (srcOffset += d), `a` earlier means d fewer (srcOffset -= d, into
        // the pre-roll handle the trim left, or -- when the handle is shorter
        // than d -- into at most N frames of leading silence, which is a
        // negative source offset and legal since proposal 23).
        std::int64_t a = std::max<std::int64_t>( firstPlace, 0 );
        const std::int64_t unsnappedA = a;
        std::int64_t b = a + placeable;
        snapPassWide_( &a, &b );
        snapDeltaA_ = a - unsnappedA;
        if( b > a )
            segs.push_back( { (offset_t) a,
                              (offset_t) ( trimmed_ + ( a - firstPlace ) ),
                              (length_t) ( b - a ) } );
    }
    lastPassCount_ = (int) segs.size();

    // ONE UNDO STEP for the whole PRESS, and it is no longer opened here
    // (plan 53 §5.4, QBX-165). SApplication::stopRecording arms the scope and
    // the first submitAction inside it opens the macro, so this recorder and
    // the MIDI one land in the SAME step and a pass that places nothing leaves
    // the undo stack untouched. Opening a macro here as well would nest
    // correctly for the count but would re-create the empty-macro problem one
    // level down -- this function returns early, above, whenever placeable
    // <= 0.
    //
    // place-recording still plans each segment against the lane it lands on --
    // a take on every covered column, plain cuts for the gaps -- so a second
    // loop pass over the first one's clip becomes a take by the verb's own
    // rules and needs no special case here.
    // Open the press scope from HERE too (QBX-165). stop() is reached without
    // SApplication::stopRecording() on the punch-out path above, so relying on
    // that function alone would leave a punch-out's placements unwrapped --
    // one undo step per segment per track. The scope is re-entrant, so when
    // stopRecording DID call us this just nests.
    app_->beginRecordCommit();
    for( const Armed &a : armed_ ) {
        if( !QFileInfo( a.file ).exists() ) {
            // The capture file never arrived. The lane gets nothing, and
            // saying so is the whole of plan 53 B6 (QBX-164): this used to be
            // this log line and nothing else, which from the user's side is
            // identical to the take never having been recorded.
            TW_LOGW( "shell", "[REC] no file for track: %s",
                     a.file.toStdString().c_str() );
            skipped_.append( { a.path, a.file,
                               QStringLiteral( "its capture file was never "
                                               "written" ) } );
            continue;
        }
        // THE RECORDER RESOLVES THE MODE, per armed lane, and submits the
        // verb with the answer (plan 53 §5.3). `place-recording` cannot
        // resolve one: `objects/cut` may not depend on `objects/track`, where
        // the folder walk lives (srecordmode.h says so at length).
        const QString mode = srecmode::resolveMode(
            splacements::rootContainer( project ),
            a.track, project );

        // DID THE LANE TAKE THE PASS? `submitAction` returns void and the
        // planner's refusal (plan 53 §5.1, reachable since QBX-172 gave
        // `takes-split` and `replace` the wrapped-column rule) travels no
        // further than a `qWarning`, so the recorder cannot be told directly.
        //
        // Sampling `rejectedCount()` around the submit is how this codebase
        // already answers that question: `SActionHistory`'s own header says
        // "the test runner samples this around each submit to detect rejected
        // actions ... which previously vanished silently". Doing the same here
        // needs no new seam in `actions/` and no second mechanism to keep in
        // step with the first.
        //
        // Finishing `SActionHistory::onRejected_`'s "TODO: Log error and
        // notify UI" is the general answer, and it is QBX-176 -- an app-wide
        // change in another module, which is why this lane-level report does
        // not wait for it.
        SActionHistory *hist = app_->actionHistory();
        const int rejectedBefore = hist ? hist->rejectedCount() : 0;
        for( const Seg &s : segs )
            app_->submitAction( new SPlaceRecordingAction(
                a.path, a.file, s.at, s.srcOffset, s.len, mode, s.extent ) );
        const int rejectedAfter = hist ? hist->rejectedCount() : rejectedBefore;
        if( rejectedAfter > rejectedBefore ) {
            // The mode is named because it is the actionable half: the same
            // lane under `takes` would have taken the pass, and the user's way
            // out is `set-track-record-mode`.
            const QString why =
                QStringLiteral( "the lane refused the pass in record mode "
                                "'%1'" ).arg( mode.isEmpty()
                                                  ? QStringLiteral( "takes" )
                                                  : mode );
            TW_LOGW( "shell", "[REC] lane refused the pass: %s (mode '%s')",
                     a.file.toStdString().c_str(),
                     mode.toStdString().c_str() );
            skipped_.append( { a.path, a.file, why } );
        }
    }
    app_->endRecordCommit();

    // ONE REPORT PER PASS, never one per lane: a band punch across eight lanes
    // that all refuse must not produce eight of anything. Logged always, and
    // handed to the UI through the app, which is where the one-dialog-per-
    // session precedent for this shape lives (notifyAudioOutputUnavailable).
    if( !skipped_.isEmpty() ) {
        TW_LOGW( "shell", "[REC] %d of %d armed lane(s) did not take the pass; "
                          "%d capture file(s) unused",
                 (int) skipped_.size(), (int) armed_.size(),
                 (int) unusedRecordings().size() );
        app_->notifyRecordingLanesSkipped( (int) skipped_.size(),
                                           skipped_.first().why );
    }

    // Arm state is deliberately untouched (item e): the armed track(s) stay
    // armed after a take is placed, ready for the next one. The
    // "recording in progress" visual (the head's translucent overlay,
    // SMVActualView::paintEvent) is already gone by the time we get here —
    // it is gated on SApplication::isRecordingActive(), which stop() cleared
    // at its very top, not on the arm flag this used to also clear.

    TW_LOGI( "shell", "[REC] placed %u segment(s) x %u track(s) from frame %lld "
                      "(captured %lld, trimmed %lld)",
             (unsigned) segs.size(), (unsigned) armed_.size(),
             (long long) firstPlace, (long long) total, (long long) trimmed_ );
    armed_.clear();
}

QStringList SAudioRecorder::unusedRecordings() const
{
    // DISTINCT, in pass order. Two lanes of one pass can share a capture file
    // (a mono source armed on both), and reporting it twice would overstate
    // what the user has to deal with.
    QStringList out;
    for( const SkippedLane &sl : skipped_ )
        if( !sl.file.isEmpty() && !out.contains( sl.file ) )
            out.append( sl.file );
    return out;
}

QString SAudioRecorder::describe() const
{
    // `skipped`/`unused` are here because `describe()` is the only surface a
    // .qxa can read the recorder through (`assert-recorded-clip contains=`),
    // and a report nothing can assert is a report that will quietly stop
    // working. The UI half -- the dialog -- is interactive-only by the same
    // rule that governs notifyAudioOutputUnavailable, so it is NOT gated, and
    // the PR says so.
    return QStringLiteral( "rec=%1 start=%2 p0=%3 anchored=%4 inLat=%5 outLat=%6 "
                           "userOff=%7 comp=%8 trim=%9 passes=%10 dev=%11 "
                           "snapA=%12 skipped=%13 unused=%14" )
        .arg( active_ ? "on" : "off" )
        .arg( (qlonglong) recordStart_ )
        .arg( (qlonglong) placement_.p0 )
        .arg( placement_.anchored ? 1 : 0 )
        .arg( (qlonglong) placement_.inputLatencyProj )
        .arg( (qlonglong) placement_.outputLatencyProj )
        .arg( (qlonglong) placement_.userOffsetProj )
        .arg( (qlonglong) placement_.compensationFrames() )
        .arg( (qlonglong) trimmed_ )
        .arg( lastPassCount_ )
        .arg( deviceName_.isEmpty() ? QStringLiteral( "-" ) : deviceName_ )
        .arg( (qlonglong) snapDeltaA_ )
        .arg( (int) skipped_.size() )
        .arg( (int) unusedRecordings().size() );
}
