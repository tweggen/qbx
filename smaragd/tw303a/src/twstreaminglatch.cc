
#include <stdlib.h>
#include <memory.h>
#include <string.h>
#include <thread>

#include "twsyslog.h"

#include "twcomponent.h"
#include "tw_freeze_context.h"

#undef DEBUG_COPYDATA

static inline int min( int a, int b )
{
	if( a<b ) return a;
	return b;
}

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
	length_t toCopy;
	offset_t destPos;

#ifdef DEBUG_COPYDATA
	fprintf( sterr, "twStreamingLatch::copyData( %d, pDest, %d): entered, bufSize = %d",
		startOffset, maxLength, bufSize );
#endif

	if (!pDest || maxLength <= 0) {
		return 0;  // Nothing to copy
	}

	// CRITICAL: maxLength is the destination buffer size allocated by the caller.
	// Do NOT clamp it based on our internal bufSize — these are independent constraints.
	// The per-memcpy bounds checks below (destPos + length <= maxLength) ensure we never
	// overflow pDest. The architectural issue (implicit buffer contracts) remains, but
	// we must not silently truncate legitimate requests.
	if (maxLength >= bufSize * 4) {
		fprintf(stderr, "WARNING: copyData contract violation: maxLength=%lld >= 4*bufSize=%lld (ratio=%.1fx). "
			"Likely architectural: caller pre-buffering or unit mismatch (bytes vs samples).\n",
			maxLength, bufSize, (double)maxLength / bufSize);
	} else if (maxLength > bufSize && maxLength > 32768) {
		fprintf(stderr, "DEBUG: copyData called with large maxLength=%lld > bufSize=%lld. "
			"May indicate caller needs optimization.\n",
			maxLength, bufSize);
	}

	toCopy = maxLength;
	destPos = 0;

	// SAFETY: Ensure we never write past destPos + maxLength, even in wraparound cases
	const sample_t * const pDestMax = pDest + maxLength;  // Hard boundary

	while( toCopy>0 ) {

		// copy process:
		// first get all out off the loaded stuff, then reload.
		// exit, if everything loaded.

		// still data from buffer (stuff loaded)?
		if( startOffset<offset ) {
			// yes, load it.
			length_t memcpyLength;
			// bufStartOffset is the position of startOffset in the buffer.
			offset_t bufStartOffset;

	
			// calculate the number of samples not read in the ringbuffer.
			length_t dataAvail = offset-startOffset;
	
#ifdef DEBUG_COMPONENT
			if( dataAvail>bufSizeDefault ) {
				throw excStandard( "twLatchStreamingOutput::readStreamingData(): Latch buffer overrun." );
			}
#endif
			// the startOffset equivalent in the buffer is (offset-startOffset) before bufPos.
			// modulo bufSize.
			bufStartOffset = (bufPos - (offset-startOffset) + bufSize) % bufSize;

			memcpyLength = min( dataAvail, toCopy );
			// Ensure we don't write past the destination buffer
			if (destPos + memcpyLength > maxLength) {
				memcpyLength = maxLength - destPos;
			}
			if (memcpyLength <= 0) {
				break;  // Destination buffer is full, stop copying
			}

			// copy out the data still stored in the latch
#ifdef DEBUG_COPYDATA
			fprintf( sterr, "twStreamingLatch::copyData( %d, pDest, %d): "
			                   "bufSize = %d; reading %d bytes, "
			                   "bufStartOffset = %d;",
				startOffset, maxLength, bufSize, memcpyLength, bufStartOffset );
#endif
			if( (bufSize-bufStartOffset) >= (offset_t)memcpyLength ) {
				// no wraparound
				if (destPos + memcpyLength > maxLength) {
					memcpyLength = maxLength - destPos;  // Clamp again
				}
				if (memcpyLength > 0) {
					// SAFETY: assert we're not writing past the hard boundary
					if (pDest + destPos + memcpyLength > pDestMax) {
						fprintf(stderr, "FATAL: memcpy would overflow: destPos=%lld, memcpyLength=%lld, maxLength=%lld\n",
							destPos, memcpyLength, maxLength);
						break;
					}
					memcpy( pDest+destPos, pBuffer+bufStartOffset, memcpyLength*sizeof( sample_t ) );
					destPos += memcpyLength;
				}
			} else {
				length_t part;
				length_t part_actual = 0;  // Track how much we actually wrote in first part
				// wraparound - need extra bounds checking since we do two memcpys
				// copy part to the end
				part = (bufSize-bufStartOffset);
				if (destPos + part > maxLength) {
					part = maxLength - destPos;  // Clamp first part
				}
				if (part > 0) {
					// SAFETY: assert we're not writing past the hard boundary
					if (pDest + destPos + part > pDestMax) {
						fprintf(stderr, "FATAL: wraparound memcpy[1] would overflow: destPos=%lld, part=%lld, maxLength=%lld\n",
							destPos, part, maxLength);
						break;
					}
					memcpy( pDest+destPos, pBuffer+bufStartOffset, part*sizeof( sample_t ) );
					destPos += part;
					part_actual = part;  // Track actual first part written
				}

				// then part from the buffer start.
				// CRITICAL: recalculate based on actual first part, not intended first part
				part = (memcpyLength - part_actual);  // Use actual first part, not (bufSize-bufStartOffset)
				if (part > 0 && destPos < maxLength) {
					if (destPos + part > maxLength) {
						part = maxLength - destPos;  // Clamp second part
					}
					if (part > 0) {
						// SAFETY: assert we're not writing past the hard boundary
						if (pDest + destPos + part > pDestMax) {
							fprintf(stderr, "FATAL: wraparound memcpy[2] would overflow: destPos=%lld, part=%lld, maxLength=%lld\n",
								destPos, part, maxLength);
							break;
						}
						memcpy( pDest+destPos, pBuffer, part*sizeof( sample_t ) );
						destPos += part;
					}
				}
			}
			toCopy -= memcpyLength;
			startOffset += memcpyLength;
		} else if( startOffset>=offset ) {
			// startOffset now is greater or equal (most probably equal) 
			// to this latch's offset.
			// read more stuff into the latch.
			length_t toFill = maxLength;
			length_t filled, maxFill;

			maxFill = min( (bufSize-bufPos), toFill );

#ifdef DEBUG_COPYDATA
			fprintf( sterr, "twStreamingLatch::copyData( %d, pDest, %d): trying to fill up %d bytes.",
				startOffset, maxLength, maxFill );
#endif

			// not enough data in buffer, fill it up.
			// CRITICAL: Check for active FreezeContext to avoid recursive freezePage calls.
			// When inside freezePage_nolock rendering, a FreezeContext is active and holds
			// pre-frozen input pages. We should use those instead of calling freezePage again.
			// Architecture: freezePage → calcOutputTo → readStreamingData → copyData
			//             If in frozen render, use pre-frozen inputs. If streaming, use freezePage.

			FreezeContext* ctx = FreezeContext::current();
			if (ctx) {
				// Inside a frozen render (freezePage_nolock is active).
				// Try to get pre-frozen input from the context.
				// Note: this latch belongs to a component; query by this latch's output index
				auto frozenInput = ctx->getInputPage(getIndex());
				if (frozenInput && frozenInput->samples.size() > 0) {
					// Use pre-frozen data directly
					filled = min((length_t)maxFill, (length_t)frozenInput->samples.size());
					if (filled > 0) {
						memcpy(pBuffer + bufPos, frozenInput->samples.data(), filled * sizeof(sample_t));
						previousPage_ = frozenInput;  // cache for next iteration
						currentPos_ += filled;         // advance position
					} else {
						filled = 0;
					}
				} else {
					// No pre-frozen input available; buffer starvation.
					// This component is a source or input wasn't pre-frozen.
					// Signal no data; caller handles gracefully.
					filled = 0;
				}
			} else {
				// Normal streaming path (not inside frozen render).
				// Use freezePage for proper state snapshots and caching.
				auto page = getComponent().freezePage(
					currentPos_,              // snapshot position for state
					nullptr,                  // no input (source component)
					0,                        // no input offset
					maxFill,                  // length to fill
					sampleRate_,              // project sample rate
					previousPage_             // cached state from last page
				);

				if (page && page->samples.size() > 0) {
					filled = min((length_t)maxFill, (length_t)page->samples.size());
					if (filled > 0) {
						memcpy(pBuffer + bufPos, page->samples.data(), filled * sizeof(sample_t));
						previousPage_ = page;  // cache for next freezePage() call
						currentPos_ += filled; // advance position
					} else {
						filled = 0;
					}
				} else {
					filled = 0;
				}
			}

			if( !filled ) {
				// No data available. This can happen when:
				// 1. Component is in a render context (freezePage) where streaming latches don't work
				// 2. Component truly has no data
				// In either case, break and return what we have so far.
				break;
			}
			bufPos = (bufPos + filled) % bufSize;
			offset += filled;
		}
	}

#ifdef DEBUG_COPYDATA
	fprintf( sterr, "twStreamingLatch::copyData( %d, pDest, %d): Leaving.",
		startOffset, maxLength  );
#endif

	return destPos;
}



