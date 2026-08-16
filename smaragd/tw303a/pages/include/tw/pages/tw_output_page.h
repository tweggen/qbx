#ifndef _TW_OUTPUT_PAGE_H_
#define _TW_OUTPUT_PAGE_H_

#include <cstdint>
#include "tw/core/twtypes.h"
#include <algorithm>
#include <vector>
#include <memory>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>

// C++17 type erasure for internal state snapshots
#include <any>

#include "tw/pages/page_interface.h"
#include "tw/pages/tw_page_accounting.h"

// Forward declaration
class twComponent;

// Phase 5 Gap 11: Unified page size constant
// All frozen pages (component, SObject) use this size for alignment
// 256 kB provides ~0.68 seconds at 48kHz stereo, balancing:
//   - Cache coherency (fits in L3)
//   - Granularity (fine enough for responsive updates)
//   - Memory overhead (sparse page allocation)
static constexpr size_t FROZEN_PAGE_SIZE_BYTES = 256 * 1024;

// Aspects that can be frozen for a component's output page
enum twRenderAspect : uint32_t {
    twAspectPreview   = 1u << 0,  // Waveform peaks for timeline visualization
    twAspectPlayback  = 1u << 1,  // Real-time audio for playback
    twAspectExport    = 1u << 2,  // High-quality audio for file export
    twAspectMetadata  = 1u << 3,  // Duration, peak levels, analysis data

    twAspectAll       = twAspectPreview | twAspectPlayback | twAspectExport | twAspectMetadata,
};

// Frozen output of a component for a time window
// Stores component output samples + internal state snapshot for sequential rendering
//
// CHANNELS (proposal 36 §4.1, built by B1b).
// A page is PLANAR and carries its own channel count. Channel c occupies
// [c * CHANNEL_STRIDE, c * CHANNEL_STRIDE + channelFrames()) of the sample
// buffer, and CHANNEL_STRIDE is the constant FRAME_CAPACITY — deliberately NOT
// validFrames, which shrinks as a tail runs out and would force every consumer
// to learn the stride at read time.
//
// `channels` is IMMUTABLE after allocation (§4.5): later phases let a stale page
// of the wrong width be treated as a MISS rather than as audio, and that rule is
// only sound if a page's width cannot change under a reader. It is enforced by
// the type — a const member, a private buffer and no setter — not by a comment.
//
// At B1b NOTHING in the tree allocates a page wider than 1. The width dimension
// exists, every consumer reads through channelPtr(), and the byte-exactness gate
// therefore says something: any golden that moves is a mis-converted call site.
struct twOutputPage : public PageBase {
    // Phase 5 Gap 11: Unified page size
    static constexpr size_t PAGE_SIZE = FROZEN_PAGE_SIZE_BYTES;  // 256 kB per channel
    static constexpr size_t FRAME_CAPACITY = PAGE_SIZE / sizeof(float);  // frames PER CHANNEL

    // Distance in samples between channel c and channel c+1. Constant by design
    // (see the class comment); a page's storage is channels * CHANNEL_STRIDE.
    static constexpr size_t CHANNEL_STRIDE = FRAME_CAPACITY;

    // Time range this page covers (in sample frames)
    offset_t startPosition;

    // Number of valid frames in this page (may be less than FRAME_CAPACITY).
    // PER CHANNEL: every channel of a page covers the same time range.
    uint32_t validFrames;

    // Which rendering aspects have been computed for this page
    std::atomic<uint32_t> validAspects;

    // Generation counter to detect invalidation
    // Incremented when invalidateAllPages() invalidates this page.
    // Audio threads compare against their cached generation to detect stale pages.
    // Allows audio to drop references to pages that have been invalidated and repurposed.
    std::atomic<uint64_t> generation{0};

    // Global content epoch this page was rendered at (tw303aEnvironment::contentEpoch()).
    // Any timeline/graph edit bumps the environment's epoch; a page whose epoch is
    // older than the environment's is stale and must not be served, regardless of
    // validAspects. 0 = never stamped (always stale).
    std::atomic<uint64_t> contentEpoch{0};

    // Proposal 16: the frozen pre-edit page this placeholder replaced.
    // While a stale page is being re-frozen (validAspects == 0 here), playback
    // serves the predecessor as stale-but-consistent audio instead of silence.
    // Access ONLY via std::atomic_load/atomic_store (the audio thread reads it
    // without the component mutex); cleared when this page is stamped frozen.
    std::shared_ptr<twOutputPage> stalePredecessor;

    // Internal state snapshot (for sequential components like reverbs, delays)
    // Allows resuming rendering from this page's endpoint without losing state
    std::any internalState;

