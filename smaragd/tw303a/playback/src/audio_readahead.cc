#include "tw/playback/audio_readahead.h"

#include <chrono>
#include <iostream>

namespace audio {

AudioReadaheadBuffer::AudioReadaheadBuffer(uint32_t /* sampleRate */, size_t readaheadFrames)
    : maxFrames_(readaheadFrames)
{
}

AudioReadaheadBuffer::~AudioReadaheadBuffer() = default;

bool AudioReadaheadBuffer::pullFrame(AudioFrame& outFrame) {
    std::unique_lock<std::mutex> lock(bufferMutex_);

    // Wait briefly (5ms) if buffer is empty
    if (buffer_.empty()) {
        bufferNotEmpty_.wait_for(lock, std::chrono::milliseconds(5),
                                 [this] { return !buffer_.empty(); });
    }

    // If still empty after timeout, return silence (stale/incomplete capture)
    if (buffer_.empty()) {
        outFrame.channels[0] = 0.0f;
        outFrame.channels[1] = 0.0f;
        return false;
    }

    // Consume oldest frame
    outFrame = buffer_.front();
    buffer_.pop_front();
    return true;
}

bool AudioReadaheadBuffer::pushFrame(const AudioFrame& frame) {
    std::lock_guard<std::mutex> lock(bufferMutex_);

    // If buffer is full, drop oldest frame (make room for new)
    if (buffer_.size() >= maxFrames_) {
        buffer_.pop_front();
        droppedFrames_.fetch_add(1, std::memory_order_relaxed);
    }

    buffer_.push_back(frame);
    bufferNotEmpty_.notify_one();
    return true;
}

size_t AudioReadaheadBuffer::occupancy() const {
    std::lock_guard<std::mutex> lock(bufferMutex_);
    return buffer_.size();
}

void AudioReadaheadBuffer::reset() {
    std::lock_guard<std::mutex> lock(bufferMutex_);
    buffer_.clear();
    bufferNotEmpty_.notify_all();
}

}  // namespace audio
