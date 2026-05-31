
#include <stdlib.h>
#include "twsyslog.h"

#include "twpipe.h"

void twPipe::init()
{
	twComponent::init();
}

void twPipe::createOutputLatches()
{
	// use default buffer size
	pOutputLatches[0] = new twStreamingLatch( *this, 0, 0 );
}

length_t twPipe::calcOutputTo( sample_t *pDest, length_t length, idx_t /* idx */ )
{
	length_t realRead;

	memcpy( inBuffer, inBuffer+env.getSRate(), sizeof( sample_t ) * length );
	realRead = ((twLatchStreamingOutput *)pInputPlugs[0]) -> readStreamingData(
		inBuffer+env.getSRate(),
		length
		);
	if( realRead==0 ) {
		throw new excStandard( "twPipe::calcOutputTo(): Source did not provide data." );
	}

	// processData( pDest, realRead ); 
	offset_t i;
	sample_t *pSrc = inBuffer+env.getSRate();
	for( i=0; i<(offset_t)length; i++ ) {
		*pSrc = *pDest++ =
			 (*pSrc*12 + (pSrc[-1310])*4 + (pSrc[-4561])*5 + (pSrc[-3364])*5
			+(pSrc[-11710])*3 + (pSrc[-12561])*3 + (pSrc[-6364])*4) / (12+4+5+5+3+3+4);
		pSrc++;
	}

	return realRead;
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

