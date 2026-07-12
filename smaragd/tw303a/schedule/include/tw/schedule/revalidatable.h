#ifndef _REVALIDATABLE_H_
#define _REVALIDATABLE_H_

#include <cstdint>
#include <memory>
#include <mutex>

class twComponent;
struct CapturePageData;

/**
 * What the CaptureRevalidator's worker threads need from a revalidatable
 * document object, expressed as an engine-owned interface so the engine
 * never includes app headers (modularization proposal 14, Phase 0 /
 * Open Question 2). SObject implements this by thin delegation to its
 * existing members, preserving the historical dispatch exactly.
 *
 * Locking contract (unchanged from the SObject-based implementation):
 * the *_nolock methods require revalMutex() to be held by the caller;
 * revalRecompute*() are called with NO object lock held (they may run for
 * tens of milliseconds) but with the destination page's lock held by the
 * worker.
 */
class IRevalidatable {
public:
    virtual ~IRevalidatable() = default;

    // The object's one mutex; guards the page pointers below.
    virtual std::mutex &revalMutex() const = 0;

    // True if the given aspects are stale on the current page.
    virtual bool revalNeeded_nolock(uint32_t aspects) const = 0;

    // Two-page buffer: the page being built (not yet visible to readers).
    virtual std::shared_ptr<CapturePageData> revalGetNextPage_nolock() const = 0;
    virtual void revalSetNextPage_nolock(std::shared_ptr<CapturePageData> page) = 0;

    // Atomically publish nextPage as currentPage.
    virtual void revalSwapPages_nolock() = 0;

    // The object's DSP root, for freezePreviewPage()-based preview rendering.
    virtual twComponent &revalRootComponent() = 0;

    // Aspect recomputation hooks (virtual dispatch to the concrete object).
    virtual void revalRecomputeMetadata(CapturePageData &page) = 0;
    virtual void revalRecomputeExport(CapturePageData &page) = 0;
};

#endif
