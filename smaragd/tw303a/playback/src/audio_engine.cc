#include "tw/playback/audio_engine.h"

#include "tw/graph/twcomponent.h"
#include "tw/pages/tw_output_page.h"
#include "tw/graph/tw303aenv.h"
#include "tw/core/twlog.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace audio {

AudioEngine::AudioEngine(std::shared_ptr<twComponent> synthOutput, uint32_t sampleRate)
    : synthOutput_(synthOutput),
      engineSampleRate_(sampleRate),
      currentPageStartPos_(0),
      pageFrameOffset_(0),
      rateRatio_(1.0)
{
    // Resamplers start in identity config (will be updated via configureResampling).
    // One per channel of whatever the graph declares; configureResampling() is
    // what re-sizes the vector if the project's width changed.
    resamplers_.resize(std::max<std::size_t>(1, graphChannels()));
    for (auto &r : resamplers_) {
        r.configure(sampleRate, sampleRate);
        r.reset();
    }
    passthrough_ = true;
}

std::size_t AudioEngine::graphChannels() const {
    if (!synthOutput_) return 1;
    const idx_t n = synthOutput_->getOutputChannels();
    return (n > 0) ? (std::size_t) n : (std::size_t) 1;
}

void AudioEngine::resetResamplers() {
    for (auto &r : resamplers_) r.reset();
}

void AudioEngine::ensureResampleBuffers(std::size_t channels, std::size_t frames) {
    if (channels == 0) channels = 1;
    if (resampleChannels_ >= channels && resampleStride_ >= frames) return;
    resampleChannels_ = std::max(resampleChannels_, channels);
    resampleStride_   = std::max(resampleStride_, frames);
    resampleBuf_.assign(resampleChannels_ * resampleStride_, 0.0f);
}

namespace {

// Every miss path in pullBlock() must leave EVERY destination buffer defined —
// a caller (the RT callback, a test) hands us scratch it has not cleared.
inline void zeroChannels(float* const* out, std::size_t nChannels,
                         std::size_t offset, std::size_t count) {
    for (std::size_t c = 0; c < nChannels; ++c) {
        if (out[c]) std::memset(out[c] + offset, 0, count * sizeof(float));
    }
}

}  // namespace

AudioEngine::~AudioEngine() {
    // Ensure read-ahead thread is stopped before destruction
    readaheadRunning_ = false;
    readaheadCv_.notify_all();
    if (readaheadThread_.joinable()) {
        readaheadThread_.join();
    }
}