    // Timing for stale-data fallback logic
    std::chrono::steady_clock::time_point createdAt;

    // Phase 5 Gap 12: Multi-Consumer Page Locking
    // Protects concurrent access from multiple render threads reading same page.
    // Writers (revalidator) hold this lock while freezing/populating the page.
    // Readers (render loops) acquire this lock when reading internalState or during updates.
    // Lock acquired during freezePage() state capture/restore operations.
    mutable std::mutex pageMutex;

    // `channels` is fixed here and can never change again. Defaulted to 1, so
    // every existing `make_shared<twOutputPage>()` allocates exactly the page it
    // allocated before this milestone, to the byte.
    explicit twOutputPage(std::uint16_t channels = 1)
        : startPosition(0),
          validFrames(0),
          validAspects(0),
          generation(0),
          createdAt(std::chrono::steady_clock::now()),
          channels_(clampChannels(channels))
    {
        channelFrames_ = FRAME_CAPACITY;
        samples_.resize((size_t)channels_ * CHANNEL_STRIDE);
        // Proposal 36 B1a: page memory is accounted at the PAGE's own lifetime,
        // because there is no pool to ask. accountedBytes_ is remembered rather
        // than recomputed in the destructor, so an accounting pair can never be
        // unbalanced by a later resizeMonoScratch() — the counters stay exact
        // even if some future code path grows a page in place.
        accountedBytes_ = samples_.size() * sizeof(float);
        tw::pages::PageAccounting::onPageAllocated(accountedBytes_);
    }

    ~twOutputPage()
    {
        tw::pages::PageAccounting::onPageReleased(accountedBytes_);
    }

    // A page is not copyable and not assignable. It never was — a std::mutex and
    // three std::atomics see to that — but saying so explicitly is half of AC
    // B1b.5: with `channels_` const there is no assignment that could carry a
    // different width into a live page, and the compiler enforces it.
    twOutputPage(const twOutputPage&) = delete;
    twOutputPage& operator=(const twOutputPage&) = delete;
    twOutputPage(twOutputPage&&) = delete;
    twOutputPage& operator=(twOutputPage&&) = delete;

    // ---- geometry -------------------------------------------------------

    /// Channel count of THIS page. Fixed at construction; see §4.4 — the width
    /// a consumer may act on is the width of the page in its hand, never a
    /// producer's declared width.
    std::uint16_t channels() const noexcept { return channels_; }

    /// Frames of storage per channel. FRAME_CAPACITY for every page the engine
    /// freezes; smaller only for a width-1 scratch page (resizeMonoScratch).
    /// This — not sampleCount() — is what a frame index must be bounded by.
    size_t channelFrames() const noexcept { return channelFrames_; }

    /// Total floats of storage (channels * CHANNEL_STRIDE, or channelFrames()
    /// for a mono scratch page). Almost never what a caller wants: a frame
    /// bound is channelFrames().
    size_t sampleCount() const noexcept { return samples_.size(); }

    /// Planar channel base pointer. An out-of-range channel is a programming
    /// error: it is reported once and answered with channel 0, so the mistake is
    /// loud in the log and can never be an out-of-bounds read on the audio
    /// thread. The §4.4 clamp (min(latchIndex, channels-1)) is a PLUG rule and
    /// lives in twStreamingLatch, not here — this must not quietly implement a
    /// downmix policy.
    float* channelPtr(idx_t c) { return samples_.data() + channelBase_(c); }
    const float* channelPtr(idx_t c) const { return samples_.data() + channelBase_(c); }

    /// Zero every channel over the whole buffer.
    void fillSilence()
    {
        std::fill(samples_.begin(), samples_.end(), 0.0f);
    }

    /// The pre-existing MONO SCRATCH path: twComponent::calcOutputTo and
    /// IOVector::CreateFromBuffer build a throwaway page as a plain buffer of a
    /// caller-chosen length. Legal only at width 1, where a stride is
    /// meaningless. Does not re-account: the page already reported its
    /// construction-time bytes (B1a), and that pairing is what keeps the
    /// accounting exact.
    void resizeMonoScratch(size_t frames)
    {
        if (channels_ != 1) {
            reportScratchOnWidePage(channels_);
            return;
        }
        samples_.resize(frames, 0.0f);
        channelFrames_ = frames;
    }

    // Sample bytes this page reported to the accounting when it was built.
    size_t accountedBytes() const { return accountedBytes_; }

