#include "file_input.h"

#include "tw/core/twlog.h"
#include "tw/core/twsyslog.h"
#include "tw/devices/midi_out_scheduler.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#include <avrt.h>
#endif

namespace audio {

namespace {

// A minimal RIFF/WAVE reader.
//
// tw_devices depends on tw/core AND NOTHING ELSE (devices CONTRACT), and that
// is worth more than the ~120 lines below: libsndfile lives behind tw_sources /
// tw_sinks, and pulling it down here to read a test fixture would make the
// platform I/O layer depend on the codec stack. The formats handled are the
// ones this repo's fixtures and renders actually produce — PCM 16/24/32 and
// IEEE float32 — and anything else is refused loudly rather than misread.
struct WavData {
    std::vector<float> samples;     // interleaved
    std::uint32_t rate = 0;
    std::uint32_t channels = 0;
    std::size_t frames = 0;
};

std::uint32_t rdU32(const unsigned char *p)
{
    return (std::uint32_t)p[0] | ((std::uint32_t)p[1] << 8) |
           ((std::uint32_t)p[2] << 16) | ((std::uint32_t)p[3] << 24);
}
std::uint16_t rdU16(const unsigned char *p)
{
    return (std::uint16_t)((std::uint32_t)p[0] | ((std::uint32_t)p[1] << 8));
}

bool readWav(const std::string &path, WavData &out, std::string &err)
{
    std::FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) { err = "cannot open '" + path + "'"; return false; }

    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size < 44) { std::fclose(f); err = "not a WAV (too short)"; return false; }

    std::vector<unsigned char> buf((std::size_t)size);
    const std::size_t got = std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    if (got != buf.size()) { err = "short read"; return false; }

    if (std::memcmp(buf.data(), "RIFF", 4) != 0 ||
        std::memcmp(buf.data() + 8, "WAVE", 4) != 0) {
        err = "not a RIFF/WAVE file";
        return false;
    }

    std::uint16_t format = 0, bits = 0, channels = 0;
    std::uint32_t rate = 0;
    const unsigned char *data = nullptr;
    std::size_t dataBytes = 0;

    std::size_t p = 12;
    while (p + 8 <= buf.size()) {
        const char *id = (const char *)&buf[p];
        const std::uint32_t sz = rdU32(&buf[p + 4]);
        const std::size_t body = p + 8;
        if (body + sz > buf.size()) break;          // truncated chunk: stop here
        if (std::memcmp(id, "fmt ", 4) == 0 && sz >= 16) {
            format   = rdU16(&buf[body]);
            channels = rdU16(&buf[body + 2]);
            rate     = rdU32(&buf[body + 4]);
            bits     = rdU16(&buf[body + 14]);
            if (format == 0xFFFE && sz >= 40)        // WAVE_FORMAT_EXTENSIBLE
                format = rdU16(&buf[body + 24]);     // the sub-format tag
        } else if (std::memcmp(id, "data", 4) == 0) {
            data = &buf[body];
            dataBytes = sz;
        }
        p = body + sz + (sz & 1);                    // chunks are word aligned
    }

    if (!data || !channels || !rate) { err = "no fmt/data chunk"; return false; }

    const std::size_t bytesPerSample = (std::size_t)bits / 8;
    if (bytesPerSample == 0) { err = "zero bit depth"; return false; }
    const std::size_t total = dataBytes / bytesPerSample;   // samples, all channels

    out.rate = rate;
    out.channels = channels;
    out.frames = total / channels;
    out.samples.resize(out.frames * channels);

