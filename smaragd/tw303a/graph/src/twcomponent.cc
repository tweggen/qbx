#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <string.h>

#include <algorithm>
#include <iostream>
#include <iterator>
#include <vector>

#include "tw/core/twsyslog.h"

#include "tw/graph/twcomponent.h"
#include "tw/graph/tw303aenv.h"
#include "tw/pages/io_vector.h"

#define DEBUG_COMPONENT

offset_t twComponent::tellPos() const
{
    return 0;
}

// --- Format negotiation defaults -----------------------------------------

twFormatCaps twComponent::getOutputCaps( idx_t /*idx*/ ) const
{
    // The engine exchanges mono Float32; `rates` left empty == any rate (the
    // negotiator intersects it with the candidate set D).
    twFormatCaps c;
    c.types        = { twSampleType::Float32 };
    c.channelCounts = { 1 };
    return c;
}

twFormatCaps twComponent::getInputCaps( idx_t /*idx*/ ) const
{
    twFormatCaps c;
    c.types        = { twSampleType::Float32 };
    c.channelCounts = { 1 };
    return c;
}

namespace {

std::vector<std::uint32_t> sortedUnique( std::vector<std::uint32_t> v )
{
    std::sort( v.begin(), v.end() );
    v.erase( std::unique( v.begin(), v.end() ), v.end() );
    return v;
}

std::vector<std::uint32_t> intersectRates( const std::vector<std::uint32_t> &a,
                                           const std::vector<std::uint32_t> &b )
{
    std::vector<std::uint32_t> out;
    std::set_intersection( a.begin(), a.end(), b.begin(), b.end(),
                           std::back_inserter( out ) );
    return out;
}

}  // namespace

bool twComponent::commitFormats( const twFormat *in,  idx_t nIn,
                                 const twFormat *out, idx_t nOut )
{
    committedIn_.assign( in, in + ( nIn > 0 ? nIn : 0 ) );
    committedOut_.assign( out, out + ( nOut > 0 ? nOut : 0 ) );
    return true;
}

bool twComponent::narrowCaps( twPortDomains &ports ) const
{
    // Default coupling: this node runs every port at one common rate (it
    // neither resamples nor rate-mixes). Intersect all port rate-domains and
    // write the result back to each. Monotone: the intersection is a subset of
    // every input domain, so it only ever removes candidates.
    std::vector<std::vector<std::uint32_t> *> r;
    for( auto &c : ports.in )  r.push_back( &c.rates );
    for( auto &c : ports.out ) r.push_back( &c.rates );
    if( r.empty() ) return false;

    std::vector<std::uint32_t> common = sortedUnique( *r[0] );
    for( std::size_t i = 1; i < r.size(); ++i )
        common = intersectRates( common, sortedUnique( *r[i] ) );

    bool changed = false;
    for( auto *pr : r ) {
        if( sortedUnique( *pr ) != common ) {
            *pr = common;
            changed = true;
        }
    }
    return changed;
}

bool twComponent::isSeekable() const
{
    return false;
}

int twComponent::seekTo( offset_t offset )
{
    return -1;  // Base implementation: component doesn't support seeking
}

void twComponent::resetAllLatches()
{
    // Reset all output latches to offset 0, ensuring deterministic capture rebuilds.
    // This fixes the nil-operation bug where rebuilding without actual changes
    // would produce different audio due to persisted latch offsets.
    for( idx_t i = 0; i < getNOutputs(); i++ ) {
        if( pOutputLatches_.size() > i && pOutputLatches_[i] ) {
            pOutputLatches_[i]->resetOffset();
        }
    }
}

void twComponent::seekInputStreams( offset_t pos )
{
    // Jump this component's input readers to pos. Each twLatchOutput tracks its
    // own read offset on the producer's timeline; after a seek the reader must
    // move too, or readStreamingData() keeps serving content for the old
    // position. Brief lock: pInputPlugs_ may be rewired from the UI thread.
    std::lock_guard<std::mutex> lock(mutex());
    for( auto &plug : pInputPlugs_ ) {
        if( plug ) plug->seekStream( pos );
    }
}

void twComponent::allocPlugs()
{
    // Phase 1: Convert to shared_ptr vectors for thread-safe lifetime management
    pInputPlugs_.clear();
    pOutputLatches_.clear();
    pInputPlugs_.resize(getNInputs());      // Allocate slots (all nullptr initially)
    pOutputLatches_.resize(getNOutputs());  // Allocate slots (all nullptr initially)
}

