#include "tw/record/capture_bridge.h"

#include "tw/core/twlog.h"
#include "tw/devices/audio_input.h"
#include "tw/devices/audio_ring.h"
#include "tw/sinks/audio_file_writer.h"
#include "tw/devices/midi_out_scheduler.h"
#include "tw/sources/twgrowingcapturesource.h"

#include "linear_resampler.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#define BRIDGE_LOG( fmt, ... ) \
    TW_LOGD( "record", "CaptureBridge::%s: " fmt, __func__, ##__VA_ARGS__ )
#define BRIDGE_WARN( fmt, ... ) \
    TW_LOGW( "record", "CaptureBridge::%s: " fmt, __func__, ##__VA_ARGS__ )

namespace audio {

namespace {

// How much the WAV thread moves per write call. 4096 frames is the plugin
// chunk everywhere else in the engine and is small enough that a stalled
// writer's backlog is measured, not merely observed once at the end.
constexpr std::size_t kWavChunkFrames = 4096;

// How long the bridge thread sleeps when the ring is empty. Half the device
// block period, floored at 1 ms and capped at 10 ms: a block-paced wake, not
// the 1 ms spin design D7 retires, and far inside the ring's 16-block depth.
std::chrono::microseconds idleWait( std::size_t blockFrames, std::uint32_t rate )
{
    if( !rate ) rate = 48000;
    double us = 0.5 * 1e6 * (double) blockFrames / (double) rate;
    if( us < 1000.0 )  us = 1000.0;
    if( us > 10000.0 ) us = 10000.0;
    return std::chrono::microseconds( (long long) us );
}

}  // namespace

struct CaptureBridge::WavSinkState {
    std::unique_ptr<AudioFileWriter> writer;
    std::string   path;
    std::uint32_t channelMask = 0;
    std::uint32_t outChannels = 0;
    std::vector<float> scratch;      // interleaved, kWavChunkFrames * outChannels
    bool ok = true;
};

CaptureBridge::CaptureBridge() {}

CaptureBridge::~CaptureBridge()
{
    stop();
}

std::shared_ptr<twGrowingCaptureSource> CaptureBridge::source() const
{
    std::lock_guard<std::mutex> lk( m_ );
    return source_;
}

std::uint64_t CaptureBridge::frontier() const
{
    const std::shared_ptr<twGrowingCaptureSource> src = source();
    return src ? src->frontier() : 0;
}

int CaptureBridge::requestInputChannels( std::uint64_t mask )
{
    if( !input_ ) return -1;
    return input_->requestChannels( mask );
}

bool CaptureBridge::start( const CaptureBridgeParams &params )
{
    if( running_.load( std::memory_order_acquire ) ) {
        lastError_ = "Capture bridge already running";
        return false;
    }
    lastError_.clear();

    // --- the input -------------------------------------------------------
    if( params.externalInput ) {
        input_ = params.externalInput;
    } else {
        ownedInput_ = params.inputBackend.empty() ? createAudioInput()
                                                  : createAudioInput( params.inputBackend );
        if( !ownedInput_ ) {
            lastError_ = "Failed to create audio input";
            return false;
        }
        input_ = ownedInput_.get();
        // BEFORE the open: an ASIO device chooses its channel set at
        // createBuffers time, and the cheapest moment to tell it is while no
        // driver is running at all. A no-op on every other backend.
        if( params.inputChannelMask )
            input_->requestChannels( params.inputChannelMask );
        if( input_->openDevice( params.inputDeviceId, params.targetRate ) < 0 ) {
            lastError_ = std::string( "Failed to open input device: " ) + input_->errorMessage();
            ownedInput_.reset();
            input_ = nullptr;
            return false;
        }
    }

    writerFactory_ = params.writerFactory;

    const AudioInputConfig &cfg = input_->getConfig();
    inputRate_          = cfg.sampleRate;
    inputChannels_      = cfg.channels ? cfg.channels : 1;
    inputLatencyFrames_ = input_->getLatencyFrames();
    targetRate_         = params.targetRate ? params.targetRate : inputRate_;
    blockFrames_        = cfg.bufferFrames ? cfg.bufferFrames : 1024;

    BRIDGE_LOG( "input=%s device=%u Hz x%u ch, project=%u Hz, block=%u, latency=%u frames",
                input_->backendName(), (unsigned) inputRate_, (unsigned) inputChannels_,
                (unsigned) targetRate_, (unsigned) blockFrames_,
                (unsigned) inputLatencyFrames_ );

    // --- the pages -------------------------------------------------------
    // Only when this bridge is capturing from the start. A MONITOR-only bridge
    // holds no source until beginCapture() (proposal 21 L3b).
    chunkFrames_ = params.chunkFrames
                       ? params.chunkFrames
                       : twGrowingCaptureSource::kDefaultChunkFrames;
    liveEnabled_.store( params.liveEnabled, std::memory_order_release );
    capturing_.store( params.capturePages, std::memory_order_release );
    captureStartHostNs_.store( 0, std::memory_order_release );
    segFrames_ = 0;
    source_.reset();
    if( params.capturePages )
        source_ = std::make_shared<twGrowingCaptureSource>(
            (idx_t) inputChannels_, (int) targetRate_, chunkFrames_ );

    // --- the live-lane ring ---------------------------------------------
    liveRing_ = std::unique_ptr<AudioRing>( new AudioRing() );
    liveRing_->reset( inputChannels_,
                      params.liveRingFrames ? params.liveRingFrames : 16384 );
    liveScratch_.assign( (std::size_t) 8192 * inputChannels_, 0.0f );

    // --- the files -------------------------------------------------------
    if( !params.wavSinks.empty() ) {
        if( !source_ ) {
            lastError_ = "WAV sinks without capturePages";
            liveRing_.reset();
            if( ownedInput_ ) { ownedInput_->closeDevice(); ownedInput_.reset(); }
            input_ = nullptr;
            return false;
        }
        std::lock_guard<std::mutex> lk( sinkM_ );
        if( !openSinks_( params.wavSinks, source_ ) ) {
            closeWriters();
            sinks_.clear();
            source_.reset();
            liveRing_.reset();
            if( ownedInput_ ) { ownedInput_->closeDevice(); ownedInput_.reset(); }
            input_ = nullptr;
            return false;
        }
    }

    // --- go --------------------------------------------------------------
    wavCursor_ = 0;
    framesIn_ = framesToPages_ = framesToLive_ = framesToWav_ = 0;
    wavLate_ = wavFinalized_ = liveOverruns_ = 0;
    ringOverruns_ = ringUnderruns_ = 0;
    bridgeWakeups_ = wavWakeups_ = 0;
    bridgeStop_   = false;
    wavStop_      = false;
    captureEnded_ = false;

    if( input_->startCapture() < 0 ) {
        lastError_ = std::string( "Failed to start capture: " ) + input_->errorMessage();
        closeWriters();
        sinks_.clear();
        source_.reset();
        liveRing_.reset();
        if( ownedInput_ ) { ownedInput_->closeDevice(); ownedInput_.reset(); }
        input_ = nullptr;
        return false;
    }

    startWall_ = std::chrono::steady_clock::now();
    running_.store( true, std::memory_order_release );
    bridgeThread_ = std::thread( [this] { bridgeThreadMain(); } );
    wavThread_    = std::thread( [this] { wavThreadMain(); } );
    return true;
}

void CaptureBridge::stop()
{
    if( !running_.exchange( false, std::memory_order_acq_rel ) ) {
        // Not running. Still join anything left over from a failed start.
        if( bridgeThread_.joinable() ) bridgeThread_.join();
        if( wavThread_.joinable() )    wavThread_.join();
        return;
    }

    // 1. The device stops producing FIRST, so the ring has a final size.
    if( input_ ) input_->stopCapture();

    // 2. The bridge thread drains what is left and exits.
    bridgeStop_.store( true, std::memory_order_release );
    bridgeCv_.notify_all();
    if( bridgeThread_.joinable() ) bridgeThread_.join();

    // 3. Only now may the WAV thread believe the frontier is final: it drains
    //    the remaining backlog OUT OF THE PAGES and exits.
    wavStop_.store( true, std::memory_order_release );
    wavCv_.notify_all();
    if( wavThread_.joinable() ) wavThread_.join();

    {
        std::lock_guard<std::mutex> sl( sinkM_ );
        closeWriters();
        sinks_.clear();
    }
    capturing_.store( false, std::memory_order_release );

    const CaptureBridgeStats s = stats();      // latches the input's counters

    // THE CAPTURE-RATE CHECK (ported from RecordingSession, main PR #55). The
    // frames the device actually handed us per real second, against the rate we
    // opened it at. A shared-mode endpoint whose OS rate differs from its
    // sibling's (one interface clock, two endpoint settings) is auto-converted
    // by Windows and MISREPORTS its clock; nothing downstream can compute its
    // way out of that, so past 1 % over a run of at least 2 s this WARNS and
    // converts the ratio into semitones and names the likely cause.
    {
        const double wall = std::chrono::duration_cast<std::chrono::duration<double>>(
                                std::chrono::steady_clock::now() - startWall_ ).count();
        const double effRate = wall > 0.0 ? (double) s.framesIn / wall : 0.0;
        const double ratio   = inputRate_ > 0 ? effRate / (double) inputRate_ : 0.0;
        BRIDGE_LOG( "capture-rate check — assumed input=%u Hz, MEASURED effective=%.1f Hz "
                    "over %.3fs (%llu input frames), ratio meas/assumed=%.4f",
                    (unsigned) inputRate_, effRate, wall,
                    (unsigned long long) s.framesIn, ratio );
        if( wall >= 2.0 && ratio > 0.0 && std::fabs( ratio - 1.0 ) > 0.01 ) {
            TW_LOGW( "record",
                     "CaptureBridge: CAPTURE RATE IS WRONG by %.1f %% — this take is "
                     "pitched %s by about %.2f semitones. The usual cause is the input "
                     "and output ENDPOINTS being set to different rates in the OS while "
                     "sharing one interface clock; check that both are set to %u Hz.",
                     ( ratio - 1.0 ) * 100.0, ratio > 1.0 ? "DOWN" : "UP",
                     std::fabs( 12.0 * std::log2( ratio ) ), (unsigned) targetRate_ );
        }
    }

    if( ownedInput_ ) {
        ownedInput_->closeDevice();
        ownedInput_.reset();
    }
    input_ = nullptr;

    BRIDGE_LOG( "stopped — in=%llu pages=%llu live=%llu wav=%llu "
                "(finalized=%llu, lateHW=%llu) overruns=%llu drops=%llu",
                (unsigned long long) s.framesIn, (unsigned long long) s.framesToPages,
                (unsigned long long) s.framesToLive, (unsigned long long) s.framesToWav,
                (unsigned long long) s.wavFinalized, (unsigned long long) s.wavLate,
                (unsigned long long) s.ringOverruns, (unsigned long long) s.pageDrops );
}

bool CaptureBridge::openSinks_( const std::vector<CaptureWavSink> &sinks,
                                const std::shared_ptr<twGrowingCaptureSource> &src )
{
    // Caller holds sinkM_.
    AudioFileConfig base;
    base.sampleRate = targetRate_;
    base.sampleType = input_ ? input_->getConfig().sampleType : twSampleType::Float32;

    for( const CaptureWavSink &spec : sinks ) {
        auto st = std::unique_ptr<WavSinkState>( new WavSinkState() );
        st->path        = spec.path;
        st->channelMask = spec.channelMask;
        st->outChannels = src->maskedChannels( spec.channelMask );
        st->writer      = writerFactory_ ? writerFactory_()
                                         : createAudioFileWriter( AudioFormat::WAV );
        if( !st->writer ) {
            lastError_ = "Failed to create WAV writer";
            return false;
        }
        AudioFileConfig fc = base;
        fc.channels = st->outChannels;
        if( !st->writer->open( st->path, fc ) ) {
            lastError_ = std::string( "Failed to open WAV file: " )
                       + st->writer->errorMessage();
            st->writer.reset();
            return false;
        }
        st->scratch.assign( kWavChunkFrames * st->outChannels, 0.0f );
        sinks_.push_back( std::move( st ) );
    }
    return true;
}

bool CaptureBridge::beginCapture( const std::vector<CaptureWavSink> &sinks )
{
    if( !running_.load( std::memory_order_acquire ) ) {
        lastError_ = "beginCapture on a bridge that is not running";
        return false;
    }
    if( capturing_.load( std::memory_order_acquire ) ) {
        lastError_ = "beginCapture while a segment is already open";
        return false;
    }
    // sinkM_ first, then m_ (see the header: the order is never the reverse).
    std::lock_guard<std::mutex> sl( sinkM_ );
    closeWriters();
    sinks_.clear();

    std::shared_ptr<twGrowingCaptureSource> src =
        std::make_shared<twGrowingCaptureSource>(
            (idx_t) inputChannels_, (int) targetRate_,
            chunkFrames_ ? chunkFrames_
                         : twGrowingCaptureSource::kDefaultChunkFrames );

    if( !openSinks_( sinks, src ) ) {
        closeWriters();
        sinks_.clear();
        return false;
    }
    wavCursor_ = 0;
    {
        std::lock_guard<std::mutex> lk( m_ );
        source_    = src;
        segFrames_ = 0;
        captureStartHostNs_.store( 0, std::memory_order_release );
        capturing_.store( true, std::memory_order_release );
    }
    BRIDGE_LOG( "segment opened, %u file(s)", (unsigned) sinks.size() );
    return true;
}

void CaptureBridge::endCapture()
{
    if( !capturing_.load( std::memory_order_acquire ) ) return;

    std::lock_guard<std::mutex> sl( sinkM_ );
    {
        // Under m_, so the bridge thread cannot be mid-append: after this the
        // frontier is final for this segment (invariant 3's precondition, made
        // local to a segment).
        std::lock_guard<std::mutex> lk( m_ );
        capturing_.store( false, std::memory_order_release );
    }
    // The ordinary drain, made from this thread — there is no second write
    // path that could disagree with the first (invariant 3).
    const std::uint64_t before = framesToWav_.load( std::memory_order_relaxed );
    drainToWriters();
    wavFinalized_.fetch_add(
        framesToWav_.load( std::memory_order_relaxed ) - before,
        std::memory_order_relaxed );
    closeWriters();
    sinks_.clear();
    BRIDGE_LOG( "segment closed at frontier %llu",
                (unsigned long long) frontier() );
}

void CaptureBridge::closeWriters()
{
    for( auto &st : sinks_ ) {
        if( st && st->writer ) {
            if( !st->writer->close() && st->ok ) {
                st->ok = false;
                if( lastError_.empty() )
                    lastError_ = "Failed to close WAV file " + st->path;
            }
            st->writer.reset();
        }
    }
}

void CaptureBridge::bridgeThreadMain()
{
    // Everything the steady state touches is sized ONCE, here. The only
    // allocation after this point is a growing-source chunk at a chunk
    // boundary (see the class comment).
    const std::size_t popFrames = std::max<std::size_t>( blockFrames_ * 4, 4096 );
    std::vector<float> pop( popFrames * inputChannels_, 0.0f );
    std::vector<float> resampled;
    resampled.reserve( (std::size_t)( popFrames * 2 + 8 ) * inputChannels_ );

    LinearResampler resampler( inputRate_, targetRate_, inputChannels_ );
    if( !resampler.passthrough() )
        BRIDGE_LOG( "resampler %u Hz -> %u Hz ACTIVE", (unsigned) inputRate_,
                    (unsigned) targetRate_ );

    const auto wait = idleWait( blockFrames_, inputRate_ );

    // After stop is requested the loop DRAINS what the device still holds and
    // then exits — bounded, so an input that never runs dry (a misbehaving
    // backend handing out frames unboundedly) can never pin the join in
    // stop(). Measured: the null input used to be exactly that, and a MIDI-only
    // live arm hung every case at its disarm.
    unsigned drainAfterStop = 0;
    for( ;; ) {
        if( bridgeStop_.load( std::memory_order_acquire ) && ++drainAfterStop > 64 ) break;
        const std::int32_t got = input_->read( pop.data(), popFrames );
        if( got < 0 ) {
            BRIDGE_WARN( "input read error: %s", input_->errorMessage() );
            break;
        }

        if( got == 0 ) {
            // Ring empty. Once the device has been stopped there is nothing
            // more coming, so this is the exit.
            if( bridgeStop_.load( std::memory_order_acquire ) ) break;
            std::unique_lock<std::mutex> lk( m_ );
            bridgeCv_.wait_for( lk, wait );
            continue;
        }

        bridgeWakeups_.fetch_add( 1, std::memory_order_relaxed );
        framesIn_.fetch_add( (std::uint64_t) got, std::memory_order_relaxed );

        const float *data   = pop.data();
        std::size_t  frames = (std::size_t) got;
        if( !resampler.passthrough() ) {
            frames = resampler.process( pop.data(), frames, resampled );
            data   = resampled.data();
        }
        if( frames == 0 ) continue;

        // SINK 1 — the live lane. First, because it is the only sink with a
        // latency budget; a dropped frame here is a monitoring glitch and a
        // counter, never a loss of the recording (which is sink 2's job).
        // Skipped entirely when nothing is popping the ring: a recording with
        // monitoring off would otherwise fill it once and then count every
        // frame of the take as an overrun (proposal 21 L3b).
        if( liveEnabled_.load( std::memory_order_acquire ) ) {
            const std::size_t pushed = liveRing_->push( data, frames );
            framesToLive_.fetch_add( pushed, std::memory_order_relaxed );
            if( pushed < frames )
                liveOverruns_.fetch_add( frames - pushed,
                                         std::memory_order_relaxed );
        }

        // SINK 2 — the pages. THE record of what was captured. Under m_, and
        // only while a SEGMENT is open: that is what makes endCapture()'s
        // `capturing_ = false` a fence rather than a hint, and what keeps a
        // monitor-only bridge from growing pages nobody asked to keep.
        std::uint64_t frontierNow = 0;
        std::size_t   appended    = 0;
        {
            std::lock_guard<std::mutex> lk( m_ );
            if( capturing_.load( std::memory_order_relaxed ) && source_ ) {
                if( segFrames_ == 0 ) {
                    // THE INPUT ANCHOR (design D6). The batch was POPPED now,
                    // but a device hands over audio it has already recorded,
                    // so its first frame was captured one batch duration ago.
                    const std::int64_t rate =
                        targetRate_ ? (std::int64_t) targetRate_ : 48000;
                    captureStartHostNs_.store(
                        MidiOutScheduler::hostNowNs()
                            - (std::int64_t) frames * 1000000000LL / rate,
                        std::memory_order_release );
                }
                appended = source_->append( data, frames );
                segFrames_ += appended;
                frontierNow = source_->frontier();
            }
        }
        framesToPages_.fetch_add( appended, std::memory_order_relaxed );

        if( appended && onFrames ) onFrames( frontierNow, appended );

        // SINK 3 is not here on purpose: the WAV thread reads the pages by
        // position, so a slow file cannot reach back and stall this loop.
        wavCv_.notify_all();
    }

    // The frontier is final from here. Anything the WAV thread still owes is
    // now, by definition, written out of the pages.
    captureEnded_.store( true, std::memory_order_release );
    wavCv_.notify_all();
}

void CaptureBridge::wavThreadMain()
{
    // NOTE it runs even with no sinks: beginCapture() may install some later
    // (proposal 21 L3b), and drainToWriters() over an empty sink list is a
    // frontier read and a return.
    const auto wait = idleWait( blockFrames_, inputRate_ );
    for( ;; ) {
        {
            std::lock_guard<std::mutex> sl( sinkM_ );
            drainToWriters();
        }

        if( wavStop_.load( std::memory_order_acquire ) ) {
            // The bridge thread has exited, so the frontier is final: complete
            // every file OUT OF THE PAGES, however far behind it had fallen.
            std::lock_guard<std::mutex> sl( sinkM_ );
            finalizeFromPages();
            break;
        }
        std::unique_lock<std::mutex> lk( m_ );
        wavCv_.wait_for( lk, wait );
        wavWakeups_.fetch_add( 1, std::memory_order_relaxed );
    }
}

void CaptureBridge::drainToWriters()
{
    // The segment in hand. Taken ONCE per drain: beginCapture() cannot swap it
    // underneath because it holds sinkM_ for the whole call, and this runs
    // either on the WAV thread (which holds sinkM_) or from endCapture()
    // (which holds it too).
    std::shared_ptr<twGrowingCaptureSource> src;
    {
        std::lock_guard<std::mutex> lk( m_ );
        src = source_;
    }
    if( !src || sinks_.empty() ) return;

    for( ;; ) {
        const std::uint64_t fr = src->frontier();
        if( fr <= wavCursor_ ) return;   // caught up

        const std::uint64_t backlog = fr - wavCursor_;
        std::uint64_t hw = wavLate_.load( std::memory_order_relaxed );
        while( backlog > hw &&
               !wavLate_.compare_exchange_weak( hw, backlog,
                                                std::memory_order_relaxed ) ) {}

        const std::size_t n =
            (std::size_t) std::min<std::uint64_t>( backlog, kWavChunkFrames );

        for( auto &st : sinks_ ) {
            if( !st->ok || !st->writer ) continue;
            src->readInterleaved( wavCursor_, st->scratch.data(), n,
                                      st->channelMask );
            if( !st->writer->write( st->scratch.data(), n ) ) {
                st->ok = false;
                BRIDGE_WARN( "write error on %s: %s", st->path.c_str(),
                             st->writer->errorMessage() );
            }
        }

        wavCursor_ += n;
        framesToWav_.fetch_add( n, std::memory_order_relaxed );
        if( captureEnded_.load( std::memory_order_acquire ) )
            wavFinalized_.fetch_add( n, std::memory_order_relaxed );
    }
}

std::size_t CaptureBridge::pullLive( float *const *out, std::size_t channels,
                                     std::size_t frames, offset_t /*pos*/ )
{
    if( !liveRing_ || !out || frames == 0 ) return 0;

    const std::size_t ringCh = liveRing_->channels();
    const std::size_t cap    = liveScratch_.size() / ( ringCh ? ringCh : 1 );
    if( frames > cap ) frames = cap;

    const std::size_t got = liveRing_->pop( liveScratch_.data(), frames );
    if( got == 0 ) return 0;

    for( std::size_t c = 0; c < channels; ++c ) {
        if( !out[ c ] ) continue;
        // Same clamp the wide-page rule uses: a caller asking for more
        // channels than the device has gets the last one it does have.
        const std::size_t src = ( c < ringCh ) ? c : ( ringCh ? ringCh - 1 : 0 );
        const float *p = liveScratch_.data() + src;
        for( std::size_t i = 0; i < got; ++i ) {
            out[ c ][ i ] = *p;
            p += ringCh;
        }
    }
    return got;
}

void CaptureBridge::clearLive()
{
    if( liveRing_ ) liveRing_->clear();
}

CaptureBridgeStats CaptureBridge::stats() const
{
    CaptureBridgeStats s;
    s.framesIn      = framesIn_.load( std::memory_order_relaxed );
    s.framesToPages = framesToPages_.load( std::memory_order_relaxed );
    s.framesToLive  = framesToLive_.load( std::memory_order_relaxed );
    s.framesToWav   = framesToWav_.load( std::memory_order_relaxed );
    s.wavLate       = wavLate_.load( std::memory_order_relaxed );
    s.wavFinalized  = wavFinalized_.load( std::memory_order_relaxed );
    s.liveOverruns  = liveOverruns_.load( std::memory_order_relaxed );
    s.bridgeWakeups = bridgeWakeups_.load( std::memory_order_relaxed );
    s.wavWakeups    = wavWakeups_.load( std::memory_order_relaxed );
    {
        std::lock_guard<std::mutex> lk( m_ );
        s.pageDrops = source_ ? source_->droppedFrames() : 0;
    }
    // The input's own counters are LATCHED, because stop() closes the device
    // and drops the pointer — and "ringOverruns == 0" is exactly the claim a
    // test makes AFTER the run, when there is no device left to ask.
    if( input_ ) {
        const AudioInputStats is = input_->stats();
        ringOverruns_.store( is.overrunFrames, std::memory_order_relaxed );
        ringUnderruns_.store( is.underrunFrames, std::memory_order_relaxed );
    }
    s.ringOverruns  = ringOverruns_.load( std::memory_order_relaxed );
    s.ringUnderruns = ringUnderruns_.load( std::memory_order_relaxed );
    return s;
}

}  // namespace audio
