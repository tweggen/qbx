#include "audio/audio_engine.h"

#include "twcomponent.h"
#include "tw_output_page.h"
#include "tw303aenv.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace audio {

AudioEngine::AudioEngine(twComponent* synthOutput, uint32_t sampleRate)
    : synthOutput_(synthOutput),
      engineSampleRate_(sampleRate),
      currentPageStartPos_(0),
      pageFrameOffset_(0),
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
    if (!pullStereoFrameFrozen(outL, outR)) {
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

    // Tier 1 Enhancement: Use frozen pages for state-continuous rendering
    // If resampling needed (engine rate != output rate), pull more frames and resample
    if (!resamplerL_.isPassthrough()) {
        // Pull at engine rate, then resample to output rate
        // rateRatio_ = outputRate / inputRate (e.g., 44100/48000 = 0.9187)
        // We need more input frames when downsampling: inFrames = outFrames / rateRatio_
        double invRatio = 1.0 / rateRatio_;  // inputRate / outputRate (e.g., 48000/44100 = 1.0884)
        length_t inFramesNeeded = (length_t)std::ceil(nFrames * invRatio);
        std::vector<float> bufL(inFramesNeeded), bufR(inFramesNeeded);

        // Track current position BEFORE consuming input frames
        // This allows us to advance by the actual output frame count, not the input count
        uint64_t posBeforeResample = currentPos_.load(std::memory_order_relaxed);

        length_t inProduced = 0;
        for (length_t i = 0; i < inFramesNeeded; ++i) {
            if (!pullStereoFrameFrozen(bufL[i], bufR[i])) {
                break;
            }
            inProduced++;
        }

        if (inProduced == 0) {
            std::memset(outL, 0, nFrames * sizeof(float));
            std::memset(outR, 0, nFrames * sizeof(float));
            return 0;
        }

        // Linear interpolation resampling: advance through input at invRatio rate
        // For output sample i, read from input at position i * invRatio
        double step = invRatio;  // inputRate / outputRate
        length_t outProduced = 0;
        for (length_t i = 0; i < nFrames; ++i) {
            double pos = (double)i * step;
            length_t k = (length_t)pos;
            double frac = pos - (double)k;

            if (k + 1 < inProduced) {
                outL[i] = bufL[k] + (bufL[k+1] - bufL[k]) * (float)frac;
                outR[i] = bufR[k] + (bufR[k+1] - bufR[k]) * (float)frac;
            } else if (k < inProduced) {
                outL[i] = bufL[k];
                outR[i] = bufR[k];
            } else {
                outL[i] = outR[i] = 0.0f;
            }
            outProduced++;
        }

        // CRITICAL: Adjust currentPos_ to reflect output frames produced, not input frames consumed.
        // currentPos_ was advanced by pullStereoFrameFrozen() calls (inProduced times).
        // But we're outputting outProduced frames. The discrepancy causes position lag.
        // Correct it: currentPos_ should be posBeforeResample + outProduced (in engine rate).
        // But we consumed inProduced engine-rate frames, so currentPos_ is now
        // posBeforeResample + inProduced. We need to subtract the excess.
        // Actually, currentPos_ should represent the engine-rate position that corresponds
        // to the output-rate frame we're about to produce. This is complex with resampling.
        // For now: set currentPos_ to the engine-rate equivalent of output position:
        // If we've output outProduced frames at output rate, that's outProduced / rateRatio_
        // frames at engine rate.
        uint64_t engineFramesForOutput = (uint64_t)std::round((double)outProduced / rateRatio_);
        currentPos_.store(posBeforeResample + engineFramesForOutput, std::memory_order_relaxed);

        return outProduced;
    }

    // Passthrough: no resampling needed
    length_t produced = 0;
    for (length_t i = 0; i < nFrames; ++i) {
        if (!pullStereoFrameFrozen(outL[i], outR[i])) {
            break;
        }
        produced++;
    }

    // Zero any remaining frames that weren't produced
    if (produced < nFrames) {
        std::memset(outL + produced, 0, (nFrames - produced) * sizeof(float));
        std::memset(outR + produced, 0, (nFrames - produced) * sizeof(float));
    }

    return produced;
}

