
#ifndef _TW_TRACKMIX_H
#define _TW_TRACKMIX_H

#include <qobject.h>
#include <atomic>
#include <vector>
#include <functional>

#include "tw/graph/twcomponent.h"

class tw303aEnvironment;

class twView;

// Timeline extent affected by a clip edit ([start, end) in track/absolute
// frames): the union of the clip's pre- and post-edit windows. Returned by
// the clip mutators so the app's invalidation walk (proposal 18 Phase 5)
// can stale only this range on the downstream chain. empty() means the
// edit touched nothing audible.
struct twEditRange {
    uint64_t start = 0;
    uint64_t end = 0;
    bool empty() const { return end <= start; }
    void unite(uint64_t s, uint64_t e) {
        if (e <= s) return;
        if (empty()) { start = s; end = e; return; }
        if (s < start) start = s;
        if (e > end) end = e;
    }
};

// Clip entry: timeline position, stable view wrapper, and state snapshot
struct ClipEntry {
    offset_t     startTime;
    length_t     duration;      // 0 = unbounded
    const void  *key;           // Caller-supplied identity (e.g. the SLink); two
                                // clips of one sample share a component, so the
                                // component pointer can NOT identify a clip
    std::shared_ptr<twView> view;  // Stable wrapper; shared so it outlives any
                                // async preview/freeze job that still references
                                // it after the clip is removed (see teardown)
    std::shared_ptr<twOutputPage> previousPage;  // State snapshot for resumption
};

// State snapshot for page boundary continuity
struct TrackInternalState {
    offset_t playOffset;  // Track timeline cursor position at page freeze
};

class twTrackMix
    : public twComponent
{
public:
    twTrackMix( tw303aEnvironment &env );
    ~twTrackMix();

    // Clip management (called by STrack on the UI thread)
    // These are protected by the inherited mutex() from twComponent
    // Clips are identified by an opaque caller-supplied key (STrack passes the
    // SLink) — component pointers are ambiguous because two clips of the same
    // sample resolve to the same shared component until their readers exist.
    // getComponentFn returns the current component for position-independent
    // queries (structure/teardown/live pull); resolveFn (proposal 19 Inv-1)
    // resolves {component, mappedPos} together for the freeze/seek path so the
    // slip mapping and the component can never straddle a lazy reader build
    // (see twView). resolveFn null = identity mapping over getComponentFn.
    // Each mutator range-scopes its own page invalidation (only pages
    // intersecting the affected extent go stale) and RETURNS that extent
    // so the caller can stale the downstream chain the same way.
    twEditRange insertClip(const void *key, offset_t startTime, length_t duration,
                    std::function<std::shared_ptr<twComponent>()> getComponentFn,
                    std::function<twResolvedClip(offset_t)> resolveFn = nullptr);
    twEditRange removeClip(const void *key);
    twEditRange updateClip(const void *key, offset_t newStartTime, length_t newDuration);

    // Proposal 19 dataflow stage 2 — planner override: the trackmix consumes
    // its clips by DIRECT view->freezePage calls, so its deps are not input
    // plugs but, per clip overlapping the page, the resolveClip()-resolved
    // {component, mappedPos} — captured under mutex() through the SAME
    // single-resolution path (twView::resolve) the render uses, so plan and
    // render cannot disagree (Inv-1 extended to the plan).
    twPagePlan planPage(uint64_t pageStart) override;

    // Track intrinsic properties (gain, mute) — applied to all output
    void setTrackMute(bool muted);
    void setTrackGain(double gainDb);

public:
    virtual void setBufferSize( length_t );

public:
    virtual bool isSeekable() const;
    virtual int seekTo( offset_t );
    virtual void createOutputLatches();

    virtual idx_t getNInputs() const;
    virtual idx_t getNOutputs() const;
    virtual const char *getInputName( idx_t ) const;
    virtual const char *getOutputName( idx_t ) const;

    // Phase 3: IOVector-based interface (type-safe, page-backed)
    virtual length_t calcOutputTo( IOVector& dest, idx_t idx ) override;

    // Phase 3: Page-based rendering — freeze track output to pages
    // Enables renderObjectInto replacement and unified page-based pipeline
    virtual std::shared_ptr<twOutputPage> freezePage(
        uint64_t startPos,
        const sample_t *inputData,
        uint64_t inputOffset,
        length_t inputLength,
        int sampleRate,
        std::shared_ptr<twOutputPage> previousPage = nullptr
    ) override;

    // Phase 3: Preview rendering using freezePage at lower resolution
    virtual std::shared_ptr<twOutputPage> freezePreviewPage(
        uint64_t startPos,
        length_t length,
        int previewSampleRate,
        int fullSampleRate,
        std::shared_ptr<twOutputPage> previousPage = nullptr
    ) override;

protected:

    virtual void reset() override;

    // State snapshot for page boundary continuity
    // Captures/restores playOffset_ to ensure consistent clip positions across pages
    std::any captureInternalState() const override;
    void restoreInternalState(const std::any& state) override;

    // Helpers: capture/restore state without acquiring mutex (caller must hold mutex)
    std::any captureInternalState_nolock() const;
    void restoreInternalState_nolock(const std::any& state);

    // Teardown protocol
    virtual void teardown() override;

private:
    // Helpers: protect clips_ iteration against concurrent modification.
    // These use the inherited mutex() from twComponent base class to avoid
    // introducing a second mutex which could cause deadlock.
    int seekTo_nolock(offset_t newOffset);
    length_t freezePage_nolock(std::shared_ptr<twOutputPage> page, uint64_t startPos,
                               length_t length, int sampleRate,
                               std::shared_ptr<twOutputPage> previousPage);

    std::vector<ClipEntry> clips_;            // Timeline entries (sorted by startTime)
    std::atomic<offset_t> playOffset_{ 0 };   // Atomic: protects race between UI seek and audio render
    bool trackMuted_{ false };                 // Track mute state
    double trackGainDb_{ 0.0 };                // Track gain in dB

};

#endif
