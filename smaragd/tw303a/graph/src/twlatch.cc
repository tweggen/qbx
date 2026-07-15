
#include <stdlib.h>
#include <memory.h>
#include <string.h>
#include <algorithm>

#include "tw/core/twsyslog.h"

#include "tw/graph/twcomponent.h"

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
twLatch::twLatch( std::shared_ptr<twComponent> component0, idx_t idx0 )
	: component( component0 ),
	  idx( idx0 ),
	  offset( 0 )
{
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
    // component is a weak_ptr (non-owning back-ref); lock to read the env.
    std::shared_ptr<twComponent> comp = component.lock();
    std::uint32_t srate = comp ? (std::uint32_t) comp->env.getSRate() : 0;
    return twCanonicalFormat( srate );
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
	auto pOutput = std::make_shared<twLatchOutput>( *this );

	outputList.push_back( pOutput );

	// The caller wires with a raw pointer (linkOutput/setInput signatures); the
	// consumer takes shared ownership via sharedOutput() in twComponent::setInput.
	return pOutput.get();
}

int twLatch::deleteOutput( twLatchOutput * pOutput )
{
#ifdef DEBUG_COMPONENT
	if( !pOutput ) throw excStandard( "twLatch::deleteOutput(): pOutput was NULL." );
#endif
	auto it = std::find_if( outputList.begin(), outputList.end(),
		[pOutput]( const std::shared_ptr<twLatchOutput> &p ) { return p.get() == pOutput; } );
	// Drop the latch's reference. If a consumer (or an audio-thread snapshot)
	// still holds the plug, the object survives until that reference is released.
	if( it != outputList.end() ) outputList.erase( it );
	return 0;
}

std::shared_ptr<twLatchOutput> twLatch::sharedOutput( twLatchOutput * pOutput )
{
	auto it = std::find_if( outputList.begin(), outputList.end(),
		[pOutput]( const std::shared_ptr<twLatchOutput> &p ) { return p.get() == pOutput; } );
	return it != outputList.end() ? *it : nullptr;
}