void twComponent::init()
{
#ifdef DEBUG_COMPONENT
    fprintf( stderr, "twComponent::init(): entered." );
#endif
    
    allocPlugs();
    createOutputLatches();
    
#ifdef DEBUG_COMPONENT
    fprintf( stderr, "twComponent::init(): leaving." );
#endif
}

/**
 *	@method twComponent.linkOutput
 *		Connect to a components output.
 *	@synopsis twLatchOutput *twComponent::linkOutput(
 *      idx_t idx )
 *	@param </i><tt>idx_t</tt><i>idx
 *		The index of the output to connect to.
 *	@desc
 *		Every component has an arbitrary number of outputs.
 *		An arbitrary number of components may connect to this
 *		output by using the twLatchOutput returned by this function.
 *	@returns
 *		A pointer to the twLatchObject, which can be used to retrieve data.
 *		or <br>NULL, if an error occured.
 */
twLatchOutput * twComponent::linkOutput( idx_t idx )
{
    if( idx < 0 || idx >= (idx_t)pOutputLatches_.size() || !pOutputLatches_[idx] ) {
        return nullptr;
    }
    return pOutputLatches_[idx]->addOutput();
}

void twComponent::setInput( idx_t idx, twLatchOutput *newOutput )
{
    // Guard: bounds check to prevent array overflow
    if( idx < 0 || idx >= getNInputs() ) {
        fprintf(stderr, "twComponent::setInput(): idx=%d out of range [0, %d)\n", (int)idx, (int)getNInputs());
        return;
    }

    // Guard: vector must be allocated
    if( idx >= (idx_t)pInputPlugs_.size() ) {
        fprintf(stderr, "twComponent::setInput(): pInputPlugs_ not allocated\n");
        return;
    }

    // Avoid unnecessary reconnection: if input is already set to the same output, do nothing
    if( pInputPlugs_[idx].get() == newOutput ) {
        return;
    }

    if( pInputPlugs_[idx] ) {
        twLatchOutput* oldPlug = pInputPlugs_[idx].get();
        auto parentLatch = &(oldPlug->getParentLatch());
        if(parentLatch==nullptr)
        {
            fprintf(stderr, "twComponent::setInput(): pInputPlugs_[idx]->getParentLatch() == null\n");
            return;
        }
        oldPlug->getParentLatch().deleteOutput(oldPlug);
        pInputPlugs_[idx] = nullptr;
        if( !newOutput ) --inputsSet_;
    } else {
        if( newOutput ) ++inputsSet_;
    }
    // Create a no-op shared_ptr wrapper that doesn't take ownership
    // (ownership is managed by the output latch)
    if( newOutput ) {
        pInputPlugs_[idx] = std::shared_ptr<twLatchOutput>(newOutput, [](twLatchOutput*){});

        // Auto-detect and track parent component for safe teardown
        // Extract parent from the latch that owns this output
        twLatch& parentLatch = newOutput->getParentLatch();
        std::shared_ptr<twComponent> parentComp = parentLatch.getComponent();

        // Track parent using a no-op shared_ptr (lifetime owned by latch, not by us)
        // This enables child->teardown() to call parent->removeInput(idx) safely
        parentComponent_ = parentComp;
        myInputIndex_ = idx;
    } else {
        pInputPlugs_[idx] = nullptr;
        // Clear parent tracking when input is disconnected
        parentComponent_.reset();
        myInputIndex_ = -1;
    }

    // Rewiring changes what THIS component produces; its cached/held pages
    // are stale. Scoped (proposal 15): callers that rewire mid-graph are
    // responsible for the downstream path (SObject::invalidateRenderPath()).
    bumpContentEpoch();
}

/**
 * Use of this method is dangerous. However, it allows nice things.
 * take care of dangling references.
 */
twLatchOutput *twComponent::getInputPlug( idx_t idx ) const
{
    if( idx < 0 || idx >= getNInputs() ) return nullptr;
    if( idx >= (idx_t)pInputPlugs_.size() ) return nullptr;
    return pInputPlugs_[idx].get();
} 

/**
 * Initialize everything needed for playback operation.
 * Subclasses should call their parent classes' implementation.
 */
int twComponent::doInitOperation( int /* initId */ ) 
{
    return 0;
}