// Pull nFrames into nChannels planar destination buffers.
//
// WIDTH (proposal 36 B5). This used to be pullBlock(outL, outR, n) — two named
// buffers, both fed from channelPtr(0), which is where playback's half of the
// mono sink lived. It now serves destination channel c from the frozen page's
// channel twPageClampChannel(page, c): the §4.4 read clamp, so a narrower page
// (a mono project, or a stale narrow page under proposal 16's fallback) plays
// on every destination channel exactly as before, and a wider one is read
// honestly. Nothing here decides how the destination channels map onto a DEVICE
// — that is twSpeaker's rule, next to the sample-rate mismatch it already owns.
length_t AudioEngine::pullBlock(float* const* outChannels, std::size_t nChannels,
                                length_t nFrames) {
    if (!outChannels || nChannels == 0) return 0;

    if (!synthOutput_ || nFrames == 0) {
        zeroChannels(outChannels, nChannels, 0, (std::size_t) nFrames);
        return 0;
    }

    // Adopt a pending live seek (requestSeek). We are the RT pull thread — the
    // sole writer of currentPos_ and the frozen-page cursor — so moving them
    // here races nothing. The readahead thread sees the currentPos_ jump on its
    // next tick and restarts its chain from the new position; until those pages
    // are frozen the pulls below simply underrun to silence for a moment.
    if (seekPending_.exchange(false, std::memory_order_acquire)) {
        uint64_t sp = seekTarget_.load(std::memory_order_relaxed);
        currentPos_.store(sp, std::memory_order_relaxed);
        currentFrozenPage_ = nullptr;
        prevFrozenPage_ = nullptr;
        pageFrameOffset_ = 0;
        cachedPageValidFrames_ = 0;
        resetResamplers();
    }

    // Tier 1 Enhancement: Use frozen pages for state-continuous rendering
    // If resampling needed (engine rate != output rate), pull more frames and resample
    if (!passthrough_) {
        // Pull at engine rate, then resample to output rate
        // rateRatio_ = outputRate / inputRate (e.g., 44100/48000 = 0.9187)
        // We need more input frames when downsampling: inFrames = outFrames / rateRatio_
        double invRatio = 1.0 / rateRatio_;  // inputRate / outputRate (e.g., 48000/44100 = 1.0884)
        length_t inFramesNeeded = (length_t)std::ceil(nFrames * invRatio);
        // Phase 1 perf: Use pre-allocated buffers instead of malloc per block.
        // One flat allocation of nChannels * stride, so a width change costs one
        // resize rather than N (and, in the steady state, none).
        ensureResampleBuffers(nChannels, (std::size_t) inFramesNeeded);
        const std::size_t stride = resampleStride_;

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

            // Step 2: Validate/load page (always: updateFrozenPage also detects
            // pages outdated by an edit mid-playback, and early-returns cheaply
            // when the held page is still current)
            updateFrozenPage(pos);

            // If page not available, stop pulling
            if (!currentFrozenPage_ || currentFrozenPage_->validAspects == 0) {
                readaheadCv_.notify_one();
                break;
            }

            // Step 3: Compute safe batch size for input pulling
            length_t batchSize = inFramesNeeded - inProduced;

            // Limit to the page's fixed timeline range. A page may hold fewer
            // rendered frames than its range covers (content ends mid-page);
            // the tail counts as silence, not as a wall, so playback can cross.
            uint64_t framesInPage = twOutputPage::FRAME_CAPACITY - pageFrameOffset_;
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
                // Only [0, validFrames) holds rendered samples; the rest of the
                // page's range is silence (buffer tail may be stale pool data).
                length_t dataFrames = (pageFrameOffset_ < cachedPageValidFrames_)
                    ? std::min(batchSize, (length_t)(cachedPageValidFrames_ - pageFrameOffset_))
                    : 0;
                // §4.4: the width we may act on is the width of the page IN HAND,
                // never the producer's declared width — this path is exactly the
                // one on which proposal 16 hands the RT callback a stale page.
                for (std::size_t c = 0; c < nChannels; ++c) {
                    const idx_t src = twPageClampChannel(*currentFrozenPage_, (idx_t) c);
                    const float *pageData = currentFrozenPage_->channelPtr(src) + pageFrameOffset_;
                    float *dst = resampleBuf_.data() + c * stride + inOffset;
                    std::copy(pageData, pageData + dataFrames, dst);
                    if (dataFrames < batchSize) {
                        std::fill(dst + dataFrames, dst + batchSize, 0.0f);
                    }
                }

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
            // DEBUG: why did the batch loop break without producing anything?
            static int stallLogCounter = 0;
            if (stallLogCounter++ % 200 == 0) {
                uint64_t pos = currentPos_.load(std::memory_order_relaxed);
                TW_LOGD( "playback", "[STALL] pullBlock(resample) produced 0: pos=%llu page=%s pageStart=%llu offs=%zu cachedValid=%u pageValid=%u pageEpoch=%llu epochNow=%llu readahead=%llu cycle=%d loopEnd=%llu",
                        (unsigned long long)pos,
                        currentFrozenPage_ ? "held" : "NULL",
                        currentFrozenPage_ ? (unsigned long long)currentFrozenPage_->startPosition : 0ULL,
                        pageFrameOffset_,
                        cachedPageValidFrames_,
                        currentFrozenPage_ ? currentFrozenPage_->validFrames : 0u,
                        currentFrozenPage_ ? (unsigned long long)currentFrozenPage_->contentEpoch.load() : 0ULL,
                        synthOutput_ ? (unsigned long long)synthOutput_->contentEpochNow() : 0ULL,
                        (unsigned long long)readaheadComputedUpTo_,
                        (int)cycleEnabled_.load(std::memory_order_relaxed),
                        (unsigned long long)loopEnd_.load(std::memory_order_relaxed) );
            }
            // Caller's buffers must be large enough to hold nFrames
            zeroChannels(outChannels, nChannels, 0, (std::size_t) nFrames);
            return 0;
        }

        // Linear interpolation resampling: advance through input at invRatio rate
        // For output sample i, read from input at position i * invRatio
        double step = invRatio;  // inputRate / outputRate
        length_t outProduced = 0;
        for (std::size_t c = 0; c < nChannels; ++c) {
            const float *in = resampleBuf_.data() + c * stride;
            float *out = outChannels[c];
            if (!out) continue;
            for (length_t i = 0; i < nFrames; ++i) {
                double p = (double)i * step;
                length_t k = (length_t)p;
                double frac = p - (double)k;

                if (k + 1 < inProduced) {
                    out[i] = in[k] + (in[k+1] - in[k]) * (float)frac;
                } else if (k < inProduced) {
                    out[i] = in[k];
                } else {
                    out[i] = 0.0f;
                }
            }
        }
        outProduced = nFrames;

        // currentPos_ was already advanced by exactly the input frames consumed
        // (Step 5 stores per batch). It must NOT be rewritten from outProduced:
        // pageFrameOffset_ advanced by inProduced in lockstep with the position,
        // and rounding the position back from the output-frame count makes the
        // page offset drift ahead of the playhead by up to a frame per block —
        // until the offset exhausts a page while the playhead is still inside
        // it, which pins playback permanently. The sub-frame phase loss per
        // block is inherent to this per-block linear resampler.

        // Diagnostic: check if output is silence
        bool isSilent = true;
        for (std::size_t c = 0; c < nChannels && isSilent; ++c) {
            const float *out = outChannels[c];
            if (!out) continue;
            for (length_t i = 0; i < outProduced && isSilent; ++i) {
                if (out[i] != 0.0f) isSilent = false;
            }
        }
        if (isSilent && outProduced > 0) {
            static int silenceCount = 0;
            uint64_t playPos = currentPos_.load(std::memory_order_relaxed);
            int64_t gap = (int64_t)readaheadComputedUpTo_ - (int64_t)playPos;
            if (++silenceCount == 1 || silenceCount % 100 == 0) {
                TW_LOGD( "playback", "[SILENCE] Callback #%d producing %lld silent frames at playback=%llu, readahead=%llu, gap=%.2f sec",
                        silenceCount, (long long)outProduced, playPos, readaheadComputedUpTo_, (double)gap / 48000.0 );
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

        // Step 2: Validate/load page (always: updateFrozenPage also detects
        // pages outdated by an edit mid-playback, and early-returns cheaply
        // when the held page is still current)
        updateFrozenPage(pos);

        // If page not available, output silence and stop
        if (!currentFrozenPage_ || currentFrozenPage_->validAspects == 0) {
            zeroChannels(outChannels, nChannels, (std::size_t) outOffset,
                         (std::size_t) (nFrames - produced));
            readaheadCv_.notify_one();  // Wake readahead to fetch page
            return produced;
        }

        // Step 3: Compute safe batch size (frames until page end, loop boundary, etc.)
        length_t batchSize = nFrames - produced;

        // Limit to the page's fixed timeline range. A page may hold fewer
        // rendered frames than its range covers (content ends mid-page);
        // the tail counts as silence, not as a wall, so playback can cross.
        uint64_t framesInPage = twOutputPage::FRAME_CAPACITY - pageFrameOffset_;
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
            zeroChannels(outChannels, nChannels, (std::size_t) outOffset,
                         (std::size_t) (nFrames - produced));
            return produced;
        }

        // Step 4: Fast path - copy batch from frozen page
        if (batchSize > 0) {
            // Only [0, validFrames) holds rendered samples; the rest of the
            // page's range is silence (buffer tail may be stale pool data).
            length_t dataFrames = (pageFrameOffset_ < cachedPageValidFrames_)
                ? std::min(batchSize, (length_t)(cachedPageValidFrames_ - pageFrameOffset_))
                : 0;
            for (std::size_t c = 0; c < nChannels; ++c) {
                float *out = outChannels[c];
                if (!out) continue;
                const idx_t src = twPageClampChannel(*currentFrozenPage_, (idx_t) c);
                const float *pageData = currentFrozenPage_->channelPtr(src) + pageFrameOffset_;
                std::copy(pageData, pageData + dataFrames, out + outOffset);
                if (dataFrames < batchSize) {
                    std::memset(out + outOffset + dataFrames, 0,
                                (batchSize - dataFrames) * sizeof(float));
                }
            }

            // Step 5: Update state ONCE per batch (not per-sample)
            pageFrameOffset_ += batchSize;
            pos += batchSize;
            outOffset += batchSize;
            produced += batchSize;

            // Store position once per batch
            currentPos_.store(pos, std::memory_order_relaxed);
        } else {
            // Safety: batchSize is 0, output remainder and exit
            zeroChannels(outChannels, nChannels, (std::size_t) outOffset,
                         (std::size_t) (nFrames - produced));
            return produced;
        }
    }

    return produced;
}

