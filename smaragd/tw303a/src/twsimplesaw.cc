
#include <stdlib.h>
#include "twsyslog.h"

#include "twsimplesaw.h"

#undef DEBUG_CALC_OUTPUT

void twSimpleSaw::init()
{
	twOsc::init();
}

void twSimpleSaw::createOutputLatches()
{
#ifdef DEBUG_COMPONENT
	fprintf( sterr, "twSimpleSaw::createOutputLatches(): entered." );
#endif
	// use default buffer size
#ifdef DEBUG_COMPONENT
	fprintf( sterr, "twSimpleSaw::createOutputLatches(): creating streaming latch..." );
#endif
	pOutputLatches[0] = new twStreamingLatch( *this, 0, 0 );
#ifdef DEBUG_COMPONENT
	fprintf( sterr, "twSimpleSaw::createOutputLatches(): leaving." );
#endif
}

length_t twSimpleSaw::calcOutputTo( sample_t *pDest, 
                                    length_t /* length*/, idx_t /* idx */ )
{
	int i, a, b;
	sample_t *pCurr = pDest;
	sample_t *pCurrFreq = freqBuffer;
	length_t realRead;

	realRead = ((twLatchStreamingOutput *)pInputPlugs[0]) -> readStreamingData(
		freqBuffer,
		env.getBufferSize()
		);
	if( realRead==0 ) {
		throw new excStandard( "twSimpleSaw::calcOutputTo(): Source did not provide data." );
	}
	

#ifdef DEBUG_CALCOUTPUT
	fprintf( sterr, "twSimpleSaw::calOutputTo(): Starting at %d, calcing %d.", currPos, length );
#endif
	a = currPos;
	b = a+realRead;

	for( i=a; i<b; i++ ) {
		unsigned currFreq;
		currFreq = *pCurrFreq++;
		if( currFreq==0 ) {
			// shut up on pCurrFreq = 0;
			*pCurr++ = 0;
		} else {
			offset_t periodLength = ((env.getSRate()*100) / currFreq);

//			this was the cool line
			*pCurr++ = (((i % periodLength) << 16) / periodLength)-periodLength/2;
//			*pCurr++ = (((i % periodLength) << 16) / periodLength)-0x8000;
		}
	}
	currPos += realRead;
	return realRead;
}

void twSimpleSaw::setBufferSize( length_t /*len*/ )
{
	if( freqBuffer ) free( freqBuffer );

	freqBuffer = (sample_t *) calloc( sizeof(sample_t), env.getBufferSize() );
	if( !freqBuffer ) {
		throw excStandard( "twSimpleSaw::setBufferSize(): Not enough memory for mixer input channels." );
	}
}

twSimpleSaw::twSimpleSaw( tw303aEnvironment &env0 ) 
	: twOsc( env0 )
{
	// start at time 0
	currPos = 0;
	// alloc frequency buffer
	freqBuffer = NULL;
	setBufferSize( env.getBufferSize() );
}