int twComponent::initOperation( int initId )
{
    int oldId = currentOperation_;
    if( initId == oldId ) return 0;
    currentOperation_ = initId;
    int res = doInitOperation( initId );
    if( res<0 ) {
        currentOperation_ = oldId;
        return res;
    }
    return 0;
}

twComponent::~twComponent ()
{
    // Phase 1: Vectors handle RAII cleanup automatically
    // shared_ptr destructs when removed from vector or when vector is destroyed
}

/**
 *	@method twComponent.twComponent
 *		Constructor for twComponent object.
 *	@synopsis twComponent::twComponent()
 *	@desc
 *		Every part of the synthesis network is a component. In C++, all components
 *		are derived from the twComponent class.
 *		Each twComponent object has several inputs and several outputs.
 *		Every input is a pointer to a twLatchOutput object, every output is a
 *		pointer to a twLatch object.
 *		<br>Each component will allocate pointers for the input/output table,
 *		any derived class will have to initialize all of the objects,
 *		as we do not know, which kind of latches will be used.
 *	@returns
 *		(nothing, constructor)
 */
twComponent::twComponent( tw303aEnvironment &env0 )
    : currentOperation_( -1 ),
      inputsSet_(0),
      env( env0 )
      // Phase 1: vectors initialize empty by default
{
}

// ============================================================================
// Output Page Caching Implementation (Phase 1 - Component-Level Freezing)
// ============================================================================

std::shared_ptr<twOutputPage> twComponent::getPageIfExists(uint64_t startPos)
{
    // Truly lock-free lookup for audio thread (real-time constraint).
    // Attempts non-blocking lock; returns nullptr if can't acquire immediately.
    // This prevents the audio thread from ever blocking on the page cache.

    // Try to acquire lock without blocking
    std::unique_lock<std::mutex> lock(mutex(), std::try_to_lock);
    if (!lock.owns_lock()) {
        // Couldn't acquire lock; another thread (likely read-ahead) is modifying the cache.
        // Return nullptr to indicate "not available right now".
        // Audio thread will output silence this frame and try again next frame.
        return nullptr;
    }

    // Lock acquired; safe to read the cache
    auto it = outputPages_.find(startPos);
    if (it != outputPages_.end()) {
        return it->second;
    }
    return nullptr;
}

std::shared_ptr<twOutputPage> twComponent::getOrAllocatePage(
    uint64_t startPos,
    uint32_t aspectsMask
)
{
    std::lock_guard<std::mutex> lock(mutex());

    auto it = outputPages_.find(startPos);
    if (it != outputPages_.end()) {
        // Page already exists
        auto& page = it->second;
        // Return it whether frozen or not; consumers check validAspects
        return page;
    }

    // Allocate new page
    auto page = std::make_shared<twOutputPage>();
    page->startPosition = startPos;
    page->createdAt = std::chrono::steady_clock::now();
    outputPages_[startPos] = page;

    // Schedule async freezing (handled by CaptureRevalidator)
    // For now, just allocate; page will be filled by freezing thread

    return page;
}

void twComponent::releaseOldPages(uint64_t keepAfterPos)
{
    std::lock_guard<std::mutex> lock(mutex());

    for (auto it = outputPages_.begin(); it != outputPages_.end(); ) {
        if (it->first + twOutputPage::PAGE_SIZE < keepAfterPos) {
            // Page is entirely before keepAfterPos; release it
            it = outputPages_.erase(it);
        } else {
            ++it;
        }
    }
}

std::vector<std::shared_ptr<twOutputPage>> twComponent::getPagesInRange(
    uint64_t startPos,
    uint64_t endPos
) const
{
    std::lock_guard<std::mutex> lock(mutex());
    std::vector<std::shared_ptr<twOutputPage>> result;

    for (const auto& [pos, page] : outputPages_) {
        if (pos >= startPos && pos < endPos) {
            result.push_back(page);
        }
    }

    return result;
}

void twComponent::invalidateAllPages()
{
    {
        std::lock_guard<std::mutex> lock(mutex());

        for (auto& [pos, page] : outputPages_) {
            page->validAspects = 0;  // Mark all aspects as stale
            page->generation++;       // Increment generation so audio threads detect invalidation
        }

        // Phase 4 Gap 9 + Tier 2 #1: Invalidation Cascade
        // Trigger downstream invalidation so dependent components also re-freeze
        // Tier 2 #1: Selective cascade - only invalidate tracked dependencies
        for (auto dependent : dependents_) {
            if (dependent) {
                dependent->invalidateAllPages();  // Recursive cascade
            }
        }
    }

    // Also call hook for subclass customization
    invalidateDependents();
}

