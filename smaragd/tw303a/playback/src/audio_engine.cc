#include "tw/playback/audio_engine.h"

#include "tw/graph/twcomponent.h"
#include "tw/pages/tw_output_page.h"
#include "tw/graph/tw303aenv.h"
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
        // Phase 1 perf: Use pre-allocated buffers instead of malloc per block
        if (inFramesNeeded > resampleBufL_.size()) {
            resampleBufL_.resize(inFramesNeeded);
            resampleBufR_.resize(inFramesNeeded);
        }

        // Track current position BEFORE consuming input frames
        // This allows us to advance by the actual output frame count, not the input count
        uint64_t posBeforeResample = currentPos_.load(std::memory_order_relaxed);

        // Phase 3 Optimization: Batch input pulling for resampling path (same as passthrough)
        // Instead of per-sample pullStereoFrameFrozen calls, use batching with cached state
        length_t inProduced = 0;
        length_t inOffset = 0;

        while (inProduced < inFramesNeeded) {
            // Step 1: Cache atomic state ONCE per batch
            uint64_t pos = currentPos_.load(std::memory_order_relaxed);
            const bool loopValid = cycleEnabled_.load(std::memory_order_relaxed);
            const uint64_t loopStart = loopStart_.load(std::memory_order_relaxed);
            const uint64_t loopEnd = loopEnd_.load(std::memory_order_relaxed);
            const uint64_t readaheadPos = readaheadComputedUpTo_;

            // Handle loop wrapping
            if (loopValid && pos >= loopEnd) {
                if (loopEnd > loopStart) {
                    pos = loopStart;
                    prevFrozenPage_ = nullptr;
                }
            }

            // Step 2: Load page if needed
            uint64_t pageSize = twOutputPage::FRAME_CAPACITY;
            uint64_t pageStartPos = (pos / pageSize) * pageSize;

            if (!currentFrozenPage_ || currentFrozenPage_->startPosition != pageStartPos ||
                currentFrozenPage_->validAspects == 0) {
                updateFrozenPage(pos);
            }

            // If page not available, stop pulling
            if (!currentFrozenPage_ || currentFrozenPage_->validAspects == 0) {
                readaheadCv_.notify_one();
                break;
            }

            // Step 3: Compute safe batch size for input pulling
            length_t batchSize = inFramesNeeded - inProduced;

            // Limit to current page capacity
            uint64_t framesInPage = cachedPageValidFrames_ - pageFrameOffset_;
            batchSize = std::min(batchSize, (length_t)framesInPage);

            // Limit to loop boundary
            if (loopValid && pos + batchSize > loopEnd) {
                batchSize = (length_t)(loopEnd - pos);
            }

            // Limit to readahead buffer
            if (readaheadPos > pos) {
                int64_t readaheadGap = (int64_t)readaheadPos - (int64_t)pos;
                batchSize = std::min(batchSize, (length_t)readaheadGap);
            } else {
                break;  // Underrun
            }

            // Step 4: Fast path - copy batch from frozen page into resampling buffers
            if (batchSize > 0) {
                const float *pageData = &currentFrozenPage_->samples[pageFrameOffset_];
                std::copy(pageData, pageData + batchSize, resampleBufL_.data() + inOffset);
                std::copy(pageData, pageData + batchSize, resampleBufR_.data() + inOffset);

                // Step 5: Update state ONCE per batch
                pageFrameOffset_ += batchSize;
                pos += batchSize;
                inOffset += batchSize;
                inProduced += batchSize;
                currentPos_.store(pos, std::memory_order_relaxed);
            } else {
                break;
            }
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
                outL[i] = resampleBufL_[k] + (resampleBufL_[k+1] - resampleBufL_[k]) * (float)frac;
                outR[i] = resampleBufR_[k] + (resampleBufR_[k+1] - resampleBufR_[k]) * (float)frac;
            } else if (k < inProduced) {
                outL[i] = resampleBufL_[k];
                outR[i] = resampleBufR_[k];
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

    // Phase 2 Optimization: Batching architecture for passthrough (no per-sample atomic loads)
    // Cache atomic state once, compute safe batch size, use tight operations for batch
    length_t produced = 0;
    length_t outOffset = 0;

    while (produced < nFrames) {
        // Step 1: Cache atomic state ONCE per batch
        uint64_t pos = currentPos_.load(std::memory_order_relaxed);
        const bool loopValid = cycleEnabled_.load(std::memory_order_relaxed);
        const uint64_t loopStart = loopStart_.load(std::memory_order_relaxed);
        const uint64_t loopEnd = loopEnd_.load(std::memory_order_relaxed);
        const uint64_t readaheadPos = readaheadComputedUpTo_;

        // Handle loop wrapping (before page load)
        if (loopValid && pos >= loopEnd) {
            if (loopEnd > loopStart) {
                pos = loopStart;
                prevFrozenPage_ = nullptr;  // Reset state chain for loop restart
            }
        }

        // Step 2: Load page if needed (only when entering new page, not per-sample)
        uint64_t pageSize = twOutputPage::FRAME_CAPACITY;
        uint64_t pageStartPos = (pos / pageSize) * pageSize;

        // Check if we need to load a new page
        if (!currentFrozenPage_ || currentFrozenPage_->startPosition != pageStartPos ||
            currentFrozenPage_->validAspects == 0) {
            updateFrozenPage(pos);
        }

        // If page not available, output silence and stop
        if (!currentFrozenPage_ || currentFrozenPage_->validAspects == 0) {
            std::memset(outL + outOffset, 0, (nFrames - produced) * sizeof(float));
            std::memset(outR + outOffset, 0, (nFrames - produced) * sizeof(float));
            readaheadCv_.notify_one();  // Wake readahead to fetch page
            return produced;
        }

        // Step 3: Compute safe batch size (frames until page end, loop boundary, etc.)
        length_t batchSize = nFrames - produced;

        // Limit to current page capacity (use cached validFrames for performance)
        uint64_t framesInPage = cachedPageValidFrames_ - pageFrameOffset_;
        batchSize = std::min(batchSize, (length_t)framesInPage);

        // Limit to loop boundary (if looping and approaching loop end)
        if (loopValid && pos + batchSize > loopEnd) {
            batchSize = (length_t)(loopEnd - pos);
        }

        // Limit to readahead buffer (underrun protection)
        if (readaheadPos > pos) {
            int64_t readaheadGap = (int64_t)readaheadPos - (int64_t)pos;
            batchSize = std::min(batchSize, (length_t)readaheadGap);
        } else {
            // Serious underrun: output silence
            std::memset(outL + outOffset, 0, (nFrames - produced) * sizeof(float));
            std::memset(outR + outOffset, 0, (nFrames - produced) * sizeof(float));
            return produced;
        }

        // Step 4: Fast path - copy batch from frozen page
        if (batchSize > 0) {
            const float *pageData = &currentFrozenPage_->samples[pageFrameOffset_];
            // Duplicate mono frozen output to stereo
            std::copy(pageData, pageData + batchSize, outL + outOffset);
            std::copy(pageData, pageData + batchSize, outR + outOffset);

            // Step 5: Update state ONCE per batch (not per-sample)
            pageFrameOffset_ += batchSize;
            pos += batchSize;
            outOffset += batchSize;
            produced += batchSize;

            // Store position once per batch
            currentPos_.store(pos, std::memory_order_relaxed);
        } else {
            // Safety: batchSize is 0, output remainder and exit
            std::memset(outL + outOffset, 0, (nFrames - produced) * sizeof(float));
            std::memset(outR + outOffset, 0, (nFrames - produced) * sizeof(float));
            return produced;
        }
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
        // Phase 2 perf: Cache validFrames to avoid repeated loads in batching loop
        cachedPageValidFrames_ = currentFrozenPage_->validFrames;
        if (pageFrameOffset_ > cachedPageValidFrames_) {
            pageFrameOffset_ = cachedPageValidFrames_;
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

    // Phase 1 perf: Pre-allocate resampling buffers once (avoid malloc per block)
    // Max input frames when downsampling: ceil(maxOutputFrames * invRatio)
    // Assuming max output block is 4096 frames, and worst-case ratio is ~1.1x
    constexpr length_t maxOutputFrames = 4096;
    double invRatio = (rateRatio_ > 0.0) ? (1.0 / rateRatio_) : 1.0;
    length_t maxInputFrames = (length_t)std::ceil(maxOutputFrames * invRatio) + 16;  // +16 for safety
    resampleBufL_.resize(maxInputFrames);
    resampleBufR_.resize(maxInputFrames);
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
    uint64_t lastPlayheadPage = UINT64_MAX;  // playhead page seen last iteration

    while (readaheadRunning_.load(std::memory_order_relaxed)) {
        std::unique_lock<std::mutex> lk(readaheadMutex_);
        readaheadCv_.wait_for(lk, std::chrono::milliseconds(20));
        lk.unlock();

        if (!synthOutput_ || !readaheadRunning_.load(std::memory_order_relaxed)) continue;

        uint64_t currentPos = currentPos_.load(std::memory_order_relaxed);
        uint64_t pageStart = (currentPos / pageSize) * pageSize;

        // Detect a real playhead jump (transport seek or loop wrap): the playhead
        // moved backwards, or past everything we've frozen. During normal playback
        // pageStart advances monotonically and stays at or behind the frontier, so
        // this never fires. (Comparing pageStart against the frontier itself fires
        // on every iteration — the readahead is *supposed* to run ahead.)
        if (lastPlayheadPage != UINT64_MAX &&
            (pageStart < lastPlayheadPage || pageStart > readaheadComputedUpTo_)) {
            fprintf(stderr, "[READAHEAD] Playhead jump: page %llu -> %llu (frontier=%llu), restarting chain\n",
                    (unsigned long long)lastPlayheadPage, (unsigned long long)pageStart,
                    (unsigned long long)readaheadComputedUpTo_);
            fflush(stderr);
            readaheadPrevPage_ = nullptr;
            // The chain restarts at the playhead; the first page frozen from here
            // is a discontinuity, which freezePage() answers with reset()+seekTo().
            readaheadComputedUpTo_ = pageStart;
        }
        lastPlayheadPage = pageStart;

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
            auto pageStart = std::chrono::high_resolution_clock::now();

            // Compute this page (blocking, may take milliseconds)
            auto page = synthOutput_->freezePage(
                pos, nullptr, 0, (length_t)pageSize,
                engineSampleRate_, readaheadPrevPage_);

            auto pageEnd = std::chrono::high_resolution_clock::now();
            auto duration_ms = std::chrono::duration<double, std::milli>(pageEnd -
                pageStart).count();

            fprintf(stderr, "[READAHEAD] Generated page [%llu, %llu) in %.2f ms (%.1f%% of page duration)\n",
                (unsigned long long) pos,
                (unsigned long long) (pos + pageSize),
                duration_ms,
                (duration_ms / 1370.0) * 100  // 1370ms = 65536 frames at 48kHz
            );

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
