
#include <stdlib.h>
#include <memory.h>
#include <string.h>
#include <thread>

#include "tw/core/twsyslog.h"

#include "tw/graph/twcomponent.h"
#include "tw/graph/tw_freeze_context.h"

twStreamingLatch::twStreamingLatch (std::shared_ptr<twComponent> component0, idx_t idx0, length_t bufSize0)
	: twLatch (component0, idx0), sampleRate_(48000)
{
	if (bufSize0 == 0)
		bufSize0 = bufSizeDefault;
	init(bufSize0);
}

twLatchOutput * twStreamingLatch::addOutput()
{
	// Allocate the streaming subtype: consumers static_cast their input plug to
	// twLatchStreamingOutput and it carries the per-reader page-chain hint, so
	// the object must actually BE one (the base addOutput would make a plain
	// twLatchOutput, and touching the extra field through it is out-of-bounds).
	auto pOutput = std::make_shared<twLatchStreamingOutput>( *this );
	outputList.push_back( pOutput );
	// The caller wires with a raw pointer (linkOutput/setInput signatures); the
	// consumer takes shared ownership via sharedOutput() in twComponent::setInput.
	return pOutput.get();
}

twStreamingLatch::~twStreamingLatch ()
{
	if (pBuffer) {
		free (pBuffer);
		pBuffer = NULL;
	}
}

void twStreamingLatch::init( length_t bufSize0 )
{
#ifdef DEBUG_COMPONENT
	fprintf( sterr, "twStreamingLatch::init( %d ): called.", bufSize0 );
#endif
	// allocate buffer
	bufSize = bufSize0;
	bufPos = 0;
	pBuffer = (sample_t *) malloc (bufSize * sizeof (sample_t));
	if (!pBuffer) {
		throw excStandard( "twStreamingLatch::init(): Not enough memory for internal buffer" );
	}
	// reset pointer
	offset = 0;

	// Phase 2: Get sample rate from component's environment
	// This is a temporary initialization; the actual sampleRate might change if the project sample rate changes
	if (getComponent()->env.getSRate() > 0) {
		sampleRate_ = getComponent()->env.getSRate();
	}
#ifdef DEBUG_COMPONENT
	fprintf( sterr, "twStreamingLatch::init( %d ): leaving.", bufSize0 );
#endif
}

length_t twStreamingLatch::copyData( offset_t startOffset, sample_t *pDest, length_t maxLength,
                                     std::shared_ptr<twOutputPage>& readerPrevPage )
{
	if (!pDest || maxLength <= 0 || startOffset < 0) {
		return 0;  // Nothing to copy
	}

	// Serve reads from position-aligned frozen pages.
	//
	// The consumer's startOffset is a timeline position; it maps directly onto
	// page-aligned freezePage() requests against the producing component, so
	// the position a consumer reads at and the content it receives can never
	// diverge. The held page is reused while the consumer is still inside it,
	// and passed as the state-chain predecessor when crossing into the
	// immediately following page, so stateful producers (reverbs, filters)
	// continue seamlessly across page boundaries. Any other page transition is
	// a discontinuity, which the producer's freezePage() answers with reset() +
	// seekTo(pageStart).
	//
	// The hint belongs to the CALLING reader (readerPrevPage), not to this
	// shared latch, so two readers of one fanned-out latch cannot corrupt each
	// other's chain. Snapshot it once into a local, work on the local, and
	// publish the final value back before returning. The load/store are atomic
	// because a double-render of this reader can run copyData concurrently on
	// two freeze threads; the hint is advisory, so last-writer-wins is fine.
	const uint64_t pageSize = twOutputPage::FRAME_CAPACITY;
	const uint64_t epochNow = getComponent()->contentEpochNow();
	length_t written = 0;

	std::shared_ptr<twOutputPage> held = std::atomic_load(&readerPrevPage);

	while (written < maxLength) {
		const uint64_t pos = (uint64_t)startOffset + (uint64_t)written;
		const uint64_t pageStart = (pos / pageSize) * pageSize;

		// The held page must be frozen AND from the current content epoch —
		// an edit (clip move/split/stretch, mute, rewiring) makes every page
		// rendered before it stale, even though its validAspects are still set.
		std::shared_ptr<twOutputPage> page = held;
		if (!page || page->startPosition != pageStart || page->validAspects == 0 ||
		    page->contentEpoch.load() < epochNow) {
			// Need a different page than the one we hold.
			// Cycle guard: if the producer is already being frozen on this
			// thread, recursing into freezePage() would loop forever.
			if (FreezeContext::isComponentInStack(getComponent())) {
				break;
			}

			// Chain state only from the immediate predecessor page; anything
			// else is a discontinuity the producer must reset+seek for.
			// A stale-epoch predecessor is also a discontinuity: its DSP state
			// was computed against pre-edit audio.
			std::shared_ptr<twOutputPage> chainFrom;
			if (held && held->validAspects != 0 &&
			    held->contentEpoch.load() >= epochNow &&
			    held->startPosition + held->validFrames == pageStart) {
				chainFrom = held;
			}

			page = getComponent()->freezePage(
				pageStart,
				nullptr,                // no pre-prepared input (pull model)
				0,
				(length_t)pageSize,     // full page
				sampleRate_,
				chainFrom);

			if (!page || page->validAspects == 0) {
				break;  // producer could not materialize this page
			}
			held = page;
		}

		const uint64_t inPage = pos - pageStart;
		if (inPage >= (uint64_t)page->validFrames) {
			break;  // producer ran dry inside this page
		}

		const uint64_t avail = (uint64_t)page->validFrames - inPage;
		const uint64_t want  = (uint64_t)(maxLength - written);
		const length_t n = (length_t)(avail < want ? avail : want);

		memcpy(pDest + written, page->samples.data() + inPage, (size_t)n * sizeof(sample_t));
		written += n;
	}

	// Publish the last page this reader served, for continuity on its next call.
	std::atomic_store(&readerPrevPage, held);
	return written;
}