    // PageBase interface implementation
    std::mutex& getMutex() const override { return pageMutex; }
    offset_t getStartPosition() const override { return startPosition; }
    void setStartPosition(offset_t pos) override { startPosition = pos; }
    uint32_t getValidAspects() const override { return validAspects.load(); }
    void setValidAspects(uint32_t aspects) override { validAspects.store(aspects); }
    uint64_t getGeneration() const override { return generation.load(); }
    void incrementGeneration() override { generation.fetch_add(1); }
    size_t getPageSize() const override { return PAGE_SIZE; }
    uint32_t getValidFrames() const override { return validFrames; }
    void setValidFrames(uint32_t frames) override { validFrames = frames; }
    // Channel 0, so a width-1 call site stays correct (§4.1). This is the page's
    // own class, so the raw buffer access below is where it belongs.
    void* getDataPtr() override { return samples_.data(); }
    const void* getDataPtr() const override { return samples_.data(); }
    std::any& getInternalState() override { return internalState; }
    const std::any& getInternalState() const override { return internalState; }
    std::chrono::steady_clock::time_point getCreatedAt() const override { return createdAt; }

private:
    // Frozen audio samples, planar: channel c starts at c * CHANNEL_STRIDE.
    // PRIVATE on purpose — an accessor-only buffer is what makes "every consumer
    // was converted" a fact the compiler checks rather than a grep result.
    std::vector<float> samples_;

    size_t channelFrames_ = 0;
    size_t accountedBytes_ = 0;

    // const: the width cannot be reassigned, and neither can a whole page (the
    // assignment operators above are deleted). AC B1b.5.
    const std::uint16_t channels_;

    size_t channelBase_(idx_t c) const
    {
        if (c >= 0 && (size_t)c < (size_t)channels_) {
            return (size_t)c * CHANNEL_STRIDE;
        }
        reportChannelOutOfRange(c, channels_);
        return 0;
    }

    static std::uint16_t clampChannels(std::uint16_t requested);
    static void reportChannelOutOfRange(idx_t c, std::uint16_t channels);
    static void reportScratchOnWidePage(std::uint16_t channels);
};

// WIDTH MISMATCH IS A MISS (proposal 36 §4.5, wired by B2).
//
// Returns true iff `page` may be READ by a consumer of a producer that currently
// declares `producerChannels` channels. A page whose width disagrees is treated
// as a MISS — never as audio: playback falls back to silence for that page, a
// meter decays.
//
// WHY THIS RULE EXISTS AT ALL. twOutputPage::channels is immutable after
// allocation, so a project width change cannot mutate a cached page; it bumps the
// content epoch instead, and the old-width pages become stale by the existing
// mechanism. But proposal 16's stale-page fallback DELIBERATELY serves stale
// pages during live playback, so that is the one path on which a page of the
// WRONG width can reach the RT callback or twLevelProbe — and reading
// channelPtr(1) of a stale width-1 page would be an out-of-bounds read ON THE
// AUDIO THREAD. (channelPtr clamps and reports rather than reading out of bounds,
// but a clamp that fires on the audio thread is a bug being papered over, not a
// policy.) Width mismatch is the one staleness that is not tolerable.
//
// A FRESH page always passes: pages are allocated at their producer's declared
// width, so only a page that predates a width change can fail. In particular a
// correctly-wide page is NOT rejected because a consumer only reads channel 0 —
// that consumer is narrow, not wrong, and rejecting it would silence playback
// between the milestone that widens the graph and the one that widens the sink.
//
// nullptr is not usable (there is nothing to read), which lets call sites write
// the check and the null test as one condition.
bool twPageWidthUsable(const twOutputPage *page, idx_t producerChannels);

// THE §4.4 READ CLAMP, in one place (proposal 36, B4).
//
// A consumer that wants channel `c` of `page` must read channel
// min(c, page.channels() - 1) — "mono plays on every channel", exactly what
// twSampleSource::read has always done for an out-of-range channel and what
// twStreamingLatch::copyData does for a plug index. B4 makes several WIDE
// components (twTrackMix mixing narrower clips, twMixer summing narrower
// tracks, twPluginInsert gathering a narrower upstream) do the same thing, and
// spelling the clamp out in each of them is how the four copies drift apart.
//
// It is deliberately NOT inside twOutputPage::channelPtr(): channelPtr treats an
// out-of-range channel as a PROGRAMMING ERROR and reports it, which is right for
// a page-internal accessor. This is a POLICY, applied by a consumer that knows
// it is reading a narrower producer on purpose.
inline idx_t twPageClampChannel(const twOutputPage &page, idx_t c)
{
    if (c < 0) return 0;
    const idx_t n = (idx_t) page.channels();
    return (c > (idx_t)(n - 1)) ? (idx_t)(n - 1) : c;
}

#endif  // _TW_OUTPUT_PAGE_H_
