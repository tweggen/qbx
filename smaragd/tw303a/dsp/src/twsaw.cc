#if 0
#include <stdlib.h>
#include "tw/core/twsyslog.h"
#include <vector>

#include "tw/dsp/twsaw.h"
#include <vector>

#undef DEBUG_CALC_OUTPUT

void twSaw::init()
{
    twOsc::init();
}

void twSaw::createOutputLatches()
{
#ifdef DEBUG_COMPONENT
	syslog( LOG_DEBUG, "twSimpleSaw::createOutputLatches(): entered." );
#endif
	// use default buffer size
#ifdef DEBUG_COMPONENT
	syslog( LOG_DEBUG, "twSimpleSaw::createOutputLatches(): creating streaming latch..." );
#endif
	pOutputLatches[0] = new twStreamingLatch( *this, 0, 0 );
#ifdef DEBUG_COMPONENT
	syslog( LOG_DEBUG, "twSimpleSaw::createOutputLatches(): leaving." );
#endif
}

// Phase 3: IOVector-based interface (type-safe, page-backed rendering)
length_t twSaw::calcOutputTo( IOVector& dest, idx_t idx )
{
	// Use temp buffer and delegate to raw-pointer version, then copy result
	std::vector<sample_t> buffer(dest.length());
	length_t result = calcOutputTo(buffer, dest.length(), idx);
	return dest.copyFrom(IOVector::CreateFromBuffer(buffer.data(), result), 0, result);
}

void twSaw::__init( )
{
	// start at time 0
	currPos = 0;
	// alloc frequency buffer
	freqBuffer = (sample_t *) calloc( env.getBufferSize(), sizeof( sample_t ) );
	if( !freqBuffer ) {
		throw excStandard( "twSimpleSaw::twSimpleSaw(): Not enough memory for frequency buffer." );
	}
	smplState = 0;
	smplStateErr = 0;
	ampl = maxVal-minVal;
	if( !ampl ) ampl = 1;
}

twSaw::twSaw( tw303aEnvironment &env0 )
	: twOsc( env0 )
{
	minVal = -32768;
	maxVal = 32767;
	rampDown = 0;
	__init();
}

twSaw::twSaw( tw303aEnvironment &env0, sample_t minVal0, sample_t maxVal0 )
	: twOsc( env0 )
{
	minVal = minVal0;
	maxVal = maxVal0;
	if( minVal > maxVal ) {
		sample_t h = maxVal;
		maxVal = minVal;
		minVal = h;
		rampDown = 1;
	} else {
		rampDown = 0;
	}
	ampl = maxVal-minVal;
	__init();
}
#endif