void AudioEngine::updateFrozenPage(uint64_t desiredPos) {
    // Calculate which page this position belongs to
    uint64_t pageSize = twOutputPage::FRAME_CAPACITY;
    offset_t pageStartPos = twFloorAlign( desiredPos, (offset_t) pageSize );

    // DEBUG: Track readahead gap
    static int gapLogCounter = 0;

    // Current content epoch: pages rendered before the last edit are stale
    // even though their validAspects are still set (lock-free atomic read).
    const uint64_t epochNow = synthOutput_->contentEpochNow();

    // A generation change means the page object was invalidated/repurposed
    // (invalidateAllPages) — its buffer cannot be trusted at all; drop hard.
    if (currentFrozenPage_ &&
        currentFrozenPage_->generation != currentPageGeneration_) {
        currentFrozenPage_ = nullptr;
        prevFrozenPage_ = nullptr;
    }

    // WIDTH MISMATCH IS A MISS (proposal 36 §4.5). Every acceptance below —
    // the fast path, the stale-held fallback, the stalePredecessor fallback —
    // is gated on this, because the stale fallbacks are the ONE path on which a
    // page frozen before a project width change can reach the RT callback. A
    // page of the wrong width is not degraded audio, it is a different
    // geometry; serving it would be reading the wrong bytes on the audio
    // thread. Dropping the held page here also covers the heldStillCovers fast
    // path below without a second test.
    const idx_t widthNow = synthOutput_->getOutputChannels();
    if (currentFrozenPage_ && !twPageWidthUsable(currentFrozenPage_.get(), widthNow)) {
        currentFrozenPage_ = nullptr;
        prevFrozenPage_ = nullptr;
    }

    // A held page from an older content epoch is still a consistent waveform
    // (proposal 16): it stays playable as a FALLBACK, but no longer satisfies
    // the fast path — every batch re-checks the cache for its re-frozen
    // replacement so the edit becomes audible as soon as it lands.
    const bool heldIsStale = currentFrozenPage_ &&
        currentFrozenPage_->contentEpoch.load() < epochNow;

    // If we're still in the current valid page, there is no page to CHOOSE —
    // but the cursor still has to be re-derived below, because desiredPos can
    // move inside the held page without the batch loop having walked it there
    // (a cycle wrap back into this same page is exactly that case). Falling
    // through to the tail costs two integer ops; the cache lookup stays behind
    // this branch so the steady state never touches the page map.
    const bool heldStillCovers = currentFrozenPage_ && !heldIsStale &&
        currentFrozenPage_->startPosition == pageStartPos &&
        currentFrozenPage_->validAspects != 0;

    if (!heldStillCovers) {
        // Lock-free cache lookup (read-only audio thread).
        // Audio thread never allocates pages, only reads existing ones.
        // Read-ahead thread allocates and freezes pages; this just reads the cache.
        auto page = synthOutput_->getPageIfExists(pageStartPos);
        if (page && page->validAspects != 0 &&
            page->contentEpoch.load() >= epochNow &&
            twPageWidthUsable(page.get(), widthNow) &&
            // Trust the page's OWN startPosition, never the map key it was
            // found under (cf. twPluginInsert's upstream page read).
            page->startPosition == pageStartPos) {
            // Page is ready; switch to it
            prevFrozenPage_ = currentFrozenPage_;
            currentFrozenPage_ = page;

            // DEBUG: Page found
            if (gapLogCounter++ % 500 == 0) {
                int64_t gap = (int64_t)readaheadComputedUpTo_ - (int64_t)pageStartPos;
                TW_LOGD( "playback", "[AUDIO] Page FOUND: playback=%llu, readahead computed=%llu, gap=%lld frames (%.2f sec at 48k)",
                        pageStartPos, readaheadComputedUpTo_, gap, (double)gap / 48000.0 );
            }
        } else if (currentFrozenPage_ &&
                   currentFrozenPage_->startPosition == pageStartPos &&
                   currentFrozenPage_->validAspects != 0) {
            // No current-epoch page yet, but the (stale) held page still covers
            // this position: keep playing pre-edit audio rather than going silent
            // while the readahead re-freezes the page (proposal 16).
            readaheadCv_.notify_one();
            if (gapLogCounter++ % 500 == 0) {
                TW_LOGW( "playback", "[AUDIO] Serving STALE page at %llu as fallback (epoch %llu < %llu), awaiting re-freeze",
                        (unsigned long long)pageStartPos,
                        (unsigned long long)currentFrozenPage_->contentEpoch.load(),
                        (unsigned long long)epochNow );
            }
        } else if (auto fallbackPage = [&]() -> std::shared_ptr<twOutputPage> {
                       // Stale-but-frozen fallback from the cache: either the
                       // pre-edit page still sitting in the map, or — if the map
                       // entry is already a mid-render placeholder — the pre-edit
                       // page it replaced (kept reachable as stalePredecessor).
                       if (page && page->validAspects != 0) return page;
                       if (page) return std::atomic_load(&page->stalePredecessor);
                       return nullptr;
                   }();
                   fallbackPage && fallbackPage->validAspects != 0 &&
                   twPageWidthUsable(fallbackPage.get(), widthNow) &&
                   fallbackPage->startPosition == pageStartPos) {
            // Crossed into a page whose re-freeze is pending or in flight; adopt
            // the pre-edit page as fallback (proposal 16). The fast path stays
            // unsatisfied (stale epoch), so adoption of the fresh page is retried
            // every batch.
            prevFrozenPage_ = currentFrozenPage_;
            currentFrozenPage_ = fallbackPage;
            readaheadCv_.notify_one();
            if (gapLogCounter++ % 500 == 0) {
                TW_LOGW( "playback", "[AUDIO] Adopted STALE fallback page at %llu, awaiting re-freeze",
                        (unsigned long long)pageStartPos );
            }
        } else {
            // Page not ready; check if within underrun threshold or too far behind
            int64_t gap = (int64_t)readaheadComputedUpTo_ - (int64_t)pageStartPos;

            // Phase 6b: Underrun detection
            if (gap >= 0 && gap < (int64_t)underrunThresholdFrames_) {
                // Gap is small (< 1 sec): output silence but continue (graceful degradation)
                if (gapLogCounter++ % 50 == 0) {
                    TW_LOGW( "playback", "[AUDIO] UNDERRUN THRESHOLD: gap=%.2f sec (< 1 sec), outputting silence for recovery",
                            (double)gap / 48000.0 );
                }
            } else {
                // Gap is large or negative: serious issue
                if (gapLogCounter++ % 10 == 0 || gap < 0) {
                    TW_LOGW( "playback", "[AUDIO] Page MISSING: playback wants=%llu, readahead computed=%llu, gap=%lld frames (%.2f sec at 48k) ***SILENCE***",
                            pageStartPos, readaheadComputedUpTo_, gap, (double)gap / 48000.0 );
                }
            }

            currentFrozenPage_ = nullptr;
            pageFrameOffset_ = 0;
            readaheadCv_.notify_one();
        }
    }

    // ONE tail, reached by every branch: the cursor is DERIVED from the page we
    // ended up holding and the position we were asked for. It is never carried
    // over from a previous call — the batch loops advance it in lockstep with
    // the playhead, but a cycle wrap (pullBlock) rewrites the playhead without
    // touching the offset, so anything that trusted the carried value read the
    // page at the PRE-WRAP offset while the playhead reported loopStart.
    // Deriving it here is what makes "what you hear" == "where the playhead is"
    // on every path, including the two that used to return early.
    if (currentFrozenPage_) {
        currentPageStartPos_ = (uint64_t)currentFrozenPage_->startPosition;
        pageFrameOffset_ = (size_t)(desiredPos - currentPageStartPos_);
        // Phase 2 perf: Cache validFrames to avoid repeated loads in batching loop.
        // pageFrameOffset_ may exceed validFrames: that region of the page's
        // range is silence, and the pull paths handle it without reading samples.
        cachedPageValidFrames_ = currentFrozenPage_->validFrames;
        currentPageGeneration_ = currentFrozenPage_->generation.load();
    }
}

