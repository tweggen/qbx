#include "audio/file_sink.h"
#include "audio/audio_file_writer.h"
#include "audio/generation_promise.h"

#include <chrono>
#include <algorithm>

namespace audio {

FileSink::FileSink(AudioFileWriter* writer, size_t bufferFrames, int64_t ageTimeoutMs)
    : writer_(writer),
      maxBufferFrames_(bufferFrames),
      ageTimeoutMs_(ageTimeoutMs),
      currentGeneration_(0)
{
}

int64_t FileSink::getCurrentTimeMs() const {
    auto now = std::chrono::steady_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
}

bool FileSink::isFrameReady(const FrameEntry& entry) const {
    // Frame is ready if:
    // 1. Generation is stable (future resolved), OR
    // 2. Frame is old enough (age-based fallback)

    int64_t now = getCurrentTimeMs();
    int64_t frameAge = now - entry.createdTimeMs;

    // Non-blocking check on future (0ms timeout)
    if (entry.readyFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
        return true;
    }

    // Fallback: write stale data if aged
    if (frameAge >= ageTimeoutMs_) {
        return true;
    }

    return false;
}

void FileSink::flushReady() {
    std::lock_guard<std::mutex> lock(bufferMutex_);

    // Write all ready frames from the front of the buffer
    while (!buffer_.empty()) {
        if (isFrameReady(buffer_.front())) {
            const FrameEntry& entry = buffer_.front();
            if (writer_) {
                writer_->write(&entry.frame.channels[0], 1);  // Write stereo frame
            }
            buffer_.pop_front();
        } else {
            break;  // Stop at first non-ready frame
        }
    }
}

bool FileSink::writeFrame(const AudioFrame& frame) {
    std::lock_guard<std::mutex> lock(bufferMutex_);

    // Get the promise for the current generation
    auto promise = GenerationRegistry::instance().getOrCreate(currentGeneration_);

    FrameEntry entry{
        frame,
        currentGeneration_,
        promise->getFuture(),
        getCurrentTimeMs()
    };

    // If buffer is full, drop oldest frame (shouldn't happen in normal operation)
    if (buffer_.size() >= maxBufferFrames_) {
        buffer_.pop_front();
    }

    buffer_.push_back(entry);
    return true;
}

void FileSink::flush() {
    // Final flush: give futures brief grace period to resolve, then write everything
    {
        std::lock_guard<std::mutex> lock(bufferMutex_);

        // Wait briefly for in-flight futures
        for (auto& entry : buffer_) {
            entry.readyFuture.wait_for(std::chrono::milliseconds(100));
        }

        // Write all buffered frames (even if not ready, due to age timeout)
        for (auto& entry : buffer_) {
            if (writer_) {
                writer_->write(&entry.frame.channels[0], 1);
            }
        }

        buffer_.clear();
    }

    // Forget old generations (cleanup)
    if (currentGeneration_ > 0) {
        GenerationRegistry::instance().forget(currentGeneration_);
    }
}

void FileSink::setGeneration(uint32_t generation) {
    std::lock_guard<std::mutex> lock(bufferMutex_);
    currentGeneration_ = generation;
}

size_t FileSink::occupancy() const {
    std::lock_guard<std::mutex> lock(bufferMutex_);
    return buffer_.size();
}

}  // namespace audio
