
#include <stdlib.h>
#include <memory.h>
#include <string.h>
#include <thread>

#include "twsyslog.h"

#include "twcomponent.h"
#include "tw_freeze_context.h"

twStreamingLatch::twStreamingLatch (twComponent & component0, idx_t idx0, length_t bufSize0)
	: twLatch (component0, idx0), currentPos_(0), previousPage_(nullptr), sampleRate_(48000)
{
	if (bufSize0 == 0)
		bufSize0 = bufSizeDefault;
	init(bufSize0);
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
	if (getComponent().env.getSRate() > 0) {
		sampleRate_ = getComponent().env.getSRate();
	}
#ifdef DEBUG_COMPONENT
	fprintf( sterr, "twStreamingLatch::init( %d ): leaving.", bufSize0 );
#endif
}

length_t twStreamingLatch::copyData( offset_t startOffset, sample_t *pDest, length_t maxLength )
{
	if (!pDest || maxLength <= 0 || startOffset < 0) {
		return 0;  // Nothing to copy
	}

	// Serve reads from position-aligned frozen pages.
	//
	// The consumer's startOffset is a timeline position; it maps directly onto
	// page-aligned freezePage() requests against the producing component, so
	// the position a consumer reads at and the content it receives can never
	// diverge. The held page (previousPage_) is reused while the consumer is
	// still inside it, and passed as the state-chain predecessor when crossing
	// into the immediately following page, so stateful producers (reverbs,
	// filters) continue seamlessly across page boundaries. Any other page
	// transition is a discontinuity, which the producer's freezePage() answers
	// with reset() + seekTo(pageStart).
	const uint64_t pageSize = twOutputPage::FRAME_CAPACITY;
	length_t written = 0;

	while (written < maxLength) {
		const uint64_t pos = (uint64_t)startOffset + (uint64_t)written;
		const uint64_t pageStart = (pos / pageSize) * pageSize;

		std::shared_ptr<twOutputPage> page = previousPage_;
		if (!page || page->startPosition != pageStart || page->validAspects == 0) {
			// Need a different page than the one we hold.
			// Cycle guard: if the producer is already being frozen on this
			// thread, recursing into freezePage() would loop forever.
			if (FreezeContext::isComponentInStack(getComponent())) {
				break;
			}

			// Chain state only from the immediate predecessor page; anything
			// else is a discontinuity the producer must reset+seek for.
			std::shared_ptr<twOutputPage> chainFrom;
			if (previousPage_ && previousPage_->validAspects != 0 &&
			    previousPage_->startPosition + previousPage_->validFrames == pageStart) {
				chainFrom = previousPage_;
			}

			page = getComponent().freezePage(
				pageStart,
				nullptr,                // no pre-prepared input (pull model)
				0,
				(length_t)pageSize,     // full page
				sampleRate_,
				chainFrom);

			if (!page || page->validAspects == 0) {
				break;  // producer could not materialize this page
			}
			previousPage_ = page;
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

	return written;
}
