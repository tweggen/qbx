#ifndef _TW_CAPTURE_BRIDGE_H_
#define _TW_CAPTURE_BRIDGE_H_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "tw/core/twtypes.h"

class twGrowingCaptureSource;

namespace audio {

class AudioInput;
class AudioFileWriter;
class AudioRing;

// One WAV file fed by the bridge. `channelMask` is a bitmask over the INPUT's
// channels (bit n = channel n; 0 = every channel), which is how one capture
// feeds several armed tracks of different widths without a second copy of the
// audio.
struct CaptureWavSink {
    std::string   path;
    std::uint32_t channelMask = 0;
};

struct CaptureBridgeParams {
    // Empty backend == read SMARAGD_AUDIO_INPUT_BACKEND (the L0 selection).
    std::string   inputBackend;
    std::string   inputDeviceId = "default";

    // Project rate. The pages and every file are written at THIS rate; the
    // resampler between the ring and the sinks is a passthrough (and is not
    // called at all) when the device already delivers it. 0 == device rate.
    std::uint32_t targetRate = 0;

    // Depth of the live-lane ring, in frames. The default is 16 device blocks
    // at 1024 — the same depth an input device's own ring uses.
    std::size_t   liveRingFrames = 16384;

    // Growing-source chunk, in frames. 0 == twGrowingCaptureSource's default
    // (one page per channel).
    std::size_t   chunkFrames = 0;

    // INPUT CHANNELS TO MAKE AVAILABLE, as a bitmask where bit n means input n
    // — the same convention as `CaptureWavSink::channelMask` and
    // `SObject::recordingChannels_`. 0 means "no opinion".
    //
    // Only ASIO acts on it (proposal 35 Phase 3): every other backend opens the
    // endpoint's channels whether anyone asked or not, whereas an ASIO driver
    // fixes its channel set at createBuffers time and must be told BEFORE it
    // opens. Passing the union of the armed tracks' masks is what lets a user
    // record input 6 of a 16-input interface without the device opening all
    // sixteen.
    std::uint64_t inputChannelMask = 0;

    std::vector<CaptureWavSink> wavSinks;

    // Does this bridge CAPTURE (append to the pages and to the files) from the
    // moment it starts? A bridge started purely to MONITOR says false: the
    // pages are the record of a RECORDING, and a monitoring session that grew
    // them would leak 370 KB/s of the user's RAM for audio nobody asked to
    // keep. It then holds no source at all until beginCapture() (proposal 21
    // L3b). Default true, which is the L3a shape and what record_bridge_test
    // exercises.
    bool capturePages = true;

    // Does the bridge push into the LIVE-LANE ring? A recording with
    // monitoring OFF has no pump popping it, so pushing would fill the ring
    // once and then count a phantom `liveOverruns` for every frame of the take
    // (proposal 21 L3b). Runtime-settable through setLiveEnabled().
    bool liveEnabled = true;

