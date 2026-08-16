#include "tw/sinks/file_sink.h"
#include "tw/sinks/audio_file_writer.h"
#include "tw/core/generation_promise.h"

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

bool FileSink::isBlockReady(const BlockEntry& entry) const {
    // A block is ready if:
    // 1. Generation is stable (future resolved), OR
    // 2. It is old enough (age-based fallback)

    int64_t now = getCurrentTimeMs();
    int64_t age = now - entry.createdTimeMs;

    // Non-blocking check on future (0ms timeout)
    if (entry.readyFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
        return true;
    }

    // Fallback: write stale data if aged
    if (age >= ageTimeoutMs_) {
        return true;
    }

    return false;
}

// Caller holds bufferMutex_ and has already established that the front block is
// ready. Writes it out and pops it.
void FileSink::writeFront_locked() {
    const BlockEntry& entry = buffer_.front();
    if (writer_ && entry.frames > 0) {
        writer_->write(entry.samples.data(), entry.frames);
    }
    bufferedFrames_ -= entry.frames;
    buffer_.pop_front();
}

bool FileSink::writeFrames(const float *interleaved, std::size_t nFrames,
                           unsigned channels) {
    if (!interleaved || nFrames == 0 || channels == 0) return true;

    std::lock_guard<std::mutex> lock(bufferMutex_);

    // Get the promise for the current generation
    auto promise = GenerationRegistry::instance().getOrCreate(currentGeneration_);

    BlockEntry entry;
    entry.samples.assign(interleaved, interleaved + nFrames * (std::size_t) channels);
    entry.frames        = nFrames;
    entry.channels      = channels;
    entry.generation    = currentGeneration_;
    entry.readyFuture   = promise->getFuture();
    entry.createdTimeMs = getCurrentTimeMs();

    bufferedFrames_ += nFrames;
    buffer_.push_back(std::move(entry));

    // Drain ready blocks when the buffer reaches the drain threshold, to prevent
    // unbounded growth. For rendering (generation 0) blocks are ready as soon as
    // the generation's promise resolves, so this flushes to disk well before any
    // cap is hit.
    while (!buffer_.empty() && bufferedFrames_ > maxBufferFrames_ / 2) {
        if (isBlockReady(buffer_.front())) {
            writeFront_locked();
        } else {
            break;
        }
    }

    return true;
}

void FileSink::flush() {
    // Mark generation complete so all pending futures resolve immediately
    GenerationRegistry::instance().markComplete(currentGeneration_);

    {
        std::lock_guard<std::mutex> lock(bufferMutex_);

        // Write all buffered blocks
        while (!buffer_.empty()) {
            writeFront_locked();
        }
        bufferedFrames_ = 0;
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
    return bufferedFrames_;
}

}  // namespace audio
