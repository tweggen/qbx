#include "audio/audio_engine.h"

#include "twcomponent.h"
#include "tw303aenv.h"
#include <algorithm>
#include <cstring>

namespace audio {

AudioEngine::AudioEngine(twComponent* synthOutput, uint32_t sampleRate)
    : synthOutput_(synthOutput),
      engineSampleRate_(sampleRate),
      rateRatio_(1.0)
{
    // Resamplers start in identity config (will be updated via configureResampling)
    resamplerL_.configure(sampleRate, sampleRate);
    resamplerR_.configure(sampleRate, sampleRate);
    resamplerL_.reset();
    resamplerR_.reset();
}

AudioEngine::~AudioEngine() = default;

bool AudioEngine::pullFrame(AudioFrame& outFrame) {
    float outL, outR;
    if (!pullStereoFrame(outL, outR)) {
        outFrame.channels[0] = 0.0f;
        outFrame.channels[1] = 0.0f;
        return false;
    }

    outFrame.channels[0] = outL;
    outFrame.channels[1] = outR;
    outFrame.numChannels = 2;
    outFrame.sampleRate = engineSampleRate_;
    return true;
}

length_t AudioEngine::pullBlock(float* outL, float* outR, length_t nFrames) {
    if (!synthOutput_ || nFrames == 0) {
        std::memset(outL, 0, nFrames * sizeof(float));
        std::memset(outR, 0, nFrames * sizeof(float));
        return 0;
    }

    // Acquire loop state (atomic, lock-free)
    const bool loopValid = cycleEnabled_.load(std::memory_order_relaxed);
    uint64_t pos = currentPos_.load(std::memory_order_relaxed);
    const uint64_t loopStart = loopStart_.load(std::memory_order_relaxed);
    const uint64_t loopEnd = loopEnd_.load(std::memory_order_relaxed);

    // If cycling and reached loop end, wrap to start
    if (loopValid && pos >= loopEnd) {
        if (loopEnd > loopStart) {
            synthOutput_->seekTo(loopStart);
            pos = loopStart;
        }
    }

    // Get output plugs
    twLatchOutput *outputPlug = synthOutput_->linkOutput(0);
    if (!outputPlug) {
        std::memset(outL, 0, nFrames * sizeof(float));
        std::memset(outR, 0, nFrames * sizeof(float));
        currentPos_.store(pos, std::memory_order_relaxed);
        return 0;
    }

    auto *inputL = static_cast<twLatchStreamingOutput*>(outputPlug);
    auto *inputR = (synthOutput_->getNOutputs() > 1)
                   ? static_cast<twLatchStreamingOutput*>(synthOutput_->linkOutput(1))
                   : nullptr;

    // Resample L channel (want nFrames at output rate)
    length_t inConsumed = 0;
    length_t gotL = resamplerL_.process(inputL, outL, nFrames, &inConsumed);

    // Resample R channel with same consumed count
    length_t gotR = 0;
    if (inputR) {
        gotR = resamplerR_.process(inputR, outR, gotL, nullptr);
        if (gotR < gotL) gotL = gotR;  // Use minimum
    } else {
        // No right channel; copy left to right
        std::memcpy(outR, outL, gotL * sizeof(float));
        gotR = gotL;
    }

    // Zero any remaining frames that weren't produced
    if (gotL < nFrames) {
        std::memset(outL + gotL, 0, (nFrames - gotL) * sizeof(float));
        std::memset(outR + gotL, 0, (nFrames - gotL) * sizeof(float));
    }

    // Update position (in input samples at component graph rate)
    pos += inConsumed;
    currentPos_.store(pos, std::memory_order_relaxed);

    return gotL;
}

bool AudioEngine::pullStereoFrame(float& outL, float& outR) {
    if (!synthOutput_) {
        outL = outR = 0.0f;
        return false;
    }

    // Acquire loop state (atomic, lock-free)
    const bool loopValid = cycleEnabled_.load(std::memory_order_relaxed);
    uint64_t pos = currentPos_.load(std::memory_order_relaxed);
    const uint64_t loopStart = loopStart_.load(std::memory_order_relaxed);
    const uint64_t loopEnd = loopEnd_.load(std::memory_order_relaxed);

    // If cycling and reached loop end, wrap to start
    if (loopValid && pos >= loopEnd) {
        if (loopEnd > loopStart) {
            synthOutput_->seekTo(loopStart);
            pos = loopStart;
        }
    }

    // Pull one frame from component graph
    twLatchOutput *outputPlug = synthOutput_->linkOutput(0);
    if (!outputPlug) {
        outL = outR = 0.0f;
        currentPos_.store(pos, std::memory_order_relaxed);
        return false;
    }

    auto *inputL = static_cast<twLatchStreamingOutput*>(outputPlug);
    auto *inputR = (synthOutput_->getNOutputs() > 1)
                   ? static_cast<twLatchStreamingOutput*>(synthOutput_->linkOutput(1))
                   : nullptr;

    // Resample L channel (want 1 frame at output rate)
    float bufL;
    length_t inConsumed = 0;
    length_t gotL = resamplerL_.process(inputL, &bufL, 1, &inConsumed);

    // Resample R channel with same consumed count
    float bufR;
    length_t gotR = 0;
    if (inputR) {
        gotR = resamplerR_.process(inputR, &bufR, gotL, nullptr);
        if (gotR < gotL) gotL = gotR;  // Use minimum
    } else {
        // No right channel; copy left
        bufR = bufL;
        gotR = gotL;
    }

    if (gotL <= 0) {
        outL = outR = 0.0f;
        currentPos_.store(pos, std::memory_order_relaxed);
        return false;
    }

    outL = bufL;
    outR = bufR;

    // Update position (in input samples at component graph rate)
    pos += inConsumed;
    currentPos_.store(pos, std::memory_order_relaxed);

    return true;
}

void AudioEngine::seekTo(uint64_t offsetSamples) {
    if (synthOutput_) {
        synthOutput_->seekTo(offsetSamples);
    }
    currentPos_.store(offsetSamples, std::memory_order_relaxed);
}

uint64_t AudioEngine::currentPosition() const {
    return currentPos_.load(std::memory_order_relaxed);
}

void AudioEngine::setLoopBoundaries(bool enabled, uint64_t start, uint64_t end) {
    loopStart_.store(start, std::memory_order_relaxed);
    loopEnd_.store(end, std::memory_order_relaxed);
    cycleEnabled_.store(enabled, std::memory_order_relaxed);
}

void AudioEngine::configureResampling(uint32_t inRate, uint32_t outRate) {
    resamplerL_.configure(inRate, outRate);
    resamplerR_.configure(inRate, outRate);
    resamplerL_.reset();
    resamplerR_.reset();
    rateRatio_ = (inRate > 0) ? ((double)outRate / (double)inRate) : 1.0;
}

}  // namespace audio