    // --- test seams ------------------------------------------------------
    // An already-open, already-startable input the bridge does NOT own. When
    // set, inputBackend/inputDeviceId are ignored and the caller keeps the
    // device's lifetime (start/stop capture are still the bridge's).
    AudioInput *externalInput = nullptr;
    // Where the WAV writers come from. Default: createAudioFileWriter(WAV).
    std::function<std::unique_ptr<AudioFileWriter>()> writerFactory;
};

// Every number the bridge can be held to. All relaxed atomics: a meter, a log
// line and a test read them, nothing synchronises on them.
struct CaptureBridgeStats {
    std::uint64_t framesIn       = 0;  // frames popped off the input ring
    std::uint64_t framesToPages  = 0;  // frames appended to the growing source
    std::uint64_t framesToLive   = 0;  // frames pushed into the live-lane ring
    std::uint64_t framesToWav    = 0;  // frames handed to the writers
    std::uint64_t wavLate        = 0;  // HIGH-WATER backlog (frontier - framesToWav)
    std::uint64_t wavFinalized   = 0;  // frames written by finalizeFromPages()
    std::uint64_t ringOverruns   = 0;  // AudioInput::stats().overrunFrames
    std::uint64_t ringUnderruns  = 0;  // AudioInput::stats().underrunFrames
    std::uint64_t liveOverruns   = 0;  // frames the live ring could not hold
    std::uint64_t pageDrops      = 0;  // frames the growing source refused
    std::uint64_t bridgeWakeups  = 0;
    std::uint64_t wavWakeups     = 0;
};

/**
 * ONE input pump, THREE sinks (proposal 21 L3a, design D7).
 *
 * A capture device's SPSC ring (L0) has exactly one consumer, and this is it.
 * The bridge drains that ring and fans the frames out to
 *
 *   1. the LIVE-LANE ring   — a second SPSC ring the LiveGraphPump pops with
 *                             pullLive(); the bridge is its single producer;
 *   2. the PAGES            — a twGrowingCaptureSource, chunked planar, atomic
 *                             frontier, readable by position from any thread;
 *   3. the WAV writers      — one per armed track, each with a channel mask.
 *
 * **The WAV sink can never stall the ring.** It does not run on the bridge
 * thread at all: a second thread reads the growing capture source BY POSITION
 * and writes what it finds. A writer that falls behind therefore costs a
 * BACKLOG (`wavLate`, a high-water mark in frames) and nothing else — the ring
 * keeps draining, the pages keep growing, `ringOverruns` stays 0 — and at stop
 * the file is completed out of the pages (`wavFinalized`). That is the whole
 * point of routing the file through the pages instead of through the ring:
 * the pages are the record of what was captured, and the file is a view of
 * them that is allowed to be late.
 *
 * Threading
 *   - the device's capture thread is the ring's only PRODUCER (L0);
 *   - the BRIDGE THREAD (owned here) is its only consumer. It is its own
 *     thread and not the pump: the pump exists only while a live lane is
 *     armed, must be allocation-free, and must not block — while the bridge
 *     allocates a chunk at a chunk boundary and outlives any plan. It wakes on
 *     a condition variable with a timeout of half the device block period
 *     (never a 1 ms spin) and pops everything the ring holds; the ring is 16
 *     blocks deep, so a wake that late cannot overrun it;
 *   - the WAV THREAD (owned here) is a by-position reader of the pages;
 *   - pullLive() is called by ONE consumer (the pump) and pops the live ring.
 *   - Nothing here touches Qt (THREADING.md rule 1).
 *
 * Allocation. Steady state on the bridge thread is allocation-free — the pop
 * scratch, the deinterleave scratch and the resampler's output vector are
 * sized once in start() — EXCEPT at a growing-source chunk boundary, where the
 * producer allocates one chunk (`channels * chunkFrames * 4` bytes; 512 KB
 * every 1.37 s for stereo at 48 kHz). twGrowingCaptureSource::reserveThrough()
 * exists for a caller that wants even that paid up front.
 */
class CaptureBridge {
public:
    CaptureBridge();
    ~CaptureBridge();

    CaptureBridge( const CaptureBridge & ) = delete;
    CaptureBridge &operator=( const CaptureBridge & ) = delete;

    /// Open the input, create the growing source, open the writers and start
    /// both threads. False on failure (errorMessage() says why); on failure
    /// nothing is left running and no file is left open.
    bool start( const CaptureBridgeParams &params );

    /// Stop the device, drain the ring to empty, finalise every file out of the
    /// pages, join both threads. Idempotent. BLOCKS — a caller that must not
    /// block (RecordingSession::requestStop) calls it from its own thread.
    void stop();

    bool isRunning() const { return running_.load( std::memory_order_acquire ); }

    /// The pages of the CURRENT (or most recent) capture segment. Outlives
    /// stop() and endCapture() — the recorded audio is still there to be read,
    /// converted (toCapturingSource) or placed after the file is closed. Null
    /// when the bridge was started with `capturePages = false` and
    /// beginCapture() has not been called.
    ///
    /// NOT safe to hold across a beginCapture(): take a copy of the
    /// shared_ptr (that is what the app's recorder does) and the segment you
    /// were given stays alive and readable whatever the bridge does next.
    std::shared_ptr<twGrowingCaptureSource> source() const;