void AudioEngine::seekTo(uint64_t offsetSamples) {
    // The graph is deliberately NOT touched here. A seek moves the ENGINE's
    // read position; it does not move the graph's cursors.
    //
    // This used to do synthOutput_->seekTo(offsetSamples) + ->reset(). Both
    // take only the component's mutex(), while a page freeze serializes on its
    // cursorMutex_ — so a seek arriving between an in-flight freeze's
    // seekTo(page->startPosition) and its renderFrames() rewrote the cursor
    // that render was about to use, and the whole 65536-frame page came out as
    // the audio of the SEEK TARGET while being cached under, and served for,
    // the original startPos. That is the wrong-position playback symptom.
    //
    // Nothing is lost: the first page after a seek is a chain discontinuity
    // (readaheadPrevPage_ is cleared just below), and freezePage_nolock answers
    // a discontinuity with reset() + seekTo(startPos) ITSELF — scoped to the
    // one freeze that needs it, under that component's cursorMutex_.
    currentPos_.store(offsetSamples, std::memory_order_relaxed);
    currentFrozenPage_ = nullptr;  // Clear frozen page cache
    prevFrozenPage_ = nullptr;
    pageFrameOffset_ = 0;

    // Reset read-ahead state chain on seek (may skip pages)
    readaheadPrevPage_ = nullptr;
    readaheadComputedUpTo_ = 0;

    // Reconfigure resamplers for new position
    resetResamplers();
}