void twComponent::setPageAsFrozen(
    uint64_t startPos,
    std::shared_ptr<twOutputPage> page,
    uint32_t aspects
)
{
    std::lock_guard<std::mutex> lock(mutex());

    auto it = outputPages_.find(startPos);
    if (it != outputPages_.end()) {
        // Update existing page reference
        it->second = page;
        page->validAspects |= aspects;  // Mark aspects as complete
    } else {
        // Insert new page
        page->validAspects |= aspects;
        outputPages_[startPos] = page;
    }
}

// ============================================================================
// Sequential Freezing Implementation (Phase 2 - Gap 3)
// ============================================================================

std::shared_ptr<twOutputPage> twComponent::freezePage(
    uint64_t startPos,
    const sample_t *inputData,
    uint64_t inputOffset,
    length_t inputLength,
    int sampleRate,
    std::shared_ptr<twOutputPage> previousPage
)
{
    // CRITICAL: Do NOT hold mutex during renderFrames, which calls calcOutputTo,
    // which may recursively call freezePage on upstream components.
    // Recursive lock attempts will deadlock (mutex is not recursive).

    // Step 1: Check cache and allocate placeholder under lock.
    // A cached page is only served if it is frozen AND from this component's
    // current content epoch — a page rendered before the last edit affecting
    // this component no longer matches the graph. Stale pages are replaced
    // with a fresh placeholder (not re-rendered in place), so consumers still
    // holding the old shared_ptr keep reading a consistent — if outdated —
    // buffer until they notice the epoch change.
    const uint64_t epochNow = contentEpochNow();
    std::shared_ptr<twOutputPage> page;
    bool needsRendering = false;

    {
        std::lock_guard<std::mutex> lock(mutex());
        auto it = outputPages_.find(startPos);
        if (it != outputPages_.end() &&
            it->second->contentEpoch.load() >= epochNow) {
            // Page exists and is current (frozen, or a placeholder another
            // thread is rendering right now)
            page = it->second;
            needsRendering = (page->validAspects == 0);
        } else if (it != outputPages_.end() && it->second->validAspects == 0) {
            // Stale-epoch placeholder: another thread is (or was) rendering it
            // for an older epoch. Reuse it rather than racing a second render
            // into the same position; it will be re-frozen once that render
            // finishes and a consumer requests the page again.
            page = it->second;
            needsRendering = true;
        } else {
            // Page doesn't exist, or exists but predates the last edit;
            // allocate a fresh placeholder (claims the render for this thread)
            page = std::make_shared<twOutputPage>();
            page->startPosition = startPos;
            page->createdAt = std::chrono::steady_clock::now();
            page->validAspects = 0;  // Not yet frozen
            if (it != outputPages_.end()) {
                // Replacing a stale-frozen page: keep it reachable while this
                // placeholder renders — playback serves it as a stale-but-
                // consistent fallback instead of silence (proposal 16).
                std::atomic_store(&page->stalePredecessor, it->second);
            }
            outputPages_[startPos] = page;
            needsRendering = true;
        }
    } // Lock released - safe for recursive freezePage calls

    // Step 2: If page was already cached AND frozen, return it
    if (!needsRendering) {
        return page;
    }

    // Step 3: Render the page (OUTSIDE mutex to allow recursive calls)
    // This is the lengthy operation that must not hold the lock
    page->validFrames = freezePage_nolock(
        page,
        inputData,
        inputOffset,
        inputLength,
        previousPage
    );

    // Step 4: Mark page as frozen and valid under lock
    // A page is "valid" if it has been frozen (rendering attempted), even if validFrames == 0.
    // Rendering can return 0 frames for legitimate reasons (pass-through with silent input, etc.)
    // The audio thread uses validAspects to know if freezing is complete; if validFrames == 0
    // but validAspects != 0, that tells the audio thread "we tried, and this was the result".
    {
        std::lock_guard<std::mutex> lock(mutex());
        // Stamp with the epoch read BEFORE rendering: if an edit landed while we
        // rendered, the page is already stale and will be re-frozen on demand.
        page->contentEpoch.store(epochNow);
        page->validAspects = twAspectAll;  // Mark as frozen, even if 0 frames resulted
        // The frozen page supersedes its pre-edit predecessor; release it
        std::atomic_store(&page->stalePredecessor,
                          std::shared_ptr<twOutputPage>{});
    }

    return page;
}