    /// Begin a capture SEGMENT on a bridge that is already running: a FRESH
    /// growing capture source, and fresh WAV writers. This is what makes
    /// "one input pump, three sinks" (design D7) true across a record start
    /// while monitoring — the device, its capture thread and its ring are
    /// never touched, so the monitored signal does not gap.
    /// Refuses when a segment is already open. Main thread.
    bool beginCapture( const std::vector<CaptureWavSink> &sinks );

    /// End the segment: stop appending, complete every file OUT OF THE PAGES
    /// (invariant 3) and close the writers. BLOCKS for as long as the WAV
    /// backlog takes to write. source() stays valid and readable afterwards.
    /// Idempotent. Main thread.
    void endCapture();

    bool isCapturing() const
    { return capturing_.load( std::memory_order_acquire ); }

    /// The steady-clock host time (MidiOutScheduler::hostNowNs()) at which
    /// frame 0 of the current segment was RECORDED — the pop time of the first
    /// batch, minus that batch's own duration, because a device hands over
    /// audio it has already captured. 0 before the first frame lands.
    ///
    /// This is the input side of design D6's anchor: the app maps it through
    /// the engine clock to get P0, the project frame of capture frame 0.
    std::int64_t captureStartHostNs() const
    { return captureStartHostNs_.load( std::memory_order_acquire ); }

    /// Turn the live-lane sink on or off while running (see
    /// CaptureBridgeParams::liveEnabled).
    void setLiveEnabled( bool on )
    { liveEnabled_.store( on, std::memory_order_release ); }
    bool liveEnabled() const
    { return liveEnabled_.load( std::memory_order_acquire ); }

    /// Frames captured so far == the growing source's frontier.
    std::uint64_t frontier() const;

    /// The LIVE-LANE sink, in twLiveInputSource::pull()'s shape: fill
    /// `channels` PLANAR buffers with `frames` frames, return how many were
    /// delivered (the caller zero-fills the shortfall). `pos` is accepted and
    /// IGNORED — a device ring has no random access, exactly as that interface
    /// documents. Deliberately not an override of twLiveInputSource: tw/record
    /// does not depend on tw/playback and one interface is not worth the edge;
    /// the app's ten-line adapter (L1b/L3b) forwards to this.
    ///
    /// Called on the PUMP THREAD: no lock, no allocation, no blocking.
    std::size_t pullLive( float *const *out, std::size_t channels,
                          std::size_t frames, offset_t pos = 0 );

    /// Drop everything the live ring holds. Control plane (a reposition or an
    /// arm), never called concurrently with pullLive().
    void clearLive();

    std::uint32_t inputChannels() const { return inputChannels_; }
    std::uint32_t inputRate() const { return inputRate_; }
    std::uint32_t targetRate() const { return targetRate_; }
    // Ask the OPEN device to make these input channels available (bit n ==
    // input n). Grow-only and, on ASIO, deferred to the next start when the
    // stream is already running — so calling it per arm is safe and cheap.
    // Returns the backend's answer: 0 applied/already open, 1 deferred, -1 no
    // device.
    int requestInputChannels( std::uint64_t mask );

    std::uint32_t inputLatencyFrames() const { return inputLatencyFrames_; }

    CaptureBridgeStats stats() const;
    const char *errorMessage() const { return lastError_.c_str(); }

    /// Called ON THE BRIDGE THREAD after every batch appended to the pages,
    /// with the new frontier. Must be realtime-safe: an atomic store, never Qt.
    std::function<void( std::uint64_t frontier, std::size_t appended )> onFrames;

private:
    void bridgeThreadMain();
    void wavThreadMain();
    /// Open the writers for `sinks` against `src`. Caller holds sinkM_.
    bool openSinks_( const std::vector<CaptureWavSink> &sinks,
                     const std::shared_ptr<twGrowingCaptureSource> &src );
    /// Write everything from wavCursor_ up to the frontier into every sink.
    /// Frames written after the capture has ENDED are counted in
    /// wavFinalized_ — same code path, because "the file is finalised from the
    /// pages" is a property of the ordinary writer loop rather than a second
    /// one; what makes those frames special is only WHEN they were written.
    void drainToWriters();
    /// The named entry point of design D7: complete every file out of the
    /// capture pages once the device has stopped and the ring is empty.
    void finalizeFromPages() { drainToWriters(); }
    void closeWriters();