void AudioEngine::requestSeek(uint64_t offsetSamples) {
    // Post the target; the RT pull thread adopts it at the top of pullBlock().
    // Ordered so the RT's acquire-exchange of seekPending_ sees seekTarget_.
    seekTarget_.store(offsetSamples, std::memory_order_relaxed);
    seekPending_.store(true, std::memory_order_release);
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
    // ONE RESAMPLER PER CHANNEL (proposal 36 B5). twResampler is documented as
    // a converter "for a single mono channel" and carries interpolation phase
    // plus an input history buffer, so a shared instance across channels would
    // be a cross-channel smear the day this path stops doing its interpolation
    // inline. The vector is sized here, on the setup path, and never on the RT
    // path — pullBlock only reads it.
    //
    // Honest note for whoever profiles this: the per-block linear interpolation
    // in pullBlock is STATELESS (it recomputes the phase from the block index),
    // so today these objects supply only the rate pair and the passthrough
    // decision. They are the right place for the state the moment the
    // interpolation moves into twResampler::process(), and one object per
    // channel costs a few hundred bytes.
    const std::size_t width = std::max<std::size_t>(1, graphChannels());
    if (resamplers_.size() != width) resamplers_.resize(width);
    for (auto &r : resamplers_) {
        r.configure(inRate, outRate);
        r.reset();
    }
    passthrough_ = resamplers_.empty() ? true : resamplers_[0].isPassthrough();
    rateRatio_ = (inRate > 0) ? ((double)outRate / (double)inRate) : 1.0;

    // Phase 1 perf: Pre-allocate resampling buffers once (avoid malloc per block)
    // Max input frames when downsampling: ceil(maxOutputFrames * invRatio)
    // Assuming max output block is 4096 frames, and worst-case ratio is ~1.1x
    constexpr length_t maxOutputFrames = 4096;
    double invRatio = (rateRatio_ > 0.0) ? (1.0 / rateRatio_) : 1.0;
    length_t maxInputFrames = (length_t)std::ceil(maxOutputFrames * invRatio) + 16;  // +16 for safety
    resampleChannels_ = 0;
    resampleStride_   = 0;
    ensureResampleBuffers(width, (std::size_t) maxInputFrames);
}

