
#ifndef _TWCOMPONENT_H_
#define _TWCOMPONENT_H_

// Split per proposal 14, Phase 1: core types live in twtypes.h, the latch
// machinery in twlatch.h. This header keeps including both so existing call
// sites compile unchanged; it no longer pulls in any Qt header.
#include <memory>
#include <mutex>
#include <condition_variable>
#include <map>
#include <atomic>
#include <vector>

#include "tw/core/twtypes.h"
#include "tw/graph/twlatch.h"
#include "tw/core/exc.h"
#include "tw/core/twformat.h"
#include "tw/pages/tw_output_page.h"
#include "tw/graph/tw_freeze_context.h"

// Forward declaration to avoid circular includes
class IOVector;

enum class ComponentState {
    ACTIVE,    // Normal operation
    ZOMBIE,    // Tearing down, outputs silence
    DELETED    // Memory freed (unused in this implementation)
};

#undef DEBUG_COMPONENT

class tw303aEnvironment;
class twComponent;
struct twFrozenInputs;   // tw/graph/tw_frozen_inputs.h (dataflow leaf renderer)

// Proposal 19 Inv-1: the atomic result of resolving one clip position. A
// windowed clip (SCut/STakeStack) must hand a freeze BOTH the component to
// render AND the position mapped into that component's own domain, computed
// from ONE structural snapshot — resolving them separately can straddle a
// concurrent lazy reader build (the takes_group_broadcast race class). twView
// carries a resolver returning this; SObject::resolveClip produces it.
struct twResolvedClip {
    std::shared_ptr<twComponent> component;
    offset_t                     mappedPos = 0;
};

