#include "tw/record/recording_session.h"

#include "tw/core/twlog.h"
#include "tw/record/capture_bridge.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

// Diagnostic output through the log sink under the "record" category, tagged
// with the function.
#define RECSESS_LOG( fmt, ... ) \
    TW_LOGD( "record", "RecordingSession::%s: " fmt, __func__, ##__VA_ARGS__ )

namespace audio {

RecordingSession::RecordingSession() {}

RecordingSession::~RecordingSession() {
    requestStop();
    joinSessionThread();
    bridge_.reset();
}

void RecordingSession::joinSessionThread() {
    // Always join a joinable thread, even after normal completion: the session
    // thread clears running_ before it returns, so gating the join on running_
    // left the finished thread un-joined (it would lurk, and a later
    // destroy/restart hit std::thread's terminate-on-joinable).
    if (sessionThread_ && sessionThread_->joinable()) {
        sessionThread_->join();
    }
    sessionThread_.reset();
}

void RecordingSession::markFinished(bool ok) {
    // Publish the terminal state for the polling UI. lastError_ must already be
    // set; finished_ is stored last with release ordering so a GUI thread that
    // sees isFinished()==true also sees succeeded_/lastError_.
    succeeded_.store(ok);
    running_.store(false);
    finished_.store(true, std::memory_order_release);
}

bool RecordingSession::start(const RecordingParams &params) {
    if (running_) {
        lastError_ = "Recording already in progress";
        return false;
    }

    if (params.armedTrackIds.empty()) {
        lastError_ = "No tracks armed for recording";
        return false;
    }

    if (params.projectDirectory.empty()) {
        lastError_ = "Project directory not specified";
        return false;
    }

    joinSessionThread();

    params_ = params;
    recordedDuration_ = 0.0;
    createdFiles_.clear();
    inputLatencyFrames_ = 0;
    lastError_.clear();

    stopRequested_ = false;
    finished_ = false;
    succeeded_ = false;

    // One file per armed track, named exactly as before (the app matches
    // created files to tracks POSITIONALLY — smainwindow.cpp).
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()) % 1000;

    CaptureBridgeParams bp;
    bp.inputDeviceId = params_.inputDeviceId.empty() ? std::string("default")
                                                     : params_.inputDeviceId;
    bp.targetRate = params_.sampleRate;

    for (std::size_t trackIdx = 0; trackIdx < params_.armedTrackIds.size(); ++trackIdx) {
        std::stringstream filenameSS;
        filenameSS << params_.projectDirectory << "/";
        filenameSS << std::put_time(std::localtime(&time), "%Y%m%d_%H%M%S");
        filenameSS << "_" << std::setfill('0') << std::setw(3) << ms.count();
        filenameSS << "_" << params_.armedTrackIds[trackIdx] << ".wav";

        CaptureWavSink sink;
        sink.path = filenameSS.str();
        sink.channelMask = (trackIdx < params_.trackChannels.size())
                               ? params_.trackChannels[trackIdx] : 0;
        createdFiles_.push_back(sink.path);
        bp.wavSinks.push_back(sink);
    }

    bridge_ = std::unique_ptr<CaptureBridge>(new CaptureBridge());

    // The playhead: published from the BRIDGE THREAD, per batch, as an atomic
    // store. `frontier` is project-rate frames captured so far, which is
    // exactly what the old loop counted by hand.
    const std::uint64_t startLocator = params_.startLocatorFrames;
    const std::uint32_t rate = params_.sampleRate ? params_.sampleRate : 48000;
    bridge_->onFrames = [this, startLocator, rate](std::uint64_t frontier,
                                                   std::size_t /*appended*/) {
        recordedDuration_.store(static_cast<double>(frontier) / rate);
        if (onPosition) onPosition(startLocator + frontier);
    };

    if (!bridge_->start(bp)) {
        lastError_ = bridge_->errorMessage();
        RECSESS_LOG("bridge failed to start: %s", lastError_.c_str());
        createdFiles_.clear();
        markFinished(false);
        if (onComplete) onComplete(false, lastError_.c_str());
        return false;
    }

    inputLatencyFrames_ = bridge_->inputLatencyFrames();
    running_ = true;

    try {
        sessionThread_ = std::unique_ptr<std::thread>(
            new std::thread([this] { sessionThreadMain(); }));
    } catch (const std::exception &e) {
        bridge_->stop();
        running_ = false;
        lastError_ = std::string("Failed to start recording thread: ") + e.what();
        markFinished(false);
        if (onComplete) onComplete(false, lastError_.c_str());
        return false;
    }

    return true;
}

void RecordingSession::requestStop() {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        stopRequested_ = true;
    }
    cv_.notify_all();
}

bool RecordingSession::isRunning() const {
    return running_;
}

bool RecordingSession::isFinished() const {
    return finished_.load(std::memory_order_acquire);
}

bool RecordingSession::succeeded() const {
    return succeeded_.load();
}

double RecordingSession::recordedDurationSeconds() const {
    return recordedDuration_;
}

const char *RecordingSession::errorMessage() const {
    return lastError_.c_str();
}

const std::vector<std::string> &RecordingSession::createdFiles() const {
    return createdFiles_;
}

void RecordingSession::sessionThreadMain() {
    // THE loop that used to be a 1 ms poll of the device. There is nothing to
    // poll any more: the bridge thread drains the input ring and the WAV thread
    // writes the files, so this thread only waits for the transport to stop —
    // waking on a 100 ms timeout purely to emit onProgress, which is the rate
    // the progress dialog was always given.
    for (;;) {
        std::unique_lock<std::mutex> lk(mutex_);
        cv_.wait_for(lk, std::chrono::milliseconds(100),
                     [this] { return stopRequested_.load(); });
        const bool stopping = stopRequested_.load();
        lk.unlock();

        if (onProgress) onProgress(recordedDuration_.load());
        if (stopping) break;
    }

    // Finalise: stop the device, drain the ring, complete every file out of the
    // capture pages. This BLOCKS, which is exactly why it is on this thread and
    // not in requestStop().
    bridge_->stop();

    const CaptureBridgeStats s = bridge_->stats();
    RECSESS_LOG("finished — %llu frames in, %llu to pages, %llu to WAV "
                "(%llu finalised from pages, late high-water %llu frames), "
                "ring overruns %llu, page drops %llu",
                (unsigned long long) s.framesIn, (unsigned long long) s.framesToPages,
                (unsigned long long) s.framesToWav, (unsigned long long) s.wavFinalized,
                (unsigned long long) s.wavLate, (unsigned long long) s.ringOverruns,
                (unsigned long long) s.pageDrops);

    bool success = true;
    std::string errorMsg = bridge_->errorMessage();
    if (!errorMsg.empty()) success = false;

    lastError_ = errorMsg;
    markFinished(success);

    // Legacy callback (not used by the progress dialog, which polls instead).
    if (onComplete) {
        onComplete(success, lastError_.c_str());
    }
}

}  // namespace audio
