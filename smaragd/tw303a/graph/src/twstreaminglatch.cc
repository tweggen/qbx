
#include <stdlib.h>
#include <memory.h>
#include <string.h>
#include <thread>

#include "tw/core/twsyslog.h"

#include "tw/graph/twcomponent.h"
#include "tw/graph/tw_freeze_context.h"
#include "tw/graph/tw_frozen_inputs.h"

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
                                     std::shared_ptr<twOutputPage>& readerPrevPage,
                                     std::atomic<uint64_t>& readerPrevEpoch )
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

	// Staleness is decided by "has MY producer's epoch moved since I accepted
	// this page", NOT by the page's own contentEpoch stamp. The stamp is
	// written by whichever component actually RENDERED the page, and that is
	// frequently not getComponent(): an insert-less twPluginChain launders its
	// upstream twTrackMix page through verbatim, so the page arrives carrying
	// the TRACKMIX's counter while the gate below compares against the CHAIN's.
	// The two are independent per-component atomics (both start at 1) and the
	// trackmix self-bumps on every insertClip/removeClip/updateClip/
	// setClipMuted/setTrackGain, so its counter runs permanently ahead — making
	// the old `page->contentEpoch < epochNow` test dead code that could never
	// fire. Measured on a folder track: held page stamped 9, chain epoch 7,
	// so a clip deleted from a NESTED lane went on being played and metered
	// forever (the page was re-served, then re-stamped current). Comparing a
	// remembered observation of ONE counter against itself cannot drift.
	// 0 is a safe "never observed" sentinel: epochs start at 1.
	uint64_t heldEpoch = readerPrevEpoch.load();

	while (written < maxLength) {
		const offset_t pos = startOffset + (offset_t)written;
		// FLOOR-aligned: this seam serves clips that may start before their
		// data, so pos can be negative (proposal 23).
		const offset_t pageStart = twFloorAlign( pos, (offset_t)pageSize );

		// The held page must be frozen AND from the current content epoch —
		// an edit (clip move/split/stretch, mute, rewiring) makes every page
		// rendered before it stale, even though its validAspects are still set.
		std::shared_ptr<twOutputPage> page = held;
		if (!page || page->startPosition != pageStart || page->validAspects == 0 ||
		    heldEpoch != epochNow) {
			// Need a different page than the one we hold.

			// Proposal 19 dataflow stage 1: a leaf render in progress on this
			// thread may have BOUND this producer's page (see
			// tw/graph/tw_frozen_inputs.h) — serve it with NO recursive pull.
			// Bound pages are trusted (no epoch re-check): epoch validity is
			// the scheduler's verify-at-publish job, which is what lets a
			// planned node render from exactly the pages it was planned
			// against. An installed-but-missing binding is recorded (stage >1
			// turns that into "node not ready") and falls through to the
			// legacy pull below, unchanged.
			bool boundServed = false;
			if (const twFrozenInputs *fi = twFrozenInputScope::active()) {
				std::shared_ptr<twOutputPage> bound =
					fi->find(getComponent().get(), pageStart);
				if (bound && bound->validAspects != 0 &&
				    bound->startPosition == pageStart) {
					held = bound;
					page = bound;
					// Deliberately NOT observed-at-epochNow. A bound page is
					// trusted only for the render that bound it (epoch validity
					// is the scheduler's verify-at-publish job); recording it as
					// current would let a LATER call reuse it off the reader
					// hint, outside any binding, with nothing left to validate
					// it. 0 forces the next call to re-validate — which is what
					// mix_test's "empty set falls back to the legacy pull"
					// control asserts.
					heldEpoch = 0;
					boundServed = true;
				} else {
					fi->noteMiss(getComponent().get(), pageStart);
				}
			}

			if (!boundServed) {
			// Cycle guard: if the producer is already being frozen on this
			// thread, recursing into freezePage() would loop forever.
			if (FreezeContext::isComponentInStack(getComponent())) {
				break;
			}

			// Chain state only from the immediate predecessor page; anything
			// else is a discontinuity the producer must reset+seek for.
			// A stale-epoch predecessor is also a discontinuity: its DSP state
			// was computed against pre-edit audio.
			// Same like-for-like rule as the reuse gate above: `heldEpoch` is
			// the epoch we OBSERVED when we took `held`, so it is comparable
			// with epochNow; the page's own stamp is not (see above).
			std::shared_ptr<twOutputPage> chainFrom;
			if (held && held->validAspects != 0 &&
			    heldEpoch == epochNow &&
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
			heldEpoch = epochNow;
			} // !boundServed (legacy pull)
		}

		const uint64_t inPage = pos - pageStart;
		if (inPage >= (uint64_t)page->validFrames) {
			break;  // producer ran dry inside this page
		}

		const uint64_t avail = (uint64_t)page->validFrames - inPage;
		const uint64_t want  = (uint64_t)(maxLength - written);
		const length_t n = (length_t)(avail < want ? avail : want);

		// THE PLUG SEAM. This latch already carries the index it was built with
		// (twLatch(component, idx)) and has never consulted it. Proposal 35 4.4
		// rule (1) gives that index its channel meaning here -- a plug pull yields
		// channel min(latchIndex, page->channels - 1) of the page the producer
		// ACTUALLY froze, which reproduces today's behaviour exactly while every
		// page is one channel wide. Wiring the latch index in is B2's job; what
		// B1b does is make the channel EXPLICIT, so the day it becomes a variable
		// there is one place to change.
		memcpy(pDest + written, page->channelPtr(0) + inPage, (size_t)n * sizeof(sample_t));
		written += n;
	}

	// Publish the last page this reader served, plus the epoch it was observed
	// at, for continuity on its next call. `heldEpoch` is only advanced where
	// `held` is, so a call that bailed out without acquiring anything leaves
	// the pair unchanged rather than blessing a stale page as current.
	std::atomic_store(&readerPrevPage, held);
	readerPrevEpoch.store(heldEpoch);
	return written;
}