// Caller must NOT hold mutex. This function does all work outside the lock.
// Caller: freezePage (holds lock briefly for cache check only)
length_t twComponent::freezePage_nolock(
    std::shared_ptr<twOutputPage> page,
    const sample_t *inputData,
    uint64_t inputOffset,
    length_t inputLength,
    std::shared_ptr<twOutputPage> previousPage
)
{
    // CRITICAL: Install a FreezeContext for cycle detection.
    // Marks this component as "being frozen" on this thread.
    // If calcOutputTo → readStreamingData → copyData tries to call freezePage
    // on this same component, FreezeContext::current() detects the cycle and
    // returns silence instead of recursing.
    FreezeContext freezeCtx(shared_from_this());

    // Initialize position and state. page->startPosition is authoritative for
    // the content; the component's current cursor is never trusted. Position is
    // generic (every seekable component can jump to startPos), internal DSP
    // state is not (a reverb tail cannot be reconstructed for an arbitrary
    // position) — so the two are handled separately:
    //
    // Contiguous case: previousPage ends exactly where this page begins, so its
    // captured internal state (reverb tails, filter memory) is the correct
    // continuation — restore it, keep state.
    //
    // Discontinuity (no previous page, or a gap/jump): state cannot be
    // reconstructed, so clear it with reset().
    //
    // In BOTH cases the position is then set explicitly: seekTo() for the
    // component's own cursor (must be state-preserving — it is a position
    // operation), seekInputStreams() for its input-side latch readers.
    // Components that cannot seek (free-running sources) simply continue from
    // their restored/reset state.
    const uint64_t startPos = page->startPosition;
    // A predecessor from an older content epoch carries DSP state computed
    // against pre-edit audio; treat it as a discontinuity (reset) instead.
    const bool contiguous = previousPage
        && previousPage->validAspects != 0
        && previousPage->contentEpoch.load() >= contentEpochNow()
        && previousPage->startPosition + previousPage->validFrames == startPos;

    if (contiguous) {
        // Resume DSP state from previous page's snapshot
        std::lock_guard<std::mutex> lock(previousPage->pageMutex);
        restoreInternalState(previousPage->internalState);
    } else {
        reset();
    }
    seekTo((offset_t)startPos);
    seekInputStreams((offset_t)startPos);

    // Calculate input data available for this page
    const sample_t *pageInput = nullptr;
    length_t pageInputLength = 0;

    if (inputData && inputOffset < inputLength) {
        pageInput = inputData + inputOffset;
        pageInputLength = inputLength - inputOffset;
    }

    // Render frames into page buffer.
    // Safe: page is in the map (protected by refcount), nobody else will create it.
    // Safe to call recursively because mutex is not held.
    // Safe from infinite recursion because FreezeContext is active; calcOutputTo
    // can query pre-frozen input pages instead of calling freezePage again.
    length_t toRender = twOutputPage::FRAME_CAPACITY;
    length_t rendered = renderFrames(
        page->samples.data(),
        toRender,
        pageInput,
        pageInputLength,
        0  // idx = 0 (first output)
    );

    // Capture new internal state for next page resumption
    {
        std::lock_guard<std::mutex> lock(page->pageMutex);
        page->internalState = captureInternalState();
    }

    // FreezeContext is automatically destroyed at end of scope, restoring previous context
    return rendered;
}

// Phase 3: Preview-specific page freezing
// Renders component output at lower resolution for UI visualization (e.g., waveform display).
// Non-blocking: returns previous page if new page not yet ready.
std::shared_ptr<twOutputPage> twComponent::freezePreviewPage(
    uint64_t startPos,
    length_t length,
    int previewSampleRate,
    int fullSampleRate,
    std::shared_ptr<twOutputPage> previousPage
)
{
    // Freeze at preview resolution: lower sample rate (typically 1kHz) for visualization
    // This allows quick redraws without waiting for full-rate processing.
    // If page not ready, return previous page for fallback rendering.
    // Note: g_inCalcOutputToPath flag is now set inside freezePage_nolock for all rendering paths.

    auto newPage = freezePage(
        startPos,
        nullptr,                    // No input data; component generates output
        0,
        length,
        previewSampleRate,          // Render at lower rate for preview
        previousPage                // Provide previous page for state restoration
    );

    // Non-blocking fallback: if new page couldn't be materialized, use previous
    if( !newPage || newPage->validFrames == 0 ) {
        return previousPage;
    }

    // Mark preview aspect as valid so UI knows data is ready
    newPage->validAspects = twAspectPreview;

    return newPage;
}