    struct WavSinkState;

    std::unique_ptr<AudioInput> ownedInput_;
    AudioInput                 *input_ = nullptr;

    std::shared_ptr<twGrowingCaptureSource> source_;
    std::unique_ptr<AudioRing>              liveRing_;
    std::vector<std::unique_ptr<WavSinkState> > sinks_;   // under sinkM_
    std::function<std::unique_ptr<AudioFileWriter>()> writerFactory_;

    std::thread bridgeThread_;
    std::thread wavThread_;

    // TWO mutexes, and the order is always sinkM_ BEFORE m_ — never the
    // reverse. `m_` is the short one the BRIDGE thread takes (its snapshot of
    // the segment, its append, its condition-variable wait); `sinkM_` is the
    // long one the WAV thread holds across a whole drain, so a control-plane
    // call that must not interleave with a write (beginCapture/endCapture)
    // takes it first and then m_. Putting the drain under `m_` instead would
    // let a large backlog block the bridge thread for the length of a file
    // write, which is exactly what invariant 2 forbids.
    mutable std::mutex      m_;
    std::mutex              sinkM_;
    std::condition_variable bridgeCv_;
    std::condition_variable wavCv_;

    std::atomic<bool> running_{ false };
    // Wall clock at start(), for the end-of-run capture-rate check (main PR #55).
    std::chrono::steady_clock::time_point startWall_{};
    std::atomic<bool> bridgeStop_{ false };
    // Set by the bridge thread as it exits: from here on the frontier is
    // final and everything the WAV thread still writes comes out of the pages.
    std::atomic<bool> captureEnded_{ false };
    std::atomic<bool> wavStop_{ false };
    // Is a capture SEGMENT open? Read by the bridge thread under m_, so
    // clearing it under m_ is a hard fence: no append can follow.
    std::atomic<bool> capturing_{ false };
    std::atomic<bool> liveEnabled_{ true };
    std::atomic<std::int64_t> captureStartHostNs_{ 0 };

    std::uint32_t inputChannels_ = 0;
    std::uint32_t inputRate_ = 0;
    std::uint32_t targetRate_ = 0;
    std::uint32_t inputLatencyFrames_ = 0;
    std::size_t   blockFrames_ = 1024;
    std::size_t   chunkFrames_ = 0;

    // WAV thread only, except under sinkM_ where a segment change resets it.
    std::uint64_t wavCursor_ = 0;
    // Frames appended to the CURRENT segment; 0 marks "the next append is
    // frame 0", which is when captureStartHostNs_ is stamped.
    std::uint64_t segFrames_ = 0;          // bridge thread, under m_

    std::atomic<std::uint64_t> framesIn_{ 0 };
    std::atomic<std::uint64_t> framesToPages_{ 0 };
    std::atomic<std::uint64_t> framesToLive_{ 0 };
    std::atomic<std::uint64_t> framesToWav_{ 0 };
    std::atomic<std::uint64_t> wavLate_{ 0 };
    std::atomic<std::uint64_t> wavFinalized_{ 0 };
    std::atomic<std::uint64_t> liveOverruns_{ 0 };
    // Latched in stats(), which is const — the device is gone by the time a
    // test asks, and "no overruns" is a claim about a run, not about a pointer.
    mutable std::atomic<std::uint64_t> ringOverruns_{ 0 };
    mutable std::atomic<std::uint64_t> ringUnderruns_{ 0 };
    std::atomic<std::uint64_t> bridgeWakeups_{ 0 };
    std::atomic<std::uint64_t> wavWakeups_{ 0 };

    // Live-ring pop scratch. Written only by the pump thread inside pullLive.
    std::vector<float> liveScratch_;

    std::string lastError_;
};

}  // namespace audio

#endif