PlaybackState AudioEngine::startPlayback() {
    // Phase 6b: Delay playback start until readahead has built minimum buffer
    uint64_t readaheadPos = readaheadComputedUpTo_;  // Plain uint64_t, no .load() needed
    uint64_t playPos = currentPos_.load(std::memory_order_relaxed);

    if (readaheadPos >= playPos + minBufferFrames_) {
        // Sufficient buffer built; transition to PLAYING
        playbackState_.store(PlaybackState::PLAYING, std::memory_order_release);
        TW_LOGI( "playback", "[PLAYBACK] Minimum buffer reached (%.1f sec), starting playback",
                (double)minBufferFrames_ / 48000.0 );
        // Signal twSpeaker that playback is ready
        playbackReadyCv_.notify_all();
        return PlaybackState::PLAYING;
    }

    // Not ready; stay in BUFFERING
    playbackState_.store(PlaybackState::BUFFERING, std::memory_order_release);
    if (readaheadPos == 0) {
        TW_LOGD( "playback", "[PLAYBACK] Waiting for readahead to build buffer... (need %.1f sec)",
                (double)minBufferFrames_ / 48000.0 );
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
    constexpr int READAHEAD_PAGES = 3;  // Minimum lookahead; actual count grows to cover minBufferFrames_
    const uint64_t pageSize = twOutputPage::FRAME_CAPACITY;
    static int readaheadLogCounter = 0;
    uint64_t lastPlayheadPage = UINT64_MAX;  // playhead page seen last iteration

    while (readaheadRunning_.load(std::memory_order_relaxed)) {
        std::unique_lock<std::mutex> lk(readaheadMutex_);
        readaheadCv_.wait_for(lk, std::chrono::milliseconds(20));
        lk.unlock();

        if (!synthOutput_ || !readaheadRunning_.load(std::memory_order_relaxed)) continue;

        uint64_t currentPos = currentPos_.load(std::memory_order_relaxed);
        offset_t pageStart = twFloorAlign( (offset_t) currentPos, (offset_t) pageSize );

        // Detect a real playhead jump (transport seek or loop wrap): the playhead
        // moved backwards, or past everything we've frozen. During normal playback
        // pageStart advances monotonically and stays at or behind the frontier, so
        // this never fires. (Comparing pageStart against the frontier itself fires
        // on every iteration — the readahead is *supposed* to run ahead.)
        if (lastPlayheadPage != UINT64_MAX &&
            (pageStart < lastPlayheadPage || pageStart > readaheadComputedUpTo_)) {
            TW_LOGI( "playback", "[READAHEAD] Playhead jump: page %llu -> %llu (frontier=%llu), restarting chain",
                    (unsigned long long)lastPlayheadPage, (unsigned long long)pageStart,
                    (unsigned long long)readaheadComputedUpTo_ );
            readaheadPrevPage_ = nullptr;
            // The chain restarts at the playhead; the first page frozen from here
            // is a discontinuity, which freezePage() answers with reset()+seekTo().
            readaheadComputedUpTo_ = pageStart;
        }
        lastPlayheadPage = pageStart;

        // Phase 6b: Skip-ahead optimization
        // Compute up to READAHEAD_PAGES ahead of the current playhead
        constexpr int SKIP_DISTANCE = 5;  // Pages to skip on blocking (Phase 6b confirmed)

        // The window is anchored at the playhead's page start, so a fixed page
        // count under-covers by up to one page depending on where inside the
        // page the playhead sits; startPlayback() needs minBufferFrames_ beyond
        // the playhead itself, so size the window to reach that.
        const uint64_t targetEnd = currentPos + minBufferFrames_;
        int pagesNeeded = (int)((targetEnd - pageStart + pageSize - 1) / pageSize);
        if (pagesNeeded < READAHEAD_PAGES) pagesNeeded = READAHEAD_PAGES;

        for (int i = 0; i < pagesNeeded; i++) {
            uint64_t pos = pageStart + (uint64_t)i * pageSize;
            // No "already computed" shortcut on the frontier here: an edit can
            // invalidate pages BEHIND readaheadComputedUpTo_ (content epoch
            // bump), and those must be re-frozen. Validity is re-checked per
            // page instead, which self-heals under any edit/playback ordering.
            const uint64_t epochNow = synthOutput_->contentEpochNow();

            // PROBE, don't allocate. getOrAllocatePage() inserts an empty
            // placeholder for a position that has none, and freezePage() later
            // REUSES that placeholder instead of allocating a fresh page — so
            // the page it replaces is never recorded as stalePredecessor, and
            // the proposal-16 fallback (which is what keeps playback graceful
            // across an edit) has nothing to fall back TO at that position. The
            // readahead only wants to know whether a current page already
            // exists; asking non-destructively is both correct and cheaper.
            auto existing = synthOutput_->getPageIfExists(pos);
            if (existing && existing->validAspects != 0 &&
                existing->contentEpoch.load() >= epochNow) {
                // Already frozen and current; update prevPage and move on
                readaheadPrevPage_ = existing;
                if (pos + pageSize > readaheadComputedUpTo_)
                    readaheadComputedUpTo_ = pos + pageSize;
                if (readaheadLogCounter++ % 100 == 0) {
                    int64_t gap = (int64_t)readaheadComputedUpTo_ - (int64_t)currentPos;
                    TW_LOGW( "playback", "[READAHEAD] Page already exists: computed up to=%llu, playhead=%llu, gap=%.2f sec",
                            readaheadComputedUpTo_, currentPos, (double)gap / 48000.0 );
                }
                continue;
            }

            // Proposal 19 dataflow stage 5: with a scheduler, the readahead is
            // a pure DEMAND CONSUMER — it never renders. Demand the remaining
            // contiguous window (pages predecessor-chained within the demand)
            // and re-check on the next tick; the worker pool executes the page
            // DAG. Only NOT-current pages reach this point, so a demand can
            // never re-render a non-caching component out of position order
            // (the stage-4 lesson). The in-flight handle keeps the 20ms tick
            // from re-issuing an identical demand while nodes still execute.
            if (scheduler_) {
                const uint64_t wantEnd =
                    pageStart + (uint64_t)pagesNeeded * pageSize;
                // Coverage is positional AND epoch-scoped. A demand issued
                // before an edit was planned against the pre-edit graph, so it
                // can only ever publish pre-edit pages; treating it as "covered"
                // suppressed the re-demand until it happened to finish, and the
                // user went on hearing the old mix for up to a whole readahead
                // window (~4 s at 48 kHz). Superseding it immediately is what
                // makes a delete/mute/solo audible at once. The stale handle is
                // simply dropped — its nodes finish and publish pages that the
                // per-page validity check above then rejects as stale.
                const bool covered = pendingDemand_ && !pendingDemand_->done()
                    && pendingDemandEpoch_ == epochNow
                    && pendingDemandStart_ <= pos && pendingDemandEnd_ >= wantEnd;
                if (!covered) {
                    const int n = (int)((wantEnd - pos) / pageSize);
                    pendingDemand_ = scheduler_->requestGraphPages(
                        synthOutput_, pos, n, /*priority*/ 9);
                    pendingDemandStart_ = pos;
                    pendingDemandEnd_ = wantEnd;
                    pendingDemandEpoch_ = epochNow;
                }
                break;   // frontier advances on later ticks as pages land
            }

            // DEBUG: About to compute page
            auto start = std::chrono::steady_clock::now();
            auto pageStart = std::chrono::high_resolution_clock::now();

            // Compute this page (blocking, may take milliseconds). Via
            // requestPage (Phase 2a): dedups against concurrent revalidation of
            // the same page rather than double-rendering it.
            auto page = synthOutput_->requestPage(
                pos, nullptr, 0, (length_t)pageSize,
                engineSampleRate_, readaheadPrevPage_);

            auto pageEnd = std::chrono::high_resolution_clock::now();
            auto duration_ms = std::chrono::duration<double, std::milli>(pageEnd -
                pageStart).count();

            TW_LOGD( "playback", "[READAHEAD] Generated page [%llu, %llu) in %.2f ms (%.1f%% of page duration)",
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
                if (pos + pageSize > readaheadComputedUpTo_)
                    readaheadComputedUpTo_ = pos + pageSize;

                // DEBUG: Log completed page
                int64_t gap = (int64_t)readaheadComputedUpTo_ - (int64_t)currentPos;
                if (readaheadLogCounter++ % 50 == 0 || elapsed > 50) {
                    TW_LOGD( "playback", "[READAHEAD] Froze page at=%llu, computed up to=%llu, playhead=%llu, gap=%.2f sec, time=%.1fms",
                            pos, readaheadComputedUpTo_, currentPos, (double)gap / 48000.0, (double)elapsed );
                }
            } else {
                // Phase 6b: Sequential freeze blocked; try skip-ahead with fresh state
                if (readaheadPrevPage_ != nullptr) {
                    uint64_t skipPos = pos + (uint64_t)SKIP_DISTANCE * pageSize;
                    auto start_skip = std::chrono::steady_clock::now();

                    // Try to freeze skip-ahead page with NO prior state (fresh chain)
                    auto skipPage = synthOutput_->requestPage(
                        skipPos, nullptr, 0, (length_t)pageSize,
                        engineSampleRate_, nullptr);  // Fresh state!

                    auto elapsed_skip = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start_skip).count();

                    if (skipPage && skipPage->validAspects != 0) {
                        // Success: start new independent chain from skipPos
                        readaheadPrevPage_ = skipPage;
                        readaheadComputedUpTo_ = skipPos + pageSize;
                        TW_LOGD( "playback", "[READAHEAD] Skip-ahead: pos %llu -> %llu (5 pages forward, fresh state), time=%.1fms",
                                pos, skipPos, (double)elapsed_skip );
                        continue;  // Continue loop, next iteration tries skipPos+pageSize
                    }
                }

                // Neither sequential nor skip-ahead worked; give up for now
                if (readaheadLogCounter++ % 10 == 0) {
                    TW_LOGE( "playback", "[READAHEAD] freezePage failed or not frozen at pos=%llu (validAspects=%u), giving up",
                            pos, page ? page->validAspects.load() : 0 );
                }
                break;
            }
        }
    }
}

}  // namespace audio
