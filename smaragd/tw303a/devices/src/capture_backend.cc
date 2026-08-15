#include "tw/devices/capture_backend.h"

// The block log below MUST be stamped with the same clock MIDI-out uses, or the
// testkit could not map a captured MIDI host time onto a project frame at all
// (proposal 37 D6). Calling hostNowNs() rather than re-deriving steady_clock
// here is what makes that impossible to break by editing only one of the two.
#include "tw/devices/midi_out_scheduler.h"

#include "tw/core/twsyslog.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace audio {

namespace {

// SMARAGD_CAPTURE_SPEED, clamped to something a deadline loop can honour.
// Read once per backend instance so a run's pacing is fixed.
double readSpeedFromEnv()
{
    const char *env = std::getenv("SMARAGD_CAPTURE_SPEED");
    if (!env || !*env) return 1.0;

    char  *end   = nullptr;
    double value = std::strtod(env, &end);
    if (end == env || !(value > 0.0)) {
        syslog(LOG_WARNING,
               "CaptureBackend: ignoring SMARAGD_CAPTURE_SPEED='%s' (not a positive number)",
               env);
        return 1.0;
    }
    // Below 0.05x a case would outlive any sane timeout; above 1000x the pump
    // is a busy loop and the pacing stops meaning anything.
    const double clamped = std::min(1000.0, std::max(0.05, value));
    if (clamped != value) {
        syslog(LOG_WARNING, "CaptureBackend: SMARAGD_CAPTURE_SPEED %.3f clamped to %.3f",
               value, clamped);
    }
    return clamped;
}

}  // namespace

CaptureBackend::CaptureBackend()
{
    config_.sampleRate   = 48000;
    config_.channels     = 2;
    config_.bufferFrames = 1024;
    config_.periodFrames = 1024;
    config_.sampleType   = twSampleType::Float32;
    speed_ = readSpeedFromEnv();
}

CaptureBackend::~CaptureBackend()
{
    stopOutput();
}

int CaptureBackend::openDevice(const std::string & /*deviceName*/,
                               std::uint32_t preferredRate)
{
    // No hardware to constrain us: adopt the requested rate, so the speaker's
    // resampler stays a passthrough and a captured frame count is a PROJECT
    // frame count. A test that had to undo a 44.1k/48k resampling before
    // decoding a position would be measuring the resampler, not the playback.
    if (preferredRate != 0) config_.sampleRate = preferredRate;

    config_.outputLatencyFrames = config_.bufferFrames;

    syslog(LOG_INFO,
           "audio: CaptureBackend active (%u Hz, %u ch, %u-frame blocks, %.2fx real time)"
           " — audio is recorded to memory, not played.",
           (unsigned) config_.sampleRate, (unsigned) config_.channels,
           (unsigned) config_.bufferFrames, speed_);
    return 0;
}

int CaptureBackend::closeDevice()
{
    stopOutput();
    // The recording deliberately SURVIVES the close: a case stops playback and
    // only then asks for the audio (dump-playback-capture), and twSpeaker::
    // stopOutput() closes the device on its way out.
    return 0;
}

void CaptureBackend::setRenderCallback(RenderCallback cb)
{
    std::lock_guard<std::mutex> lock(callbackMutex_);
    callback_ = std::move(cb);
}

int CaptureBackend::startOutput()
{
    if (running_.load(std::memory_order_relaxed)) return 0;

    clearCapture();

    {
        std::lock_guard<std::mutex> lock(captureMutex_);
        capture_.sampleRate = config_.sampleRate;
        capture_.channels   = config_.channels;
    }

    running_.store(true, std::memory_order_relaxed);
    pumpRunning_.store(true, std::memory_order_relaxed);
    pump_ = std::thread([this] { pumpLoop(); });
    return 0;
}

int CaptureBackend::stopOutput()
{
    pumpRunning_.store(false, std::memory_order_relaxed);
    if (pump_.joinable()) pump_.join();
    running_.store(false, std::memory_order_relaxed);
    return 0;
}

std::vector<AudioDeviceInfo> CaptureBackend::enumerateDevices() const
{
    // One synthetic endpoint. Not an empty list: an empty list means "only the
    // system default exists", and a device picker showing the system's real
    // speakers while this backend is active would be a lie.
    return { AudioDeviceInfo{ "capture", "Capture (in-memory test device)" } };
}

CaptureBackend::CaptureBuffer CaptureBackend::capturedAudio() const
{
    std::lock_guard<std::mutex> lock(captureMutex_);
    return capture_;
}

std::size_t CaptureBackend::capturedFrames() const
{
    std::lock_guard<std::mutex> lock(captureMutex_);
    return capture_.frames();
}

void CaptureBackend::clearCapture()
{
    std::lock_guard<std::mutex> lock(captureMutex_);
    capture_.samples.clear();
    capture_.droppedFrames = 0;
    // The block log describes THIS recording's frames; keeping it across a
    // clear would map host times onto frame indices that no longer exist.
    blockLog_.clear();
}

std::vector<CaptureBackend::BlockStamp> CaptureBackend::capturedBlockLog() const
{
    std::lock_guard<std::mutex> lock(captureMutex_);
    return blockLog_;
}

std::int64_t CaptureBackend::frameAtHostTime(std::int64_t hostTimeNs) const
{
    std::vector<BlockStamp> log;
    double nominal = 48000.0;
    {
        std::lock_guard<std::mutex> lock(captureMutex_);
        log = blockLog_;
        if (capture_.sampleRate) nominal = (double) capture_.sampleRate;
    }
    return frameAtHostTime(log, hostTimeNs, nominal * speed_);
}

