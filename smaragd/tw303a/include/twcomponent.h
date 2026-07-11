
#ifndef _TWCOMPONENT_H_
#define _TWCOMPONENT_H_

#include <qobject.h>
#include <memory>
#include <mutex>
#include <map>
#include <atomic>

#include "exc.h"
#include "twformat.h"
#include "tw_output_page.h"
#include "tw_freeze_context.h"

// Forward declaration to avoid circular includes
class IOVector;

enum class ComponentState {
    ACTIVE,    // Normal operation
    ZOMBIE,    // Tearing down, outputs silence
    DELETED    // Memory freed (unused in this implementation)
};

#define DTOR_DEL(x) {if((x)) {delete (x); (x) = NULL; }}

#undef DEBUG_COMPONENT

typedef signed long long length_t;
typedef signed short idx_t;
typedef float sample_t;
#define SAMPLE_NORM_MIN (-1.0)
#define SAMPLE_NORM_MAX (1.0)
typedef unsigned long long offset_t;

// The type used for preview datas.
typedef signed char previewPart_t;
typedef struct {
    previewPart_t min, max;
} preview_t;


class tw303aEnvironment;
class twLatchOutput;
class twComponent;

class twLatch
{
private:
    twLatch();
    twComponent & component;
    idx_t idx;
protected:
    QList<twLatchOutput*> outputList;
    // the current top offset of the Latch
    offset_t offset;
    
public:
    twLatch( twComponent & component0, idx_t idx0 );

    virtual offset_t getOffset () { return offset; }
    virtual void resetOffset() { offset = 0; }  // Reset for capture rebuilds
    virtual ~ twLatch();

    inline twComponent & getComponent() { return component; }
    inline idx_t getIndex() { return idx; }

    // Native format of the data this latch produces. The default reports the
    // canonical mono-Float32 format at the environment sample rate, so every
    // existing latch tells the truth without any change. Producers that emit a
    // different format (foreign rate, Int16 PCM, …) override this.
    virtual twFormat getFormat() const;

    virtual twLatchOutput * addOutput();
    virtual int deleteOutput( twLatchOutput * latchOutput );

};

class twLatchOutput
{
private:
    twLatch & parentLatch;
protected:
    offset_t offset;
public:
    twLatchOutput (twLatch & latch)
        : parentLatch(latch) { offset = latch.getOffset(); }
    inline twLatch & getParentLatch () { return parentLatch; }

    // The consumer's single entry point for "what am I about to read?".
    // Delegates to the producing latch.
    twFormat getFormat() const { return parentLatch.getFormat(); }

    virtual length_t readData( sample_t *, length_t  ) {
        throw excStandard( "twLatchOutput(): Tried to read data from the Latch itselves." );
    };
};

class twStreamingLatch
    : public twLatch
{
private:
    void init( length_t bufSize0 );
protected:
    sample_t * pBuffer;
    // defaults to 16384
    length_t bufSize;
    // the current position (equivalent to offset in parent class)
    offset_t bufPos;

    // Phase 2: freezePage() state tracking
    offset_t currentPos_;                              // Current playback position for snapshots
    std::shared_ptr<twOutputPage> previousPage_;      // Cached page for state continuity
    int sampleRate_;                                   // Project sample rate for freezePage()

public:
    twStreamingLatch( twComponent & comp, idx_t idx0, length_t bufSize0 );
    virtual ~ twStreamingLatch ();

    length_t copyData( offset_t startOffset, sample_t *pDest, length_t maxLength );

    // Phase 2: Reset cached page on seek (breaks state chain)
    void resetPageCache() { previousPage_ = nullptr; }

    static const int bufSizeDefault;
};

class twLatchStreamingOutput
    : public twLatchOutput
{
private:
protected:
public:
    twLatchStreamingOutput (twStreamingLatch & latch)
        : twLatchOutput((twLatch &) latch) {}
    
    length_t readStreamingData( sample_t * pDest, length_t maxLength );
    inline length_t readData( sample_t * pDest, length_t maxLength )
        { return readStreamingData( pDest, maxLength ); }

    // Read up to maxFrames frames of the latch's NATIVE format (getFormat())
    // into dest, with no conversion; dest must hold
    // maxFrames * getFormat().bytesPerFrame() bytes. Returns frames read. The
    // sink decides whether to convert (see twConvertFrames). Current latches
    // store canonical mono Float32, so this presently yields the same bytes as
    // readStreamingData — a producer storing another representation overrides it.
    length_t readRaw( void * dest, length_t maxFrames );
    
    inline twStreamingLatch & getParentStreamingLatch()
        { return (twStreamingLatch &) getParentLatch(); }
};

class twComponent
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
    virtual void onDependencyTeardown(twComponent* dep);

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
    virtual void addDependent(twComponent* dependent);

    // Helper: invalidate all components in a set (used by cascade)
    static void invalidateComponentSet(std::vector<twComponent*>& components) {
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
    // Sequential rendering model:
    //   Page 0: reset() → renderFrames() → capture state → return page
    //   Page 1: restore state from page 0 → renderFrames() → capture new state → return page
    //   Page N: ...
    //
    // This ensures continuous audio across page boundaries with state resumption.
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
    //   startPos - time position this page covers (for logging/debugging)
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
    std::vector<twComponent*> dependents_;

private:
    mutable std::mutex stateMutex_;

    // Page cache: maps start position → frozen output page
    std::map<uint64_t, std::shared_ptr<twOutputPage>> outputPages_;

private:
    std::vector<twFormat> committedIn_;
    std::vector<twFormat> committedOut_;
};

#include "tw303aenv.h"

#endif

