#ifndef _RENDER_SESSION_H_
#define _RENDER_SESSION_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "audio/audio_file_writer.h"

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
    bool start(twComponent *synthOutput, const RenderParams &params, std::uint32_t sampleRate);

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

private:
    void renderThreadMain();

    twComponent *synthOutput_ = nullptr;
    RenderParams params_;
    std::uint32_t sampleRate_ = 48000;
    std::size_t totalSamples_ = 0;
    std::atomic<std::size_t> samplesWritten_{0};
    std::atomic<bool> running_{false};
    std::atomic<bool> cancelRequested_{false};
    std::unique_ptr<std::thread> renderThread_;
    std::string lastError_;
    std::unique_ptr<AudioFileWriter> writer_;
};

}  // namespace audio

#endif
