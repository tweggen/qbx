#include "tw/record/capture_bridge.h"

#include "tw/core/twlog.h"
#include "tw/devices/audio_input.h"
#include "tw/devices/audio_ring.h"
#include "tw/sinks/audio_file_writer.h"
#include "tw/sources/twgrowingcapturesource.h"

#include "linear_resampler.h"

#include <algorithm>
#include <chrono>

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

std::uint64_t CaptureBridge::frontier() const
{
    return source_ ? source_->frontier() : 0;
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
        if( input_->openDevice( params.inputDeviceId, params.targetRate ) < 0 ) {
            lastError_ = std::string( "Failed to open input device: " ) + input_->errorMessage();
            ownedInput_.reset();
            input_ = nullptr;
            return false;
        }
    }

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
    const std::size_t chunk = params.chunkFrames
                                  ? params.chunkFrames
                                  : twGrowingCaptureSource::kDefaultChunkFrames;
    source_ = std::make_shared<twGrowingCaptureSource>(
        (idx_t) inputChannels_, (int) targetRate_, chunk );

    // --- the live-lane ring ---------------------------------------------
    liveRing_ = std::unique_ptr<AudioRing>( new AudioRing() );
    liveRing_->reset( inputChannels_,
                      params.liveRingFrames ? params.liveRingFrames : 16384 );
    liveScratch_.assign( (std::size_t) 8192 * inputChannels_, 0.0f );

    // --- the files -------------------------------------------------------
    for( const CaptureWavSink &spec : params.wavSinks ) {
        auto st = std::unique_ptr<WavSinkState>( new WavSinkState() );
        st->path        = spec.path;
        st->channelMask = spec.channelMask;
        st->outChannels = source_->maskedChannels( spec.channelMask );
        st->writer      = params.writerFactory ? params.writerFactory()
                                               : createAudioFileWriter( AudioFormat::WAV );
        if( !st->writer ) {
            lastError_ = "Failed to create WAV writer";
            closeWriters();
            sinks_.clear();
            source_.reset();
            liveRing_.reset();
            if( ownedInput_ ) { ownedInput_->closeDevice(); ownedInput_.reset(); }
            input_ = nullptr;
            return false;
        }
        AudioFileConfig fc;
        fc.sampleRate = targetRate_;
        fc.channels   = st->outChannels;
        fc.sampleType = cfg.sampleType;
        if( !st->writer->open( st->path, fc ) ) {
            lastError_ = std::string( "Failed to open WAV file: " ) + st->writer->errorMessage();
            st->writer.reset();
            closeWriters();
            sinks_.clear();
            source_.reset();
            liveRing_.reset();
            if( ownedInput_ ) { ownedInput_->closeDevice(); ownedInput_.reset(); }
            input_ = nullptr;
            return false;
        }
        st->scratch.assign( kWavChunkFrames * st->outChannels, 0.0f );
        sinks_.push_back( std::move( st ) );
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

    closeWriters();

    const CaptureBridgeStats s = stats();      // latches the input's counters

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

    for( ;; ) {
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
        const std::size_t pushed = liveRing_->push( data, frames );
        framesToLive_.fetch_add( pushed, std::memory_order_relaxed );
        if( pushed < frames )
            liveOverruns_.fetch_add( frames - pushed, std::memory_order_relaxed );

        // SINK 2 — the pages. THE record of what was captured.
        const std::size_t appended = source_->append( data, frames );
        framesToPages_.fetch_add( appended, std::memory_order_relaxed );

        if( onFrames ) onFrames( source_->frontier(), appended );

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
    if( sinks_.empty() ) return;

    const auto wait = idleWait( blockFrames_, inputRate_ );
    for( ;; ) {
        drainToWriters();

        if( wavStop_.load( std::memory_order_acquire ) ) {
            // The bridge thread has exited, so the frontier is final: complete
            // every file OUT OF THE PAGES, however far behind it had fallen.
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
    for( ;; ) {
        const std::uint64_t fr = source_->frontier();
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
            source_->readInterleaved( wavCursor_, st->scratch.data(), n,
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
    s.pageDrops     = source_ ? source_->droppedFrames() : 0;
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
