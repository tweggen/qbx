
#include <stdlib.h>
#include "twsyslog.h"

#include "twpipe.h"
#include "io_vector.h"

void twPipe::init()
{
	twComponent::init();
}

void twPipe::createOutputLatches()
{
	// use default buffer size
	pOutputLatches[0] = new twStreamingLatch( *this, 0, 0 );
}

// Phase 3: IOVector-based interface (type-safe, page-backed rendering)
length_t twPipe::calcOutputTo( IOVector& dest, idx_t /* idx */ )
{
	// Allocate output buffer
	sample_t *pDest = (sample_t *)alloca(dest.length() * sizeof(sample_t));
	length_t realRead;

	// Shift delay line buffer (move samples left, new input goes at end)
	memcpy( inBuffer, inBuffer+env.getSRate(), sizeof( sample_t ) * dest.length() );

	// Read new input data
	realRead = ((twLatchStreamingOutput *)pInputPlugs[0])->readStreamingData(
		inBuffer+env.getSRate(), dest.length());
	if( realRead == 0 ) {
		throw new excStandard( "twPipe::calcOutputTo(): Source did not provide data." );
	}

	// Apply delay-line filter with weighted taps
	offset_t i;
	sample_t *pSrc = inBuffer + env.getSRate();
	sample_t *pOut = pDest;
	for( i = 0; i < (offset_t)realRead; i++ ) {
		*pSrc = *pOut++ =
			 (*pSrc*12 + (pSrc[-1310])*4 + (pSrc[-4561])*5 + (pSrc[-3364])*5
			+(pSrc[-11710])*3 + (pSrc[-12561])*3 + (pSrc[-6364])*4) / (12+4+5+5+3+3+4);
		pSrc++;
	}

	// Write to IOVector destination
	return dest.copyFrom(IOVector::CreateFromBuffer(pDest, realRead), 0, realRead);
}

void twPipe::setBufferSize( length_t length )
{
	if( inBuffer ) free( inBuffer );
	inBuffer = (sample_t *) calloc( sizeof(sample_t), length+env.getSRate() );
	if( !inBuffer ) {
		throw excStandard( "twPipe::setBufferSize(): Not enough memory for mixer input channels." );
	}
}

twPipe::twPipe( tw303aEnvironment &env0 )
	: twComponent( env0 )
{
	// alloc frequency buffer
	inBuffer = NULL;
	setBufferSize( env.getBufferSize() );
}

void twPipe::reset()
{
	// Stateless pass-through: nothing to reset
}