std::int64_t CaptureBackend::frameAtHostTime(const std::vector<BlockStamp> &log,
                                             std::int64_t hostTimeNs,
                                             double nominalFramesPerSecond)
{
    if (log.empty()) return -1;

    auto interpolate = [](const BlockStamp &a, const BlockStamp &b,
                          std::int64_t t) -> double {
        const double dt = (double) (b.hostTimeNs - a.hostTimeNs);
        if (dt == 0.0) return (double) a.firstFrame;
        const double f = (double) (t - a.hostTimeNs) / dt;
        return (double) a.firstFrame + f * (double) (b.firstFrame - a.firstFrame);
    };

    const std::size_t n = log.size();
    if (n == 1) {
        const double dNs = (double) (hostTimeNs - log[0].hostTimeNs);
        return (std::int64_t) std::llround((double) log[0].firstFrame +
                                      dNs * nominalFramesPerSecond * 1e-9);
    }

    if (hostTimeNs <= log.front().hostTimeNs)
        return (std::int64_t) std::llround(interpolate(log[0], log[1], hostTimeNs));
    if (hostTimeNs >= log.back().hostTimeNs)
        return (std::int64_t) std::llround(interpolate(log[n - 2], log[n - 1], hostTimeNs));

    // The log is strictly increasing in host time (one pump thread appends it),
    // so a binary search is exact rather than merely fast.
    std::size_t lo = 0, hi = n - 1;
    while (hi - lo > 1) {
        const std::size_t mid = (lo + hi) / 2;
        if (log[mid].hostTimeNs <= hostTimeNs) lo = mid; else hi = mid;
    }
    return (std::int64_t) std::llround(interpolate(log[lo], log[hi], hostTimeNs));
}

void CaptureBackend::pumpLoop()
{
    using clock = std::chrono::steady_clock;

    const std::size_t   block    = config_.bufferFrames ? config_.bufferFrames : 1024;
    const std::uint32_t channels = config_.channels ? config_.channels : 2;
    const std::uint32_t rate     = config_.sampleRate ? config_.sampleRate : 48000;

    const std::uint64_t maxFrames = (std::uint64_t) rate * kMaxCaptureSeconds;

    // Nanoseconds of wall clock per block, scaled by the speed multiplier.
    const auto period = std::chrono::nanoseconds(
        (long long) ((double) block * 1e9 / ((double) rate * speed_)));

    std::vector<float> scratch(block * channels, 0.0f);

    auto     next        = clock::now();
    bool     warnedLate  = false;
    bool     warnedFull  = false;

    while (pumpRunning_.load(std::memory_order_relaxed)) {
        // The callback runs with NO lock of ours held — the platform backends
        // call it from a driver thread that holds nothing of ours either, and a
        // lock held across it would put our mutex on the RT path.
        // Stamped BEFORE the callback: this is the instant the device asked for
        // the block, which is what a wall-clock event has to be compared with.
        const std::int64_t blockHostNs = MidiOutScheduler::hostNowNs();

        std::size_t produced = 0;
        {
            RenderCallback cb;
            {
                std::lock_guard<std::mutex> lock(callbackMutex_);
                cb = callback_;
            }
            if (cb) {
                std::fill(scratch.begin(), scratch.end(), 0.0f);
                produced = cb(scratch.data(), block, channels);
                // A callback claiming more than it was asked for would index
                // past the scratch buffer below; the contract says "up to
                // `frames`", so clamp rather than trust.
                if (produced > block) produced = block;
            } else {
                std::fill(scratch.begin(), scratch.end(), 0.0f);
            }
        }
        // A short pull is silence, not an error (AudioBackend's contract). The
        // recording keeps the WHOLE block either way, because the DEVICE would
        // have consumed the whole block: dropping the tail would silently
        // compress the timeline and put every later frame at the wrong offset,
        // which is exactly the failure mode a position decode exists to catch.
        if (produced < block) {
            std::fill(scratch.begin() + (std::size_t)(produced * channels),
                      scratch.end(), 0.0f);
        }

        {
            std::lock_guard<std::mutex> lock(captureMutex_);
            const std::uint64_t have = (std::uint64_t) capture_.frames();
            if (have + block <= maxFrames) {
                blockLog_.push_back(BlockStamp{ blockHostNs, (std::int64_t) have });
                capture_.samples.insert(capture_.samples.end(),
                                        scratch.begin(), scratch.end());
            } else {
                // Past the cap nothing is stored, so nothing is stamped: a log
                // entry for a frame the recording does not contain would map
                // host times onto an index no decoder can reach.
                capture_.droppedFrames += block;
                if (!warnedFull) {
                    warnedFull = true;
                    syslog(LOG_WARNING,
                           "CaptureBackend: recording hit the %llu s cap; further frames are dropped",
                           (unsigned long long) kMaxCaptureSeconds);
                }
            }
        }

        next += period;
        const auto now = clock::now();
        if (next <= now) {
            // Fell behind (a slow callback, a loaded machine). Re-anchor rather
            // than sprint to catch up: a burst of back-to-back callbacks is the
            // one thing that would let the pump outrun real time, and the whole
            // point of the pacing is that it cannot.
            if (!warnedLate) {
                warnedLate = true;
                syslog(LOG_WARNING,
                       "CaptureBackend: pump fell behind its deadline by %lld us "
                       "(callback slower than real time); pacing re-anchored",
                       (long long) std::chrono::duration_cast<std::chrono::microseconds>(
                           now - next).count());
            }
            next = now;
        } else {
            std::this_thread::sleep_until(next);
        }
    }
}

}  // namespace audio