    if (format == 1 && bits == 16) {
        for (std::size_t i = 0; i < out.samples.size(); ++i) {
            const std::int16_t v =
                (std::int16_t)rdU16(data + i * 2);
            out.samples[i] = (float)v / 32768.0f;
        }
    } else if (format == 1 && bits == 24) {
        for (std::size_t i = 0; i < out.samples.size(); ++i) {
            const unsigned char *s = data + i * 3;
            std::int32_t v = (std::int32_t)((std::uint32_t)s[0] << 8 |
                                            (std::uint32_t)s[1] << 16 |
                                            (std::uint32_t)s[2] << 24);
            out.samples[i] = (float)(v >> 8) / 8388608.0f;
        }
    } else if (format == 1 && bits == 32) {
        for (std::size_t i = 0; i < out.samples.size(); ++i)
            out.samples[i] = (float)(std::int32_t)rdU32(data + i * 4) / 2147483648.0f;
    } else if (format == 3 && bits == 32) {
        std::memcpy(out.samples.data(), data, out.samples.size() * sizeof(float));
    } else {
        char msg[128];
        std::snprintf(msg, sizeof(msg),
                      "unsupported WAV encoding (format %u, %u bit)",
                      (unsigned)format, (unsigned)bits);
        err = msg;
        return false;
    }
    return true;
}

}  // namespace

FileAudioInput::FileAudioInput(std::string path) : path_(std::move(path))
{
    config_.sampleRate = 48000;
    config_.channels = 2;
    config_.bufferFrames = (std::uint32_t)kBlockFrames;
    config_.sampleType = twSampleType::Float32;
}

FileAudioInput::~FileAudioInput()
{
    closeDevice();
}

void FileAudioInput::setReportedLatencyFrames(std::uint32_t frames)
{
    config_.inputLatencyFrames = frames;
}

int FileAudioInput::openDevice(const std::string &, std::uint32_t preferredRate)
{
    if (open_) return 0;

    WavData w;
    std::string err;
    if (!readWav(path_, w, err)) {
        lastError_ = "file input: " + err;
        syslog(LOG_WARNING, "devices: %s", lastError_.c_str());
        return -1;
    }

    samples_ = std::move(w.samples);
    fileFrames_ = w.frames;
    config_.sampleRate = w.rate;
    config_.channels = w.channels;
    config_.bufferFrames = (std::uint32_t)kBlockFrames;

    // The file's rate is what it is. A caller asking for another one gets the
    // truth and resamples (RecordingSession already does exactly that for a
    // device whose shared engine refuses the project rate) — silently
    // relabelling the file would move every frame in time.
    if (preferredRate && preferredRate != w.rate) {
        TW_LOGI( "devices",
                "file input: '%s' is %u Hz; the caller asked for %u Hz and will "
                "resample", path_.c_str(), w.rate, preferredRate );
    }

    // 16 blocks of headroom: the consumer polls, and a scheduling hiccup on its
    // side must cost latency, not audio.
    ring_.reset(config_.channels, kBlockFrames * 16);
    open_ = true;
    atEnd_.store(false, std::memory_order_relaxed);

    TW_LOGI( "devices", "file input: '%s' %zu frames, %u Hz, %u ch, "
                        "%s, reported latency %u frames",
            path_.c_str(), fileFrames_, config_.sampleRate, config_.channels,
            loop_ ? "looping" : "stop at end", config_.inputLatencyFrames );
    return 0;
}

int FileAudioInput::closeDevice()
{
    stopCapture();
    open_ = false;
    samples_.clear();
    fileFrames_ = 0;
    return 0;
}

int FileAudioInput::startCapture()
{
    if (!open_) { lastError_ = "file input: device not open"; return -1; }
    if (running_.load(std::memory_order_acquire)) return 0;

    ring_.clear();
    ring_.resetStats();
    blocksDelivered_.store(0, std::memory_order_relaxed);
    wakeups_.store(0, std::memory_order_relaxed);
    atEnd_.store(false, std::memory_order_relaxed);

    blockLog_.assign(kMaxBlockLog, 0);
    blockLogCount_.store(0, std::memory_order_release);

    waiter_.open();
    // The schedule is anchored HERE, on the control-plane call, not inside the
    // thread: a caller that wants to check the pacing (or, later, convert a
    // block index to a host time) must be able to read the anchor the instant
    // startCapture() returns, and a thread that has not been scheduled yet
    // would still be reporting 0.
    startHostNs_.store(MidiOutScheduler::hostNowNs(), std::memory_order_release);
    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this] { captureThreadMain(); });
    return 0;
}

