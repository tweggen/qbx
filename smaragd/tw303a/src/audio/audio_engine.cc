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

AudioEngine::~AudioEngine() {
    // Ensure read-ahead thread is stopped before destruction
    readaheadRunning_ = false;
    readaheadCv_.notify_all();
    if (readaheadThread_.joinable()) {
        readaheadThread_.join();
    }
}

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
            // Caller's buffers must be large enough to hold nFrames
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

        // Diagnostic: check if output is silence
        bool isSilent = true;
        for (length_t i = 0; i < outProduced && isSilent; ++i) {
            if (outL[i] != 0.0f || outR[i] != 0.0f) isSilent = false;
        }
        if (isSilent && outProduced > 0) {
            static int silenceCount = 0;
            uint64_t playPos = currentPos_.load(std::memory_order_relaxed);
            int64_t gap = (int64_t)readaheadComputedUpTo_ - (int64_t)playPos;
            if (++silenceCount == 1 || silenceCount % 100 == 0) {
                fprintf(stderr, "[SILENCE] Callback #%d producing %lld silent frames at playback=%llu, readahead=%llu, gap=%.2f sec\n",
                        silenceCount, (long long)outProduced, playPos, readaheadComputedUpTo_, (double)gap / 48000.0);
                fflush(stderr);
            }
        }

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

    // Read sample from current frozen page. If at end, try to advance to next page.
    if (pageFrameOffset_ >= currentFrozenPage_->validFrames) {
        // At end of current page; try to move to next page
        // Advance position to next page and try to load it
        pos = (currentPageStartPos_ / twOutputPage::FRAME_CAPACITY + 1) * twOutputPage::FRAME_CAPACITY;
        updateFrozenPage(pos);

        // Try again with the new page
        if (!currentFrozenPage_ || pageFrameOffset_ >= currentFrozenPage_->validFrames) {
            outL = outR = 0.0f;
            currentPos_.store(pos, std::memory_order_relaxed);
            return false;
        }
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

    // DEBUG: Track readahead gap
    static int gapLogCounter = 0;

    // Check if current page has been invalidated (generation changed)
    if (currentFrozenPage_ && currentFrozenPage_->generation != currentPageGeneration_) {
        // Page was invalidated and repurposed while we held it; drop reference
        currentFrozenPage_ = nullptr;
        prevFrozenPage_ = nullptr;
    }

    // If we're still in the current valid page, nothing to do
    if (currentFrozenPage_ && currentFrozenPage_->startPosition == pageStartPos &&
        currentFrozenPage_->validAspects != 0) {
        return;
    }

    // Lock-free cache lookup (read-only audio thread).
    // Audio thread never allocates pages, only reads existing ones.
    // Read-ahead thread allocates and freezes pages; this just reads the cache.
    auto page = synthOutput_->getPageIfExists(pageStartPos);
    if (page && page->validAspects != 0) {
        // Page is ready; switch to it
        prevFrozenPage_ = currentFrozenPage_;
        currentFrozenPage_ = page;
        currentPageStartPos_ = pageStartPos;
        currentPageGeneration_ = page->generation.load();
        pageFrameOffset_ = desiredPos - pageStartPos;
        if (pageFrameOffset_ > currentFrozenPage_->validFrames) {
            pageFrameOffset_ = currentFrozenPage_->validFrames;
        }

        // DEBUG: Page found
        if (gapLogCounter++ % 500 == 0) {
            int64_t gap = (int64_t)readaheadComputedUpTo_ - (int64_t)pageStartPos;
            fprintf(stderr, "[AUDIO] Page FOUND: playback=%llu, readahead computed=%llu, gap=%lld frames (%.2f sec at 48k)\n",
                    pageStartPos, readaheadComputedUpTo_, gap, (double)gap / 48000.0);
            fflush(stderr);
        }
    } else {
        // Page not ready; check if within underrun threshold or too far behind
        int64_t gap = (int64_t)readaheadComputedUpTo_ - (int64_t)pageStartPos;

        // Phase 6b: Underrun detection
        if (gap >= 0 && gap < (int64_t)underrunThresholdFrames_) {
            // Gap is small (< 1 sec): output silence but continue (graceful degradation)
            if (gapLogCounter++ % 50 == 0) {
                fprintf(stderr, "[AUDIO] UNDERRUN THRESHOLD: gap=%.2f sec (< 1 sec), outputting silence for recovery\n",
                        (double)gap / 48000.0);
                fflush(stderr);
            }
        } else {
            // Gap is large or negative: serious issue
            if (gapLogCounter++ % 10 == 0 || gap < 0) {
                fprintf(stderr, "[AUDIO] Page MISSING: playback wants=%llu, readahead computed=%llu, gap=%lld frames (%.2f sec at 48k) ***SILENCE***\n",
                        pageStartPos, readaheadComputedUpTo_, gap, (double)gap / 48000.0);
                fflush(stderr);
            }
        }

        currentFrozenPage_ = nullptr;
        pageFrameOffset_ = 0;
        readaheadCv_.notify_one();
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

    // Reset read-ahead state chain on seek (may skip pages)
    readaheadPrevPage_ = nullptr;
    readaheadComputedUpTo_ = 0;

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

PlaybackState AudioEngine::startPlayback() {
    // Phase 6b: Delay playback start until readahead has built minimum buffer
    uint64_t readaheadPos = readaheadComputedUpTo_;  // Plain uint64_t, no .load() needed
    uint64_t playPos = currentPos_.load(std::memory_order_relaxed);

    if (readaheadPos >= playPos + minBufferFrames_) {
        // Sufficient buffer built; transition to PLAYING
        playbackState_.store(PlaybackState::PLAYING, std::memory_order_release);
        fprintf(stderr, "[PLAYBACK] Minimum buffer reached (%.1f sec), starting playback\n",
                (double)minBufferFrames_ / 48000.0);
        fflush(stderr);
        // Signal twSpeaker that playback is ready
        playbackReadyCv_.notify_all();
        return PlaybackState::PLAYING;
    }

    // Not ready; stay in BUFFERING
    playbackState_.store(PlaybackState::BUFFERING, std::memory_order_release);
    if (readaheadPos == 0) {
        fprintf(stderr, "[PLAYBACK] Waiting for readahead to build buffer... (need %.1f sec)\n",
                (double)minBufferFrames_ / 48000.0);
        fflush(stderr);
    }
    return PlaybackState::BUFFERING;
}

PlaybackState AudioEngine::getPlaybackState() const {
    return playbackState_.load(std::memory_order_acquire);
}

void AudioEngine::startReadahead() {
    // Ensure no prior thread is running
    stopReadahead();

    // Reset state and start new thread
    readaheadRunning_ = true;
    readaheadPrevPage_ = nullptr;
    readaheadComputedUpTo_ = 0;
    readaheadThread_ = std::thread([this] { readaheadLoop(); });
}

void AudioEngine::stopReadahead() {
    if (!readaheadThread_.joinable()) return;

    readaheadRunning_ = false;
    readaheadCv_.notify_all();
    readaheadThread_.join();
}

void AudioEngine::readaheadLoop() {
    constexpr int READAHEAD_PAGES = 3;  // How far ahead to pre-compute
    const uint64_t pageSize = twOutputPage::FRAME_CAPACITY;
    static int readaheadLogCounter = 0;

    while (readaheadRunning_.load(std::memory_order_relaxed)) {
        std::unique_lock<std::mutex> lk(readaheadMutex_);
        readaheadCv_.wait_for(lk, std::chrono::milliseconds(20));
        lk.unlock();

        if (!synthOutput_ || !readaheadRunning_.load(std::memory_order_relaxed)) continue;

        uint64_t currentPos = currentPos_.load(std::memory_order_relaxed);
        uint64_t pageStart = (currentPos / pageSize) * pageSize;

        // Detect seek: playhead jumped backwards or far forward → reset state chain
        if (pageStart < readaheadComputedUpTo_ ||
            pageStart > readaheadComputedUpTo_ + READAHEAD_PAGES * pageSize) {
            if (readaheadLogCounter++ % 50 == 0) {
                fprintf(stderr, "[READAHEAD] Seek detected: pageStart=%llu, was computing=%llu, resetting state\n",
                        pageStart, readaheadComputedUpTo_);
                fflush(stderr);
            }
            readaheadPrevPage_ = nullptr;
            // DON'T update readaheadComputedUpTo_ here - let the loop naturally advance it
            // This prevents claiming we've computed pages we haven't actually frozen yet
        }

        // Phase 6b: Skip-ahead optimization
        // Compute up to READAHEAD_PAGES ahead of the current playhead
        constexpr int SKIP_DISTANCE = 5;  // Pages to skip on blocking (Phase 6b confirmed)

        for (int i = 0; i < READAHEAD_PAGES; i++) {
            uint64_t pos = pageStart + (uint64_t)i * pageSize;
            if (pos < readaheadComputedUpTo_) continue;  // Already computed

            auto existing = synthOutput_->getOrAllocatePage(pos);
            if (existing && existing->validAspects != 0) {
                // Already frozen; update prevPage and move on
                readaheadPrevPage_ = existing;
                readaheadComputedUpTo_ = pos + pageSize;
                if (readaheadLogCounter++ % 100 == 0) {
                    int64_t gap = (int64_t)readaheadComputedUpTo_ - (int64_t)currentPos;
                    fprintf(stderr, "[READAHEAD] Page already exists: computed up to=%llu, playhead=%llu, gap=%.2f sec\n",
                            readaheadComputedUpTo_, currentPos, (double)gap / 48000.0);
                    fflush(stderr);
                }
                continue;
            }

            // DEBUG: About to compute page
            auto start = std::chrono::steady_clock::now();

            // Compute this page (blocking, may take milliseconds)
            auto page = synthOutput_->freezePage(
                pos, nullptr, 0, (length_t)pageSize,
                engineSampleRate_, readaheadPrevPage_);

            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();

            if (page && page->validAspects != 0) {
                readaheadPrevPage_ = page;
                // CRITICAL: Only update readaheadComputedUpTo_ AFTER page is confirmed frozen
                // This ensures audio callback never finds validAspects == 0
                readaheadComputedUpTo_ = pos + pageSize;

                // DEBUG: Log completed page
                int64_t gap = (int64_t)readaheadComputedUpTo_ - (int64_t)currentPos;
                if (readaheadLogCounter++ % 50 == 0 || elapsed > 50) {
                    fprintf(stderr, "[READAHEAD] Froze page at=%llu, computed up to=%llu, playhead=%llu, gap=%.2f sec, time=%.1fms\n",
                            pos, readaheadComputedUpTo_, currentPos, (double)gap / 48000.0, (double)elapsed);
                    fflush(stderr);
                }
            } else {
                // Phase 6b: Sequential freeze blocked; try skip-ahead with fresh state
                if (readaheadPrevPage_ != nullptr) {
                    uint64_t skipPos = pos + (uint64_t)SKIP_DISTANCE * pageSize;
                    auto start_skip = std::chrono::steady_clock::now();

                    // Try to freeze skip-ahead page with NO prior state (fresh chain)
                    auto skipPage = synthOutput_->freezePage(
                        skipPos, nullptr, 0, (length_t)pageSize,
                        engineSampleRate_, nullptr);  // Fresh state!

                    auto elapsed_skip = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start_skip).count();

                    if (skipPage && skipPage->validAspects != 0) {
                        // Success: start new independent chain from skipPos
                        readaheadPrevPage_ = skipPage;
                        readaheadComputedUpTo_ = skipPos + pageSize;
                        fprintf(stderr, "[READAHEAD] Skip-ahead: pos %llu -> %llu (5 pages forward, fresh state), time=%.1fms\n",
                                pos, skipPos, (double)elapsed_skip);
                        fflush(stderr);
                        continue;  // Continue loop, next iteration tries skipPos+pageSize
                    }
                }

                // Neither sequential nor skip-ahead worked; give up for now
                if (readaheadLogCounter++ % 10 == 0) {
                    fprintf(stderr, "[READAHEAD] freezePage failed or not frozen at pos=%llu (validAspects=%u), giving up\n",
                            pos, page ? page->validAspects.load() : 0);
                    fflush(stderr);
                }
                break;
            }
        }
    }
}

}  // namespace audio
