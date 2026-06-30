#if 0
#include <stdlib.h>
#include "twsyslog.h"

#include "twsaw.h"

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

length_t twSaw::calcOutputTo( sample_t *pDest, length_t /* length */, idx_t /* idx*/ )
{
	int i, a, b;
	sample_t *pCurr = pDest;
	sample_t *pCurrFreq = freqBuffer;
	length_t realRead;
	sample_t smpl = smplState;
	sample_t smplErr = smplStateErr;

	realRead = ((twLatchStreamingOutput *)pInputPlugs[0]) -> readStreamingData(
		freqBuffer,
		env.getBufferSize()
		);
	if( realRead==0 ) {
		throw new excStandard( "twSimpleSaw::calcOutputTo(): Source did not provide data." );
	}
	

#ifdef DEBUG_CALCOUTPUT
	syslog( LOG_DEBUG, "twSimpleSaw::calOutputTo(): Starting at %d, calcing %d.", currPos, length );
#endif
	a = currPos;
	b = a+realRead;

	for( i=a; i<b; i++ ) {
		register unsigned currFreq;
		currFreq = *pCurrFreq++;
		if( currFreq==0 ) {
			// shut up on pCurrFreq = 0;
			*pCurr++ = 0;
			smpl = 0;
			smplErr = 0;
		} else {
			register offset_t sdiff = ampl*4096/((env.getSRate()*100) / currFreq) + smplErr;
	
			if( rampDown ) {
				*pCurr++ = maxVal-smpl;
			} else {
				*pCurr++ = smpl+minVal;
			}
//			cout << smpl <<endl;
			smpl = (smpl+(sdiff>>12))%ampl;
			smplErr = sdiff&0xfff;
		}

//		if( (i & 0x1f) == 0 ) cout << currFreq << " " << pCurr[-1] << endl;
	}
	currPos += realRead;
	smplState = smpl;
	smplStateErr = smplErr;
	return realRead;
}

// Phase 3: IOVector-based interface (type-safe, page-backed rendering)
length_t twSaw::calcOutputTo( IOVector& dest, idx_t idx )
{
	// Use temp buffer and delegate to raw-pointer version, then copy result
	sample_t *buffer = (sample_t *)alloca(dest.length() * sizeof(sample_t));
	length_t result = calcOutputTo(buffer, dest.length(), idx);
	return dest.copyFrom(IOVector::CreateFromBuffer(buffer, result), 0, result);
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