int FileAudioInput::stopCapture()
{
    if (!thread_.joinable()) {
        running_.store(false, std::memory_order_release);
        return 0;
    }
    running_.store(false, std::memory_order_release);
    waiter_.wake();
    thread_.join();                 // Qt-free join (THREADING.md rule 1)
    waiter_.close();
    startHostNs_.store(0, std::memory_order_release);
    return 0;
}

void FileAudioInput::captureThreadMain()
{
    tw::TwLog::markNonBlocking();
    tw::TwLog::nameThread( "audio-in-file" );

    // MMCSS "Pro Audio", exactly as the WASAPI RENDER thread does
    // (wasapi_backend.cc). A device's capture thread runs at device priority;
    // this one is standing in for a device, and without the promotion an
    // ordinary desktop scheduling hiccup shows up as delivery jitter that the
    // real thing would not have had. Failure is ignored — the pacing is then
    // merely worse, never wrong.
#if defined(_WIN32)
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
#endif

    const std::int64_t period =
        (std::int64_t)((double)kBlockFrames * 1e9 / (double)config_.sampleRate);
    const std::int64_t t0 = startHostNs_.load(std::memory_order_acquire);

    std::size_t cursor = 0;                       // frame index into the file
    std::uint64_t block = 0;
    std::vector<float> scratch(kBlockFrames * config_.channels, 0.0f);

    while (running_.load(std::memory_order_acquire)) {
        // Block i is DUE one period after the previous one: a device hands over
        // audio it has already recorded, so nothing can arrive at t0 itself.
        const std::int64_t due = t0 + (std::int64_t)(block + 1) * period;
        waiter_.waitUntil(due);
        if (!running_.load(std::memory_order_acquire)) break;

        if (fileFrames_ == 0) break;

        // Fill one block from the file, wrapping when looping.
        std::size_t filled = 0;
        while (filled < kBlockFrames) {
            if (cursor >= fileFrames_) {
                if (!loop_) break;
                cursor = 0;
            }
            const std::size_t n =
                std::min(kBlockFrames - filled, fileFrames_ - cursor);
            std::memcpy(&scratch[filled * config_.channels],
                        &samples_[cursor * config_.channels],
                        n * config_.channels * sizeof(float));
            filled += n;
            cursor += n;
        }

        if (filled > 0) {
            // Stamp BEFORE the push: the push is the delivery, and a stamp
            // taken after it would fold the memcpy into the pacing number.
            const std::int64_t at = MidiOutScheduler::hostNowNs();
            const std::size_t n = blockLogCount_.load(std::memory_order_relaxed);
            if (n < kMaxBlockLog) {
                blockLog_[n] = at;
                blockLogCount_.store(n + 1, std::memory_order_release);
            }
            ring_.push(scratch.data(), filled);
            wakeups_.fetch_add(1, std::memory_order_relaxed);
            blocksDelivered_.fetch_add(1, std::memory_order_relaxed);
        }
        ++block;

        if (!loop_ && cursor >= fileFrames_) {
            // Played out. Stay alive (the consumer may still be draining the
            // ring) but deliver nothing further — a stopped device, not a
            // closed one.
            atEnd_.store(true, std::memory_order_release);
            break;
        }
    }

#if defined(_WIN32)
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
#endif
}

std::size_t FileAudioInput::blockLog(std::vector<std::int64_t> &out) const
{
    const std::size_t n = blockLogCount_.load(std::memory_order_acquire);
    out.assign(blockLog_.begin(), blockLog_.begin() + (std::ptrdiff_t) n);
    return n;
}

std::int32_t FileAudioInput::read(float *interleaved, std::size_t frameCount)
{
    if (!interleaved) return -1;
    return (std::int32_t)ring_.pop(interleaved, frameCount);
}

AudioInputStats FileAudioInput::stats() const
{
    AudioInputStats s;
    s.framesPushed = ring_.framesPushed();
    s.framesPopped = ring_.framesPopped();
    s.overrunFrames = ring_.overrunFrames();
    s.underrunFrames = ring_.underrunFrames();
    s.captureWakeups = wakeups_.load(std::memory_order_relaxed);
    return s;
}

std::vector<AudioInputDeviceInfo> FileAudioInput::listDevices() const
{
    return {{"file", "File input (" + path_ + ")", config_.channels}};
}

}  // namespace audio
