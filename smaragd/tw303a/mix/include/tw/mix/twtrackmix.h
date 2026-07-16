
#ifndef _TW_TRACKMIX_H
#define _TW_TRACKMIX_H

#include <qobject.h>
#include <atomic>
#include <vector>
#include <functional>

#include "tw/graph/twcomponent.h"

class tw303aEnvironment;

class twView;

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
    // getComponentFn returns the current component (allows dynamic lookup);
    // mapPosFn optionally folds the clip's slip offset into clip-relative
    // positions before they reach the component (see twView).
    void insertClip(const void *key, offset_t startTime, length_t duration,
                    std::function<std::shared_ptr<twComponent>()> getComponentFn,
                    std::function<offset_t(offset_t)> mapPosFn = nullptr);
    void removeClip(const void *key);
    void updateClip(const void *key, offset_t newStartTime, length_t newDuration);

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
