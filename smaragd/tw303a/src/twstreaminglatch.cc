
#include <stdlib.h>
#include <memory.h>
#include <string.h>

#include <syslog.h>

#include "twcomponent.h"

#undef DEBUG_COPYDATA

static inline int min( int a, int b )
{
	if( a<b ) return a;
	return b;
}

twStreamingLatch::twStreamingLatch (twComponent & component0, idx_t idx0, length_t bufSize0)
	: twLatch (component0, idx0)
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
	syslog( LOG_DEBUG, "twStreamingLatch::init( %d ): called.", bufSize0 );
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
#ifdef DEBUG_COMPONENT
	syslog( LOG_DEBUG, "twStreamingLatch::init( %d ): leaving.", bufSize0 );
#endif
}

length_t twStreamingLatch::copyData( offset_t startOffset, sample_t *pDest, length_t maxLength )
{
	length_t toCopy;
	offset_t destPos;

#ifdef DEBUG_COPYDATA
	syslog( LOG_DEBUG, "twStreamingLatch::copyData( %d, pDest, %d): entered, bufSize = %d",
		startOffset, maxLength, bufSize );
#endif

	toCopy = maxLength;
	destPos = 0;

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
			// copy out the data still stored in the latch
#ifdef DEBUG_COPYDATA
			syslog( LOG_DEBUG, "twStreamingLatch::copyData( %d, pDest, %d): "
			                   "bufSize = %d; reading %d bytes, "
			                   "bufStartOffset = %d;",
				startOffset, maxLength, bufSize, memcpyLength, bufStartOffset );
#endif
			if( (bufSize-bufStartOffset) >= (offset_t)memcpyLength ) {
				// no wraparound
				memcpy( pDest+destPos, pBuffer+bufStartOffset, memcpyLength*sizeof( sample_t ) );
				destPos += memcpyLength;
			} else {
				length_t part;
				// wraparound
				// copy part to the end
				part = (bufSize-bufStartOffset);
				memcpy( pDest+destPos, pBuffer+bufStartOffset, part*sizeof( sample_t ) );
				destPos += part;
				// then part from the buffer start.
				part = (memcpyLength-(bufSize-bufStartOffset));
				memcpy( pDest+destPos, pBuffer, part*sizeof( sample_t ) );
				destPos += part;
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
			syslog( LOG_DEBUG, "twStreamingLatch::copyData( %d, pDest, %d): trying to fill up %d bytes.",
				startOffset, maxLength, maxFill );
#endif

			// not enough data in buffer, fill it up.
			filled = getComponent().calcOutputTo( pBuffer + bufPos, maxFill, 0 );
			if( !filled ) {
				throw excStandard( "twLatchStreamingOutput::readStreamingData(): "
									   "Internal: Component did not provide data." );
			}
			bufPos = (bufPos + filled) % bufSize;
			offset += filled;
		}
	}

#ifdef DEBUG_COPYDATA
	syslog( LOG_DEBUG, "twStreamingLatch::copyData( %d, pDest, %d): Leaving.",
		startOffset, maxLength  );
#endif

	return destPos;
}



