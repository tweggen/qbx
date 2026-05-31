
#include <stdlib.h>
#include <memory.h>
#include <string.h>

#include "twsyslog.h"

#include "twcomponent.h"

 const int twStreamingLatch::bufSizeDefault = 16384;

/**
 *	@method twLatchStreamingOutput.readStreamingData
 *		Read data from a stream output.
 *	@synopsis length_t twLatchStreamingOutput::readStreamingData(
 *      sample_t * pDest, length_t maxLength )
 *	@param </i><tt>sample_t *</tt><i>pDest
 *		The buffer, the samples should be written to.
 *	@param </i><tt>length_t</tt><i> maxLength
 *		The maximum of samples to be read.
 *	@desc
 *		Reads a maximum <i>maxLength</i> samples to <i>pDest</i>
 *	@returns
 *		The number of samples read.
 */
length_t twLatchStreamingOutput::readStreamingData( sample_t * pDest, length_t maxLength )
{
	length_t len;

	len = getParentStreamingLatch().copyData( offset, pDest, maxLength );
	if( len>0 ) {
		offset += len;
	}
	return len;
}

length_t twLatchStreamingOutput::readRaw( void * dest, length_t maxFrames )
{
	// twStreamingLatch stores canonical mono Float32, so native bytes are the
	// float samples readStreamingData yields. A latch holding another binary
	// representation would override its output's readRaw alongside getFormat().
	return readStreamingData( (sample_t *) dest, maxFrames );
}

/**
 *	@method twLatch.twLatch
 *		Constructor for twLatch object.
 *	@synopsis twLatch::twLatch( twComponent & component0 )
 *	@param </i><tt>twComponent &amp;</tt><i>component0
 *		The component, this latch is associated with.
 *	@desc
 *		Each latch is associated with exactly one compoment. The latch
 *		will act as a read latch for all connected components, which will read
 *		from each of the twLatchOutputs objects.
 *	@returns
 *		(nothing, constructor).
 */
twLatch::twLatch( twComponent & component0, idx_t idx0 )
	: component( component0 ),
	  idx( idx0 ),
	  offset( 0 )
{
        qWarning( "Creating latch for component %p.\n", &component );
}

twLatch::~twLatch()
{
    // FIXME: Empty the latch output list.
}

/**
 *	@method twLatch.getFormat
 *		Native format of the data this latch produces.
 *	@desc
 *		Default: the canonical mono-Float32 exchange format at the environment
 *		sample rate. This is byte-identical to what every component has always
 *		produced, so unmodified latches keep behaving exactly as before.
 *		Producers emitting a different format override this. twLatch is a friend
 *		of twComponent, so it may read the component's environment directly.
 */
twFormat twLatch::getFormat() const
{
    return twCanonicalFormat( (std::uint32_t) component.env.getSRate() );
}

/**
 *	@method twLatch.addOutput
 *		Returns a latch output object from a latch.
 *	@synopsis twLatchOutput * twLatch::addOutput()
 *	@desc
 *		Every latch dispatches one component's output data to an
 *		arbitrary number of reading components.<br>
 *		Each of the reading components will connect to the source
 *		component by retrieving one output latch from the component
 *		using its <i>linkOutput()</i> method.
 *	@returns
 *		A pointer to the latch output object.
 */
twLatchOutput * twLatch::addOutput()
{
	twLatchOutput *pOutput = new twLatchOutput( *this );
	if( !pOutput ) throw excStandard( "twLatch::addOutput(): Unable to create new output." );

	outputList.append( pOutput );

	return pOutput;
}

int twLatch::deleteOutput( twLatchOutput * pOutput )
{
#ifdef DEBUG_COMPONENT
	if( !pOutput ) throw excStandard( "twLatch::deleteOutput(): pOutput was NULL." );
#endif
	outputList.removeOne( pOutput );
	delete pOutput;
	return 0;
}


