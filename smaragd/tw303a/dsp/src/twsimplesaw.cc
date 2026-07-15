
#include <stdlib.h>
#include "tw/core/twsyslog.h"
#include <vector>

#include "tw/dsp/twsimplesaw.h"
#include <vector>
#include "tw/pages/io_vector.h"
#include <vector>

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
	pOutputLatches_[0] = std::make_shared<twStreamingLatch>( shared_from_this(), 0, 0 );
#ifdef DEBUG_COMPONENT
	fprintf( sterr, "twSimpleSaw::createOutputLatches(): leaving." );
#endif
}

// Phase 3: IOVector-based interface (type-safe, page-backed rendering)
length_t twSimpleSaw::calcOutputTo( IOVector& dest, idx_t /* idx */ )
{
	std::vector<sample_t> pDest(dest.length());
	sample_t *pCurr = pDest.data();
	sample_t *pCurrFreq = freqBuffer;
	length_t realRead;

	realRead = static_cast<twLatchStreamingOutput*>(pInputPlugs_[0].get())->readStreamingData(
		freqBuffer, dest.length());
	if( realRead == 0 ) {
		throw new excStandard( "twSimpleSaw::calcOutputTo(): Source did not provide data." );
	}

	offset_t a = currPos;
	offset_t b = a + realRead;

	for( offset_t i = a; i < b; i++ ) {
		unsigned currFreq;
		currFreq = *pCurrFreq++;
		if( currFreq == 0 ) {
			*pCurr++ = 0;
		} else {
			offset_t periodLength = ((env.getSRate() * 100) / currFreq);
			*pCurr++ = (((i % periodLength) << 16) / periodLength) - periodLength/2;
		}
	}
	currPos += realRead;

	// Write to IOVector destination
	return dest.copyFrom(IOVector::CreateFromBuffer(pDest.data(), realRead), 0, realRead);
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

