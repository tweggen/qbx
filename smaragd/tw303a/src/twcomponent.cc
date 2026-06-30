#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <string.h>

#include <algorithm>
#include <iostream>
#include <iterator>
#include <vector>

#include "twsyslog.h"

#include "twcomponent.h"
#include "io_vector.h"

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
    for( idx_t j = 0; j < nOut; ++j ) {
        twFormat prev = ( (std::size_t) j < committedOut_.size() )
                      ? committedOut_[j] : twFormat{};
        if( prev != out[j] ) emit formatChanged( j, prev, out[j] );
    }
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
    if( pOutputLatches ) {
        for( idx_t i = 0; i < getNOutputs(); i++ ) {
            if( pOutputLatches[i] ) {
                pOutputLatches[i]->resetOffset();
            }
        }
    }
}

void twComponent::allocPlugs()
{
    if( getNInputs()>0 ) {
        pInputPlugs = (twLatchOutput **) ::calloc (sizeof (twLatchOutput *), getNInputs() );
        if( !pInputPlugs ) {
            throw excStandard(
                "twComponent::twComponent(): "
                "Not enough memory to create input plug pointer table." );
        }
    } else {
        pInputPlugs = NULL;
    }
    if( getNOutputs()>0 ) {
        pOutputLatches = (twLatch **)  calloc (sizeof (twLatch *), getNOutputs() );
        if( !pOutputLatches ) {
            throw excStandard(
                "twComponent::twPlugs(): "
                "Not enough memory to create output plug pointer table." );
        }
    } else {
        pOutputLatches = NULL;
    }
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
    return pOutputLatches[idx]->addOutput();
}

void twComponent::setInput( idx_t idx, twLatchOutput *newOutput )
{
    // Guard: if pInputPlugs is null, the component wasn't properly initialized
    // (likely because it reports 0 inputs, or was created but not fully initialized).
    // Silently return to prevent a crash; the component won't have audio routed to it.
    if( !pInputPlugs ) {
        fprintf(stderr, "twComponent::setInput(): pInputPlugs == nullptr (component reports 0 inputs)\n");
        return;
    }

    // Guard: bounds check to prevent array overflow
    if( idx >= getNInputs() ) {
        fprintf(stderr, "twComponent::setInput(): idx=%d >= getNInputs()=%d\n", (int)idx, (int)getNInputs());
        return;
    }

    // Avoid unnecessary reconnection: if input is already set to the same output, do nothing
    if( pInputPlugs[idx] == newOutput ) {
        return;
    }

    if( pInputPlugs[idx] ) {
        auto parentLatch = &(pInputPlugs[idx]->getParentLatch());
        if(parentLatch==nullptr)
        {
            fprintf(stderr, "twComponent::setInput(): pInputPlugs[idx]->getParentLatch() == null\n");
            return;
        }
        pInputPlugs[idx]->getParentLatch().deleteOutput( pInputPlugs[idx] );
        pInputPlugs[idx] = NULL;
        if( !newOutput ) --inputsSet_;
    } else {
        if( newOutput ) ++inputsSet_;
    }
    pInputPlugs[idx] = newOutput;
}

/**
 * Use of this method is dangerous. However, it allows nice things.
 * take care of dangling references.
 */
twLatchOutput *twComponent::getInputPlug( idx_t idx ) const
{
    if( idx<0 || idx>getNInputs() ) return NULL;
    return pInputPlugs[idx];
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
    if( pInputPlugs ) free( pInputPlugs );
    if( pOutputLatches ) free( pOutputLatches );
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
      env( env0 ),
      pOutputLatches(0),
      pInputPlugs(0)
{    
}

// ============================================================================
// Output Page Caching Implementation (Phase 1 - Component-Level Freezing)
// ============================================================================

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

    // Step 1: Check cache and allocate placeholder under lock
    std::shared_ptr<twOutputPage> page;
    bool needsRendering = false;

    {
        std::lock_guard<std::mutex> lock(mutex());
        auto it = outputPages_.find(startPos);
        if (it != outputPages_.end()) {
            // Page already exists (valid or invalid); return it
            page = it->second;
        } else {
            // Page doesn't exist; allocate and insert placeholder
            // This prevents another thread from allocating the same page concurrently
            page = std::make_shared<twOutputPage>();
            page->startPosition = startPos;
            page->createdAt = std::chrono::steady_clock::now();
            page->validAspects = 0;  // Not yet frozen
            outputPages_[startPos] = page;
            needsRendering = true;
        }
    } // Lock released - safe for recursive freezePage calls

    // Step 2: If page was already cached, return it
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
    {
        std::lock_guard<std::mutex> lock(mutex());
        page->validAspects = twAspectAll;  // Mark all aspects as complete
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
    // Initialize state: reset if page 0, else restore from previous
    if (previousPage) {
        // Resume from previous page's snapshot
        std::lock_guard<std::mutex> lock(previousPage->pageMutex);
        restoreInternalState(previousPage->internalState);
    } else {
        // First page: start from reset state
        reset();
    }

    // Calculate input data available for this page
    const sample_t *pageInput = nullptr;
    length_t pageInputLength = 0;

    if (inputData && inputOffset < inputLength) {
        pageInput = inputData + inputOffset;
        pageInputLength = inputLength - inputOffset;
    }

    // Render frames into page buffer
    // Safe: page is in the map (protected by refcount), nobody else will create it
    // Safe to call recursively because mutex is not held
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

void twComponent::addDependent(twComponent* dependent) {
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