bool AudioEngine::pullStereoFrameFrozen(float& outL, float& outR) {
    if (!synthOutput_) {
        outL = outR = 0.0f;
        return false;
    }

    // Acquire loop state (atomic, lock-free)
    const bool loopValid = cycleEnabled_.load(std::memory_order_relaxed);
    uint64_t pos = currentPos_.load(std::memory_order_relaxed);
    const uint64_t loopStart = loopStart_.load(std::memory_order_relaxed);
    const uint64_t loopEnd = loopEnd_.load(std::memory_order_relaxed);

    // Handle loop wrapping WITHOUT seekTo() - maintains state continuity
    if (loopValid && pos >= loopEnd) {
        if (loopEnd > loopStart) {
            pos = loopStart;  // Wrap position mathematically
            prevFrozenPage_ = nullptr;  // Reset for loop restart
        }
    }

    // Update frozen page if we've moved to a new page position
    updateFrozenPage(pos);

    if (!currentFrozenPage_) {
        outL = outR = 0.0f;
        currentPos_.store(pos, std::memory_order_relaxed);
        return false;
    }

    // Read sample from current frozen page
    if (pageFrameOffset_ >= currentFrozenPage_->validFrames) {
        outL = outR = 0.0f;
        currentPos_.store(pos, std::memory_order_relaxed);
        return false;
    }

    // Extract samples from frozen page (mono frozen output, duplicate to stereo)
    float sample = currentFrozenPage_->samples[pageFrameOffset_];
    outL = sample;
    outR = sample;

    // Advance position
    pageFrameOffset_++;
    pos++;

    currentPos_.store(pos, std::memory_order_relaxed);
    return true;
}

void AudioEngine::updateFrozenPage(uint64_t desiredPos) {
    // Calculate which page this position belongs to
    uint64_t pageSize = twOutputPage::FRAME_CAPACITY;
    uint64_t pageStartPos = (desiredPos / pageSize) * pageSize;

    // Check if current page has been invalidated (generation changed)
    if (currentFrozenPage_ && currentFrozenPage_->generation != currentPageGeneration_) {
        // Page was invalidated and repurposed while we held it; drop reference
        currentFrozenPage_ = nullptr;
        prevFrozenPage_ = nullptr;
    }

    // If we're still in the current valid page, nothing to do
    if (currentFrozenPage_ && currentFrozenPage_->startPosition == pageStartPos) {
        return;
    }

    // Tier 1 Enhancement: Freeze pages sequentially with state continuity
    // prevFrozenPage_ carries state snapshot from previous page
    currentFrozenPage_ = synthOutput_->freezePage(
        pageStartPos,
        nullptr,            // No pre-prepared input; uses latches
        0,
        0,
        engineSampleRate_,
        prevFrozenPage_     // State chain: page N resumes from page N-1
    );

    if (currentFrozenPage_) {
        currentPageStartPos_ = pageStartPos;
        currentPageGeneration_ = currentFrozenPage_->generation;  // Snapshot generation to detect later invalidation
        // Calculate offset within the new page
        pageFrameOffset_ = desiredPos - pageStartPos;
        if (pageFrameOffset_ > currentFrozenPage_->validFrames) {
            pageFrameOffset_ = currentFrozenPage_->validFrames;
        }
        prevFrozenPage_ = currentFrozenPage_;  // Save for next page's state restoration
    } else {
        pageFrameOffset_ = 0;
    }
}

void AudioEngine::seekTo(uint64_t offsetSamples) {
    // Seeking resets state (unavoidable when scrubbing to arbitrary positions)
    // For looping, setLoopBoundaries() uses frozen pages instead
    if (synthOutput_) {
        synthOutput_->seekTo(offsetSamples);
        synthOutput_->reset();  // Reset to ensure clean state for the seek
    }
    currentPos_.store(offsetSamples, std::memory_order_relaxed);
    currentFrozenPage_ = nullptr;  // Clear frozen page cache
    prevFrozenPage_ = nullptr;
    pageFrameOffset_ = 0;

    // Reconfigure resamplers for new position
    resamplerL_.reset();
    resamplerR_.reset();
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