// ============================================================================
// Tier 2 Enhancement #1: Selective Invalidation with Dependency Tracking
// ============================================================================

void twComponent::addDependent(std::shared_ptr<twComponent> dependent) {
    if (!dependent) return;

    std::lock_guard<std::mutex> lock(mutex());

    // Avoid duplicate entries
    auto it = std::find(dependents_.begin(), dependents_.end(), dependent);
    if (it == dependents_.end()) {
        dependents_.push_back(dependent);
    }
}

// ============================================================================
// Phase 3 Refactor: IOVector Interface Migration
// ============================================================================

// Default implementation of calcOutputTo(IOVector&): wraps in temporary page-backed buffer
// and calls the raw-pointer interface. This provides a migration path: existing components
// can continue using the old raw-pointer interface, while new components can override
// this IOVector version for direct page-backed rendering (more efficient).
length_t twComponent::calcOutputTo( IOVector& dest, idx_t idx )
{
    // Create temporary buffer for old interface, then copy to dest
    auto tmpPage = std::make_shared<twOutputPage>();
    tmpPage->samples.resize(dest.length(), 0.0f);

    // Call the raw-pointer version (which all existing components implement)
    length_t rendered = calcOutputTo(tmpPage->samples.data(), dest.length(), idx);

    // Copy rendered data into the IOVector destination
    if (rendered > 0) {
        dest.copyFrom(IOVector::CreateForPageOutput(tmpPage), 0, rendered);
    }

    return rendered;
}

// Default implementation of calcOutputTo(raw-pointer) for Phase 3 migration.
// During Phase 3, subclasses remove their raw-pointer implementations and
// get this default, which wraps the IOVector version instead.
// This reverses the old dependency: IOVector <= raw-pointer becomes raw-pointer <= IOVector.
length_t twComponent::calcOutputTo( sample_t *pDest, length_t length, idx_t idx )
{
    // Use temporary page-backed IOVector wrapper
    auto tmpPage = std::make_shared<twOutputPage>();
    tmpPage->samples.resize(length, 0.0f);
    IOVector dest = IOVector::CreateForPageOutput(tmpPage);

    // Call the IOVector version (the new primary interface)
    length_t rendered = calcOutputTo(dest, idx);

    // Copy result back to raw-pointer buffer
    if (rendered > 0) {
        memcpy(pDest, tmpPage->samples.data(), rendered * sizeof(sample_t));
    }

    return rendered;
}

// Teardown protocol: mark as ZOMBIE, deregister from parent, notify dependents
void twComponent::teardown()
{
    // Phase 1: Mark as ZOMBIE immediately (audio thread sees this on next render)
    state_.store(ComponentState::ZOMBIE, std::memory_order_release);

    // Phase 1b: Self-deregister from parent (bidirectional handoff)
    if (auto parent = parentComponent_.lock()) {
        if (myInputIndex_ >= 0) {
            parent->removeInput(myInputIndex_);
        }
    }

    // Phase 1c: Notify dependents this component is being torn down
    std::vector<std::shared_ptr<twComponent> > depsCopy;
    {
        std::lock_guard<std::mutex> lock(mutex());
        depsCopy = dependents_;
    }
    for (auto dep : depsCopy) {
        if (dep) dep->onDependencyTeardown(shared_from_this());
    }

    // Phase 2: Snapshot children and recurse (base class has no children, subclasses override)
}

// Set input slot to nullptr (called by child during teardown)
void twComponent::removeInput(idx_t idx)
{
    std::lock_guard<std::mutex> lock(mutex());

    // Bounds check
    if (idx < 0 || idx >= (idx_t)pInputPlugs_.size()) {
        return;
    }

    // Set to nullptr (audio thread sees this immediately)
    pInputPlugs_[idx] = nullptr;
}

// Default callback when a dependency is torn down
void twComponent::onDependencyTeardown(std::shared_ptr<twComponent> dep)
{
    // Default: ignore (component handles NULL inputs gracefully)
    (void)dep;  // Suppress unused parameter warning
}
