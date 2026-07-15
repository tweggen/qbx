#ifndef _RENDER_SESSION_H_
#define _RENDER_SESSION_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "tw/sinks/audio_file_writer.h"
#include "tw/playback/audio_engine.h"
#include "tw/sinks/file_sink.h"

class twComponent;

namespace audio {

struct RenderParams {
    enum class Extent { EntireProject, TimeSelection };

    Extent extent = Extent::EntireProject;
    double startTimeSec = 0.0;
    double endTimeSec = 0.0;
    AudioFormat format = AudioFormat::WAV;
    int quality = 6;  // 0-10 for OGG
    std::string outputPath;
};

class RenderSession {
public:
    RenderSession();
    ~RenderSession();

    // Start rendering. Returns false if already rendering or invalid params.
    bool start(std::shared_ptr<twComponent> synthOutput, const RenderParams &params, std::uint32_t sampleRate);

    // Request cancellation. Safe to call from any thread.
    void requestCancel();

    // Query state (safe to call from any thread)
    bool isRunning() const;
    std::size_t samplesWritten() const;
    std::size_t totalSamples() const;
    const char *errorMessage() const;

    // Callbacks (called from render thread, must be thread-safe)
    std::function<void(std::size_t written, std::size_t total)> onProgress;
    std::function<void(bool success, const char *error)> onComplete;
    // Playhead publication: absolute project position in frames, so the app
    // can move its locator along with the render. Called from the render
    // thread — the handler must be realtime-safe (atomic store, no Qt/UI).
    // Replaces the former direct SApplication call (proposal 14, Phase 0).
    std::function<void(std::uint64_t absPos)> onPosition;

private:
    void renderThreadMain();

    std::shared_ptr<twComponent> synthOutput_;
    RenderParams params_;
    std::uint32_t sampleRate_ = 48000;
    std::size_t totalSamples_ = 0;
    std::size_t startOffsetSamples_ = 0;
    std::atomic<std::size_t> samplesWritten_{0};
    std::atomic<bool> running_{false};
    std::atomic<bool> cancelRequested_{false};
    std::unique_ptr<std::thread> renderThread_;
    std::string lastError_;
    std::unique_ptr<AudioFileWriter> writer_;
    std::unique_ptr<FileSink> fileSink_;        // Buffered output with futures
};

}  // namespace audio

#endif
