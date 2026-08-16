
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
#include <string>
#include <vector>

#include "tw/core/twtypes.h"
#include "tw/graph/twlatch.h"
#include "tw/core/exc.h"
#include "tw/core/twformat.h"
#include "tw/pages/tw_output_page.h"
#include "tw/graph/tw_freeze_context.h"
#include "tw/graph/tw_page_plan.h"

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

    // The EXTERNAL seek entry point — and a regression detector.
    //
    // A component's play cursor is written from two places that take DIFFERENT
    // locks: a page freeze serializes on cursorMutex_ and repositions the
    // component itself (reset() + seekTo(page->startPosition)) just before
    // renderFrames(); an outside seek takes only mutex(). An outside seek
    // landing in that window rewrites the cursor the in-flight render is about
    // to read, so the whole 65536-frame page comes out as the audio of the SEEK
    // TARGET while being cached under, and served for, its original startPos.
    // That is the wrong-position playback bug, and the cure was to delete the
    // external cascades (play start, record start, capture rebuild, plugin
    // chain producers) rather than to add a lock.
    //
    // seek() is what everything OUTSIDE the freeze machinery calls now. It is
    // non-virtual on purpose: it asserts the window is clear and then dispatches
    // to the virtual seekTo(). Freeze-internal repositioning (freezePage_nolock,
    // twTrackMix::seekTo's clip walk, twView::seekTo, a chain seeking its own
    // taps) keeps calling seekTo() directly — it is already inside the freeze
    // that owns the cursor.
    //
    // NDEBUG is stripped from RelWithDebInfo in this repo, so the assert is live
    // across the whole suite: any surviving or newly added external cascade
    // announces itself instead of degrading into an occasional wrong page.
    int seek( offset_t );

    // RAII marker for "a freeze render is running on this component". Public
    // because a subclass that overrides the WHOLE freeze rather than
    // freezePage_nolock (twTrackMix) has to mark its own window.
    class FreezeInFlight {
    public:
        explicit FreezeInFlight( twComponent &c ) : c_( c )
        { c_.freezeInFlight_.fetch_add( 1, std::memory_order_acq_rel ); }
        ~FreezeInFlight()
        { c_.freezeInFlight_.fetch_sub( 1, std::memory_order_acq_rel ); }
        FreezeInFlight( const FreezeInFlight & ) = delete;
        FreezeInFlight &operator=( const FreezeInFlight & ) = delete;
    private:
        twComponent &c_;
    };

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

    // WIDE rendering: fill EVERY channel of `page` in ONE pass (proposal 36
    // §4.3, milestone B2). Called by freezePage_nolock instead of renderFrames()
    // exactly when the page in hand is wider than one channel.
    //
    // WHY THIS IS NOT A PER-CHANNEL LOOP OVER renderFrames(). v2 of the proposal
    // specified that loop; it is wrong and must never be built.
    // twSampleReader::calcOutputTo — the very component the loop was meant to
    // serve — ADVANCES A CURSOR (pos_ += dest.length()), while freezePage_nolock
    // seeks ONCE before rendering. A loop would render channel 0, advance the
    // cursor a whole page, and fill channel 1 with the NEXT PAGE'S AUDIO — the
    // "coherent page displaced by one page" bug this repo has already bled for.
    // The same applies to every input-side plug cursor, and internalState is
    // captured once after rendering, so a state chain would be meaningful for
    // channel 0 only.
    //
    // So a wide component seeks once, fills every channel in that one pass, and
    // advances its cursor once. `page.startPosition` is authoritative for the
    // content (freezePage_nolock has already reset/restored state and seeked);
    // `frames` bounds the write, and is never more than page.channelFrames().
    //
    // Returns frames rendered PER CHANNEL (every channel of a page covers the
    // same time range), like renderFrames().
    //
    // The base implementation REFUSES: it reports, fills silence and returns 0.
    // It never silently renders something plausible, and it is deliberately NOT
    // a Q_ASSERT — §7 trap 9: Q_ASSERT_X is compiled out of the build everyone
    // runs (Qt defines QT_NO_DEBUG even though CMake strips -DNDEBUG), so an
    // assert here would vanish and leave a silent wrong render.
    virtual length_t renderPageWide( twOutputPage &page, length_t frames,
                                     const sample_t *input, length_t inputLength );

    // PROPOSAL 36 §4.4 RULE (2), added by B4 — "a wide component reads its
    // bound input PAGES directly and picks channels itself".
    //
    // Returns the page of input plug `plugIdx`'s producer covering pageStart,
    // through exactly the seam the mono plug pull uses: a page bound by the
    // dataflow scheduler is served with no rendering, otherwise the legacy
    // recursive pull runs, and either way THIS reader's page-chain hint is
    // maintained so a stateful producer still continues across page boundaries.
    // nullptr = unwired input, or the producer could not materialise the page;
    // the caller renders silence for that input.
    //
    // The plug is SNAPSHOTTED under mutex() and the lock RELEASED before the
    // pull — the rule twPluginInsert::pullUpstreamPage already had to obey
    // (plugins/CONTRACT.md invariant 13): a component's own mutex must never be
    // held across a call into a producer.
    //
    // The CALLER picks the channel, with twPageClampChannel. This deliberately
    // does not, because the page's width is a fact and the consumer's is only a
    // promise (§4.4), and because a downmix policy is never a seam's business.
    std::shared_ptr<twOutputPage> fetchInputPage( idx_t plugIdx, offset_t pageStart );

    // How many times the base renderPageWide() has refused, process-wide. The
    // refusal logs once (a freeze loop would otherwise emit thousands of
    // records); this counter is what a test can assert on. AC B2.3.
    static uint64_t wideRenderRefusals();

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

    // --- Page width (proposal 36 §4.2, milestone B2) ----------------------
    // How many CHANNELS this component's frozen page carries. AUTHORITATIVE for
    // page width from B2 on: every page this component allocates is built at
    // this width, and freezePage_nolock dispatches on the width of the page in
    // its hand (§4.4 — a declared width is a promise about future pages; the
    // page you hold is a fact).
    //
    // Deliberately NOT getNOutputs(), and the two must never be merged (§7 trap
    // 8). getNOutputs() is the PATCH-BAY PORT COUNT and already means three
    // different things in three classes: twRewire's N plugs are buses,
    // twSampleReader's N outputs are channels, twWavInput returns a hardcoded 4
    // with one latch built. Conflating them is how this stays broken.
    //
    // Default 1 ⇒ every existing component is unchanged and correct, and a
    // width-1 page takes byte-for-byte today's render path.
    //
    // A component that returns > 1 MUST override renderPageWide(); the base
    // implementation refuses (see below).
    virtual idx_t getOutputChannels() const { return 1; }

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
    //   Page 0 [0..FRAME_CAPACITY]: render from reset, save delay line state → OutputPage.internalState
    //   Page 1 [FRAME_CAPACITY..2*FRAME_CAPACITY]: restore delay line state, render, save new state
    //   (FRAME_CAPACITY, not PAGE_SIZE: a page's extent in POSITIONS is 65536
    //    frames; PAGE_SIZE is its 262144 BYTES. Saying PAGE_SIZE here is how the
    //    releaseOldPages units bug got written — see invariant 8 in CONTRACT.md.)
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
        offset_t startPos,
        uint32_t aspectsMask = twAspectAll
    );

    // Lock-free page lookup (audio thread only).
    // Returns existing page if found, nullptr if not. Never allocates.
    // Safe for real-time threads because it uses only atomic reads.
    std::shared_ptr<twOutputPage> getPageIfExists(offset_t startPos);

    // Release pages outside of a time retention window (memory management).
    // Frees pages whose [startPos, startPos + FRAME_CAPACITY) range ends before
    // keepAfterPos. keepAfterPos and startPos are both FRAME positions; the
    // page's own extent is FRAME_CAPACITY frames, NOT PAGE_SIZE (which is the
    // page's size in BYTES). See the note on the definition — this comparison
    // was frames-against-bytes until proposal 36 B1a.
    //
    // NOTE (B1a): nothing in the tree calls this. Page caches are pruned only by
    // invalidation and by component teardown, so a long session's outputPages_
    // maps grow without bound. Retiring or wiring it is proposal 36 B9's call;
    // the arithmetic is fixed here because widening the page would have
    // multiplied the error the day it did get wired.
    void releaseOldPages(offset_t keepAfterPos);

    // --- Page-memory accounting (proposal 36 B1a) ---------------------------
    //
    // The GLOBAL figures live in tw::pages::PageAccounting and are exact by
    // construction (they ride the page's own lifetime). These add the PER
    // COMPONENT breakdown, which the global counters cannot give: they answer
    // "which components are holding the resident pages", which is the question
    // a memory regression is actually about.
    //
    // A component registers itself in a process-wide registry at construction.
    // The registry is intentionally NOT an owner — it holds raw pointers and the
    // destructor removes them — so it cannot keep a component alive.

    struct PageStats {
        size_t pages = 0;   // pages in THIS component's outputPages_
        size_t bytes = 0;   // their sample bytes
        size_t frozen = 0;  // of those, how many carry validAspects != 0
    };

    // This component's own cache. Takes mutex(); never call from the RT thread.
    PageStats pageStats() const;

    // The same measurement with a TRY-lock, ADDING into `out` and reporting
    // whether it got the lock at all. The registry walkers below use it: they
    // hold the registry lock for the whole walk (which is what keeps the raw
    // pointers alive), so waiting on a component mutex there would be a
    // lock-order inversion against a thread destroying a component.
    bool pageStatsTry( PageStats &out ) const;

    // Sum over every live component. Note this is NOT the same number as
    // tw::pages::PageAccounting::global(): a page bound into a scheduler node,
    // held by an audio callback, or chained as a stalePredecessor is alive but
    // may no longer be in any component's map. The DIFFERENCE between the two is
    // the interesting figure, and reportPageMemory() prints both.
    static PageStats componentPageStats();

    // TW_LOG one summary line plus one line per component TYPE holding pages,
    // heaviest first. `label` names the moment ("after render", "at exit").
    // RETURNS the same text it logged, so a caller that also wants to print it
    // (the report-page-memory test hook does, into the harness's own stdout)
    // gets ONE walk of the registry and cannot print figures that disagree with
    // the ones in the log.
    static std::string reportPageMemory( const char *label );

    // The breakdown as text without logging it. One record per line.
    static std::string describePageMemory( const char *label );

    // Get all cached pages in a time range (for iteration, cleanup, debugging).
    std::vector<std::shared_ptr<twOutputPage>> getPagesInRange(
        offset_t startPos,
        offset_t endPos
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
    virtual void invalidatePagesInRange(offset_t start, offset_t end);

    // Proposal 27 M1 — the readiness gate. While false, freezePage() produces
    // an explicit SILENT page (validFrames 0, buffer zeroed) instead of
    // rendering: valid and current, so consumers never block, but NOT a
    // latch — whoever flips this back to true must also invalidate the
    // component's pages (bumpContentEpoch()/invalidatePagesInRange()), after
    // which the silent pages read stale and re-freeze with real audio.
    // Convergence is purely epoch-driven and order-independent. Settable from
    // any thread; read lock-free in the freeze path. Default: ready.
    void setRenderReady(bool ready) {
        renderReady_.store(ready, std::memory_order_release);
    }
    bool isRenderReady() const {
        return renderReady_.load(std::memory_order_acquire);
    }

protected:
    // Caller already holds mutex() (e.g. twTrackMix's clip mutators).
    void invalidatePagesInRange_nolock(offset_t start, offset_t end);

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
        offset_t startPos,
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
        offset_t startPos,
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

    // Proposal 19 dataflow, stage 2 — planned render through the VIRTUAL
    // freeze path. Installs `inputs` thread-scoped (self = this) and calls
    // freezePage(startPos, …), so component-specific freeze overrides
    // (twTrackMix's clip rendering, twView forwarding) are honoured: every
    // input the render reaches — via the copyData seam OR a direct child
    // freezePage() call (the twTrackMix clip path) — is served from the bound
    // set; unbound wants are recorded in inputs.misses and (stage 2) fall
    // back to the legacy pull. Unlike freezePageFromInputs (the raw base-body
    // leaf), publication follows the component's own freezePage semantics.
    std::shared_ptr<twOutputPage> freezePageWithInputs(
        offset_t startPos,
        const twFrozenInputs &inputs,
        std::shared_ptr<twOutputPage> previousPage = nullptr
    );

    // Proposal 19 dataflow, stage 2 — the PLANNER hook: capture the
    // structural snapshot of the node (this, pageStart): which producer pages
    // a render of [pageStart, pageStart+FRAME_CAPACITY) will consume, plus
    // this component's content epoch at plan time (the scheduler's
    // verify-at-publish reference). STRUCTURAL walk only — must not render,
    // and must capture any lazily-resolved structure (readers, clip windows)
    // through the same single-resolution path the render itself uses
    // (Inv-1's resolveClip), so plan and render cannot disagree.
    //
    // Base implementation: one grid-aligned dep per streaming input plug
    // (mixer/rewire/plugin-chain shape — copyData reads the producer at the
    // consumer's own timeline positions). Sources with no inputs plan empty.
    // twTrackMix overrides with its clip enumeration.
    virtual twPagePlan planPage( offset_t pageStart );

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
        offset_t startPos,
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

    // Readiness gate (see setRenderReady()); true = render normally.
    std::atomic<bool> renderReady_{true};

    // How many freeze renders are inside their reset/seek/render window on this
    // component right now (see seek() and FreezeInFlight). Only ever read by
    // the seek() assert — it is a detector, not a lock, and nothing waits on it.
    std::atomic<int> freezeInFlight_{0};

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
    std::map<offset_t, std::shared_ptr<twOutputPage>> outputPages_;

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
    std::map<offset_t, std::shared_ptr<InFlightFreeze>> inflight_;
    std::mutex inflightMutex_;                   // guards inflight_ only

private:
    std::vector<twFormat> committedIn_;
    std::vector<twFormat> committedOut_;
};

#include "tw/graph/tw303aenv.h"

#endif