class twComponent
    : public std::enable_shared_from_this<twComponent>
{
private:
    int currentOperation_;
    
protected:
    virtual int doInitOperation( int );

    int inputsSet_;
    tw303aEnvironment &env;
    std::vector<std::shared_ptr<twLatch>> pOutputLatches_;
    std::vector<std::shared_ptr<twLatchOutput>> pInputPlugs_;

    // Teardown protocol state
    std::atomic<ComponentState> state_{ComponentState::ACTIVE};

    // Teardown protocol: parent component tracking
    std::weak_ptr<twComponent> parentComponent_;  // Parent that owns this component
    idx_t myInputIndex_{-1};  // Which input slot am I in parent's pInputPlugs_?

    friend class twLatch;
    friend class twStreamingLatch;
    
public:
    twComponent( tw303aEnvironment & env );
    virtual ~twComponent();
    
    virtual bool isSeekable() const;
    virtual int seekTo( offset_t );
    virtual offset_t tellPos() const;
    virtual void resetAllLatches();  // Reset all output latches to offset 0

    // Re-position this component's input-side stream readers (twLatchOutput
    // offsets) to pos. Part of the position protocol in freezePage(): after
    // seekTo(pos), the input readers must jump too, or the next render keeps
    // pulling upstream content for the old position.
    void seekInputStreams( offset_t pos );

    // Reset component to initial state (silence, zero position, empty buffers).
    // Called before sequential rendering from scratch or to resume from snapshot.
    // Essential for freezing model: every page starts from known reset state.
    virtual void reset() = 0;

    // Push-based rendering: render N frames with pre-prepared input (Phase 2 - Gap 8)
    // Used by freezePage() to fill component output pages sequentially.
    //
    // Renders from current internal state (which may be restored from snapshot).
    // Advances internal state by N frames. Different from calcOutputTo() which
    // reads input from upstream latches; renderFrames() takes explicit input buffer.
    //
    // Args:
    //   output - destination buffer (must hold N frames)
    //   length - frames to render
    //   input - pre-prepared input samples (may be nullptr for sources with no input)
    //   inputLength - valid frames in input buffer
    //   idx - output index (for multi-channel components)
    //
    // Returns: frames actually rendered (may be < length if input exhausted)
    //
    // Default: fall back to calcOutputTo() for backwards compatibility.
    // Override: in components that need to accept pre-prepared input for freezing.
    virtual length_t renderFrames(sample_t *output, length_t length,
                                   const sample_t *input, length_t inputLength,
                                   idx_t idx) {
        // Default: components without explicit renderFrames() use pull-based calcOutputTo()
        // This maintains compatibility during transition to push-based freezing model
        return calcOutputTo(output, length, idx);
    }

    // ========== Phase 3 Refactor: IOVector-based interface ==========
    // NEW: Type-safe interface using IOVector for bounds-checked rendering.
    // Default implementation wraps raw-pointer interface for compatibility.
    // Components can override this new interface when ready for type-safe rendering.
    // NOTE: All overrides should check ZOMBIE state at the start:
    //   if (state_.load(std::memory_order_acquire) == ComponentState::ZOMBIE) {
    //       return dest.fillSilence(0, dest.length());
    //   }
    virtual length_t calcOutputTo( IOVector& dest, idx_t idx );

    // LEGACY (DEPRECATED): Raw-pointer interface (removed in Phase 3 migration).
    // DEPRECATION TIMELINE:
    //   - v0.x: Both interfaces work (IOVector preferred for new code)
    //   - v1.0: Raw-pointer interface will be REMOVED
    //   - Migration: Use freezePage() for page-based rendering, or use IOVector for compatibility
    // For migration guide, see: docs/COMPONENT_MIGRATION_GUIDE.md
    //
    // Default implementation (Phase 3): wraps IOVector in temporary page buffer for compatibility.
    // During Phase 3 migration, subclasses remove their implementations and rely on default.
    [[deprecated("Use freezePage() or IOVector-based calcOutputTo() instead. Raw-pointer interface will be removed in v1.0")]]
    virtual length_t calcOutputTo( sample_t *pDest, length_t length, idx_t idx );  // NOT pure virtual

    void setInput( idx_t idx, twLatchOutput * pLatchOutput );
    virtual twLatchOutput *getInputPlug( idx_t ) const;
    int getInputsSet() const { return inputsSet_; }
    virtual twLatchOutput *linkOutput( idx_t idx );

    virtual void allocPlugs();
    virtual void init();
    virtual void createOutputLatches() = 0;

    // Teardown Protocol
    // Mark component as ZOMBIE, deregister from parent, notify dependents, then recursively tear down children
    virtual void teardown();

    // Set input slot to nullptr (called by child during teardown to deregister from parent)
    virtual void removeInput(idx_t idx);

    // Callback when a dependency is being torn down (override in components with external dependencies)
    virtual void onDependencyTeardown(std::shared_ptr<twComponent> dep);

    // Set parent tracking for safe teardown (called when component is wired to a parent)
    // parent: shared_ptr to parent component (weak_ptr internally prevents circular refs)
    // inputIndex: which slot in parent's pInputPlugs_ array this component occupies
    void setParentComponent(std::shared_ptr<twComponent> parent, idx_t inputIndex) {
        parentComponent_ = parent;
        myInputIndex_ = inputIndex;
    }
    
    virtual idx_t getNInputs() const = 0;
    virtual idx_t getNOutputs() const = 0;
    virtual const char *getInputName( idx_t idx ) const = 0;
    virtual const char *getOutputName( idx_t idx ) const = 0;

    // --- Format negotiation (proposal 04 §3) -----------------------------
    // Seed capability domains for one port. Default: mono Float32 at any rate.
    virtual twFormatCaps getOutputCaps( idx_t idx ) const;
    virtual twFormatCaps getInputCaps ( idx_t idx ) const;

    // The node's in<->out coupling relation, iterated to a fixpoint by the
    // negotiator. It narrows the given port domains to mutual consistency and
    // MUST be monotone (remove candidates only) — that is what guarantees
    // termination. Returns true iff it narrowed anything. The default couples
    // every port to one common rate (a node that neither resamples nor
    // rate-mixes); a rate-decoupling node (resampler) overrides this to return
    // false. Contract: domains are concrete (the negotiator has expanded "any"
    // to the candidate set D before calling), so an empty domain means
    // infeasible, not "any".
    virtual bool narrowCaps( twPortDomains &ports ) const;

    // Commit a single chosen format per port after the negotiation fixpoint.
    // The node records them and does any heavy, node-specific setup (a
    // resampler would build its kernel here). Default: record the formats.
    // Returns false if the committed format is unworkable for this node.
    virtual bool commitFormats( const twFormat *in,  idx_t nIn,
                                const twFormat *out, idx_t nOut );
    
    virtual void setBufferSize( length_t ) {};

    int initOperation( int );

    // --- Internal State Snapshots (Phase 1 - Gap 2) --------------------------
    // Sequential/stateful components (reverbs, delays, grain) can save/restore state
    // to enable sequential rendering with state resumption between pages.
    //
    // Example: Spring Reverb
    //   Page 0 [0..PAGE_SIZE]: render from reset, save delay line state → OutputPage.internalState
    //   Page 1 [PAGE_SIZE..2*PAGE_SIZE]: restore delay line state, render, save new state
    //   Result: Reverb output is continuous; no artifacts at page boundaries
    //
    // Stateless components (oscillators, simple mixers) use default (empty any, no-op restore).

    // Capture current internal state for serialization into OutputPage.internalState.
    // Default: return empty std::any (stateless components need no override).
    // Override in stateful components: reverbs, delays, grain time-stretch, etc.
    virtual std::any captureInternalState() const {
        return std::any();  // Default: no state to capture
    }

    // Restore internal state from snapshot (for sequential rendering resume).
    // Called before freezing a page when resuming from a previous page's snapshot.
    // Default: no-op (stateless components need no override).
    // Override in stateful components to restore delay lines, grain state, etc.
    virtual void restoreInternalState(const std::any& state) {
        // Default: no-op (state parameter unused)
        (void)state;  // Suppress unused parameter warning
    }

    // --- Output Page Caching (Phase 1 - Gap 1) ----------------
    // Component-level frozen output pages for efficient multi-consumer rendering.
    // All components own a cache of their output for time windows (pages).
    // This enables:
    // - One computation per component per time window, no redundancy
    // - Sequential rendering from reset state with internal state snapshots
    // - Deterministic audio (same input → same output, always)

    // Get or allocate a frozen output page covering the given time position.
    // Returns non-null shared_ptr; page may not be frozen yet (check validAspects).
    // Non-blocking: consumers can fall back to stale pages if not ready.
    std::shared_ptr<twOutputPage> getOrAllocatePage(
        uint64_t startPos,
        uint32_t aspectsMask = twAspectAll
    );

    // Lock-free page lookup (audio thread only).
    // Returns existing page if found, nullptr if not. Never allocates.
    // Safe for real-time threads because it uses only atomic reads.
    std::shared_ptr<twOutputPage> getPageIfExists(uint64_t startPos);

    // Release pages outside of a time retention window (memory management).
    // Frees pages whose [startPos, startPos+PAGE_SIZE) range ends before keepAfterPos.
    void releaseOldPages(uint64_t keepAfterPos);

    // Get all cached pages in a time range (for iteration, cleanup, debugging).
    std::vector<std::shared_ptr<twOutputPage>> getPagesInRange(
        uint64_t startPos,
        uint64_t endPos
    ) const;

    // Invalidate all cached pages (called when component parameters change).
    // Marks all pages' validAspects as 0, triggering re-freezing.
    // Also invalidates downstream components (Gap 9: invalidation cascade).
    void invalidateAllPages();

    // Per-component content epoch (proposal 15: scoped invalidation).
    // Pages this component renders are stamped with the epoch read before
    // rendering; a page is stale when page->contentEpoch < contentEpochNow().
    // Bumping is SCOPED: an edit bumps the edited component and everything on
    // its path to the root (the app walks the SObject tree — see
    // SObject::invalidateRenderPath()), leaving sibling caches untouched.
    // Lock-free; safe on the audio thread.
    uint64_t contentEpochNow() const {
        return contentEpoch_.load(std::memory_order_acquire);
    }

    // Mark this component's cached/held pages stale (its output changed).
    // Virtual so containers can forward (twPluginChain bumps its inserts,
    // whose pages bake in upstream audio).
    virtual void bumpContentEpoch() {
        contentEpoch_.fetch_add(1, std::memory_order_acq_rel);
    }

    // RANGE-SCOPED variant (proposal 18 Phase 5, refining proposal 15):
    // only pages intersecting [start, end) go stale. Advances the epoch,
    // then RE-BLESSES every cached page that (a) does not intersect the
    // range and (b) was CURRENT at the moment of the bump — a page already
    // stale from an earlier, un-refrozen edit stays stale (re-blessing it
    // would resurrect outdated audio). Placeholders being rendered right
    // now are left alone: they stamp the epoch they read at entry and
    // simply re-freeze once more if they raced the edit. Virtual with the
    // same forwarding structure as bumpContentEpoch.
    virtual void invalidatePagesInRange(uint64_t start, uint64_t end);

protected:
    // Caller already holds mutex() (e.g. twTrackMix's clip mutators).
    void invalidatePagesInRange_nolock(uint64_t start, uint64_t end);

public:

    // Phase 4 Gap 9: Invalidation Cascade
    // Called when this component's parameters change to invalidate downstream consumers.
    // Default: no-op (components with no side effects need not override).
    // Override in components that are consumed by others (mixers, effects chains, etc.)
    // to mark dependent components for re-freezing.
    //
    // Example: If Component A outputs to Component B via a latch,
    // and A's parameters change, A->invalidateDependents() should invalidate B.
    //
    // Thread-safe: components can be called from revalidator workers or UI thread.
    // Tier 2 Enhancement #1: Selective Invalidation Cascade
    // Invalidate only affected downstream components that actually read from this component.
    // By default (no-op), but can be overridden by components that track dependencies.
    virtual void invalidateDependents() {
        // Default: no downstream invalidation (no explicit dependencies tracked)
        // Override in components that have known consumers (e.g., mixers, effects chains)
    }

    // Tier 2 Enhancement #1: Dependency tracking for selective invalidation
    // Called when this component is wired as input to another component.
    // Allows downstream component to register itself for selective invalidation.
    virtual void addDependent(std::shared_ptr<twComponent> dependent);

    // Helper: invalidate all components in a set (used by cascade)
    static void invalidateComponentSet(std::vector<std::shared_ptr<twComponent> >& components) {
        for (auto comp : components) {
            if (comp) {
                comp->invalidateAllPages();
            }
        }
    }

    // Internal: mark a specific page as frozen and valid for given aspects.
    // Called by revalidator after successful freezing.
    void setPageAsFrozen(
        uint64_t startPos,
        std::shared_ptr<twOutputPage> page,
        uint32_t aspects = twAspectAll
    );

    // Freeze component output into a page (Phase 2 - Gap 3)
    // Called by CaptureRevalidator worker threads to materialize frozen output.
    //
    // Position contract: startPos is AUTHORITATIVE for the page content — the
    // page always contains this component's output for [startPos, startPos +
    // FRAME_CAPACITY). Pages are full FRAME_CAPACITY units; callers should
    // request page-aligned positions and extract sub-ranges.
    //
    // Sequential rendering model (position is generic, state is not):
    //   Contiguous (previousPage ends exactly at startPos):
    //     restore state from previousPage → seekTo(startPos) →
    //     seekInputStreams(startPos) → renderFrames() → capture new state
    //   Discontinuity (no previousPage, or a gap/jump):
    //     reset() → seekTo(startPos) → seekInputStreams(startPos) →
    //     renderFrames() → capture state
    //   The position is set explicitly in BOTH cases (seekTo must be
    //   state-preserving — it is a position operation). Stateful components
    //   (reverbs, filters) cannot reconstruct state for an arbitrary position,
    //   so a discontinuity clears state via reset(); only a contiguous
    //   previous page allows seamless state continuation.
    //
    // Thread-safe multi-consumer reading (Phase 5 Gap 12):
    //   Returned page->pageMutex protects concurrent access from multiple readers.
    //   Readers should acquire lock when accessing internalState or during updates.
    //   Example:
    //     auto page = component->freezePage(...);
    //     {
    //       std::lock_guard<std::mutex> lock(page->pageMutex);
    //       restoreInternalState(page->internalState);  // Protected from concurrent modification
    //     }
    //
    // Args:
    //   startPos - time position this page covers (authoritative for content)
    //   inputData - pre-frozen input samples (e.g., from upstream component's frozen page)
    //   inputOffset - offset into inputData where this page starts
    //   inputLength - valid frames in inputData
    //   sampleRate - audio sample rate (for format negotiation)
    //   previousPage - prior page's snapshot (for state resumption), or nullptr if page 0
    //
    // Returns: OutputPage with frozen samples + internal state snapshot
    //
    // Default: basic implementation that calls renderFrames().
    // Override in complex components for custom freezing logic.
    virtual std::shared_ptr<twOutputPage> freezePage(
        uint64_t startPos,
        const sample_t *inputData,
        uint64_t inputOffset,
        length_t inputLength,
        int sampleRate,
        std::shared_ptr<twOutputPage> previousPage = nullptr
    );

    // Proposal 19 Phase 2a — request/ready front door over freezePage().
    // Resolves a frozen, current page for startPos. On a cache hit (frozen page
    // whose contentEpoch is current) it returns immediately; otherwise it
    // dedups: the first requester for a given (this,startPos,epoch) performs the
    // freeze while any concurrent requester for the SAME page waits for that one
    // render instead of launching a duplicate. This collapses the double-render
    // that pure (non-serial) nodes could otherwise suffer when two drivers
    // (revalidation worker, playback readahead, offline render) request the same
    // page at once — serial-cursor nodes were already serialized by
    // cursorMutex_, so this changes their timing only, not their output.
    //
    // Semantically equivalent to freezePage(): same arguments, same returned
    // page. It dispatches through the virtual freezePage(), so component-
    // specific freeze overrides (twTrackMix, twMixer, twView, plugins) are
    // honoured. Phase 2b routes the recursive input pulls through here too, at
    // which point the freeze reads only already-ready input pages.
    std::shared_ptr<twOutputPage> requestPage(
        uint64_t startPos,
        const sample_t *inputData,
        uint64_t inputOffset,
        length_t inputLength,
        int sampleRate,
        std::shared_ptr<twOutputPage> previousPage = nullptr
    );

    // Proposal 19 dataflow, stage 1 — the LEAF RENDERER: render one page of
    // this component consuming ONLY the already-frozen input pages in
    // `inputs` (see tw/graph/tw_frozen_inputs.h and "Phase 2 REVISED" in the
    // proposal). The set is installed thread-scoped for the duration of the
    // render; twStreamingLatch::copyData serves bound pages directly instead
    // of recursively pulling the producer's freezePage(). A wanted-but-unbound
    // page is recorded in inputs.misses and — in stage 1 — falls back to the
    // legacy recursive pull, so an EMPTY set makes this byte-identical to the
    // classic freeze. Later stages turn misses into "node not ready".
    //
    // `page` is caller-allocated (the dataflow scheduler owns page identity
    // and publication; this function neither consults nor updates
    // outputPages_). Serialization (usesSerialCursor/cursorMutex_, or the
    // future per-component lane) is the CALLER's responsibility, exactly as
    // for freezePage_nolock. Returns the rendered frame count.
    length_t freezePageFromInputs(
        std::shared_ptr<twOutputPage> page,
        const twFrozenInputs &inputs,
        std::shared_ptr<twOutputPage> previousPage = nullptr
    );

    // Proposal 19 Phase 1 — single-cursor serialization policy.
    // freezePage() renders by MUTATING this component's instance state
    // (reset/seekTo/restoreInternalState → pos_, file cursor, DSP memory). A
    // component that carries such a cursor cannot be frozen by two threads at
    // once: concurrent freezes of one shared component (e.g. one twWavInput
    // shared by every clip/take/track) read each other's positions and corrupt
    // output — the confirmed takes_group_broadcast flake. When this returns
    // true, freezePage() serializes on the component's own cursorMutex_ (a
    // queue-of-one owned by the component; becomes a real async actor queue in
    // Phase 2). Default false: pure nodes (mixers/routers combining already-
    // frozen input pages) stay parallel; single-cursor/stateful components
    // (sources, readers) override to true. Phase 2 inverts the mix graph so
    // pure nodes no longer double-render either, retiring this policy.
    virtual bool usesSerialCursor() const { return false; }

    // Phase 3: Preview-specific freezing — lower resolution for UI visualization
    // Renders component output at preview sample rate (e.g., 1kHz) for waveform display.
    // Returns previous page if new page not ready (fallback for non-blocking UI redraws).
    // Default: calls freezePage() with reduced sample rate.
    virtual std::shared_ptr<twOutputPage> freezePreviewPage(
        uint64_t startPos,
        length_t length,
        int previewSampleRate,  // Typically 1000 Hz for waveform visualization
        int fullSampleRate,     // Actual component sample rate for state consistency
        std::shared_ptr<twOutputPage> previousPage = nullptr
    );

private:
    // Helper for freezePage: does the actual rendering work.
    // Caller must NOT hold mutex (called outside lock to allow recursive freezePage calls).
    // Returns: number of frames actually rendered into the page.
    length_t freezePage_nolock(
        std::shared_ptr<twOutputPage> page,
        const sample_t *inputData,
        uint64_t inputOffset,
        length_t inputLength,
        std::shared_ptr<twOutputPage> previousPage
    );

protected:
    // Unified mutex protecting all component state: parameters, internal state,
    // page cache, and dependency tracking. Derived classes should:
    //   1. Override public methods to acquire lock via mutex() then call _nolock variant
    //   2. Implement _nolock() methods that assume lock is held
    //   3. Document _nolock methods with "Caller must hold mutex()"
    //
    // This pattern prevents both race conditions and deadlocks across the component hierarchy.
    // See [[unified-component-locking]] for the full strategy.
    inline std::mutex& mutex() const { return stateMutex_; }

protected:
    // Tier 2 Enhancement #1: Dependency tracking for selective invalidation
    // Components that depend on this component's output (for cascade invalidation)
    std::vector<std::shared_ptr<twComponent> > dependents_;

    // Per-component content epoch (see contentEpochNow()/bumpContentEpoch()).
    // Starts at 1 so a default-constructed page (contentEpoch 0) is stale.
    std::atomic<uint64_t> contentEpoch_{1};

private:
    mutable std::mutex stateMutex_;

    // Serializes freezePage() for single-cursor components (see
    // usesSerialCursor()). Held across the whole freeze INCLUDING the recursive
    // upstream freeze; each component takes its OWN cursorMutex_ and the graph
    // is an acyclic DAG (FreezeContext breaks cycles), so acquisition is always
    // ancestor-before-descendant — no lock-order inversion. Distinct from
    // stateMutex_/mutex() (short data-structure critical sections); ordering is
    // always cursorMutex_ (outer) → mutex() (inner).
    std::mutex cursorMutex_;

    // Page cache: maps start position → frozen output page
    std::map<uint64_t, std::shared_ptr<twOutputPage>> outputPages_;

    // Proposal 19 Phase 2a — in-flight freeze dedup (see requestPage()).
    // At most one freeze task per startPos may be active; concurrent requesters
    // for the same page wait on the entry's condition variable rather than
    // launching a duplicate render. Keyed by startPos; the entry carries the
    // epoch it was launched for so a newer-epoch request supersedes a stale one.
    struct InFlightFreeze {
        uint64_t epoch = 0;                     // contentEpoch this render targets
        bool done = false;                      // set true when page is ready
        std::shared_ptr<twOutputPage> page;     // result (nullptr if render failed)
        std::mutex m;                           // guards done/page for the CV
        std::condition_variable cv;
    };
    std::map<uint64_t, std::shared_ptr<InFlightFreeze>> inflight_;
    std::mutex inflightMutex_;                   // guards inflight_ only

private:
    std::vector<twFormat> committedIn_;
    std::vector<twFormat> committedOut_;
};

#include "tw/graph/tw303aenv.h"

#endif

