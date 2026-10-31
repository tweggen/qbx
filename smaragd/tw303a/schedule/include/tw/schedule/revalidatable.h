#ifndef _REVALIDATABLE_H_
#define _REVALIDATABLE_H_

#include <atomic>
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

    // --- PREVIEW-PREP RETRY BOOKKEEPING (QBX-259) ---------------------------
    //
    // A false from revalPrepPreview() is a DESIGNED outcome and not a failure
    // -- the contract above says so -- so the caller must not report it as
    // one. It was logged at ERROR, which cost twice over:
    //
    //   * the ORDINARY TRANSIENT reported as an error. Measured: it fires on
    //     the FIRST touch of an asset placement of a sub-arrangement, whose
    //     capture is not built yet ("content still loading", SCut's own
    //     comment), and on NONE of twelve warm repeat runs. An ERR line in an
    //     otherwise clean run trains a reader to ignore ERR lines here -- and
    //     the subsystem next door has one that must never be ignored
    //     (SCut::ensureReader's "capture did not converge ... serving it
    //     as-is", QBX-258).
    //   * the case that IS a defect produced THE SAME LINE. An object whose
    //     preview source can never be materialized means a clip permanently
    //     drawn as a grey box, which is the very thing revalPrepPreview()
    //     exists to prevent, and it was indistinguishable from the noise.
    //
    // The two are told apart by COUNTING, which is the idiom SCut already
    // uses for its own capture (captureRebuildAttempts_ / captureStaleReported_
    // in scut.h): silent while it is a retry, once and loudly when it is not.
    // Kept HERE rather than in a map keyed by object pointer in the
    // revalidator, because the lifetime is then the object's own and a retired
    // object cannot leave a stale entry behind to be charged to its successor
    // at the same address.
    static const int kMaxPrepAttempts = 8;

    /** One prep attempt came back false. Returns true EXACTLY ONCE -- on the
     *  attempt that reaches kMaxPrepAttempts -- so a caller logs a stuck
     *  object once and says nothing at all about the ordinary transient. */
    bool notePrepRetry()
    {
        const int n = ++prepAttempts_;
        if (n < kMaxPrepAttempts) return false;
        return !prepStuckReported_.exchange(true);
    }

    /** Prep succeeded: forget the streak, so a later transient is treated as
     *  a transient instead of inheriting the tail of an older streak. The
     *  reset is what makes the counter a STREAK rather than a lifetime total.
     */
    void notePrepConverged()
    {
        prepAttempts_.store(0);
        prepStuckReported_.store(false);
    }

    /** Attempts in the current streak. For tests and triage only. */
    int prepAttempts() const { return prepAttempts_.load(); }

private:
    // Atomic because several workers may dispatch for one object: the
    // revalidator holds a ref per job, not a per-object lock, and
    // dispatchRecomputation runs outside CS1.
    std::atomic<int>  prepAttempts_{0};
    std::atomic<bool> prepStuckReported_{false};
};

#endif
