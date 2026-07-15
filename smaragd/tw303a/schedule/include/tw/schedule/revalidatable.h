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

    // Keep references for objects in queues
    virtual void revalAddRef() = 0;
    virtual void revalRemoveRef() = 0;

    // True if the given aspects are stale on the current page.
    virtual bool revalNeeded_nolock(uint32_t aspects) const = 0;

    // Two-page buffer: the page being built (not yet visible to readers).
    virtual std::shared_ptr<CapturePageData> revalGetNextPage_nolock() const = 0;
    virtual void revalSetNextPage_nolock(std::shared_ptr<CapturePageData> page) = 0;

    // Atomically publish nextPage as currentPage.
    virtual void revalSwapPages_nolock() = 0;

    // The object's DSP root, for freezePreviewPage()-based preview rendering.
    virtual std::shared_ptr<twComponent> revalRootComponent() = 0;

    // Aspect recomputation hooks (virtual dispatch to the concrete object).
    virtual void revalRecomputeMetadata(CapturePageData &page) = 0;
    virtual void revalRecomputeExport(CapturePageData &page) = 0;

    // Called on the worker BEFORE the generic preview render, with no object
    // lock held. Lets the object materialize whatever its preview reads from
    // (e.g. a container cut's offline capture). Return false if the source
    // could not be materialized: the Preview aspect is then NOT marked valid,
    // so the next getCapture() re-schedules and the preview retries instead
    // of staying blank forever.
    virtual bool revalPrepPreview() { return true; }

    // Called on the worker after aspects were recomputed and published, with
    // no locks held. `aspects` holds only the aspects that actually succeeded.
    // Implementations use this to notify the UI thread to repaint (queued).
    virtual void revalCompleted(uint32_t aspects) { (void)aspects; }
};

#endif
