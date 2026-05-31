
#include <stdlib.h>

#include "twmoog.h"

void twMoog::setBufferSize( length_t /* len */ )
{
	if( freqBuffer ) free( freqBuffer );

	freqBuffer = (sample_t *) calloc( sizeof(sample_t), env.getBufferSize() );
	if( !freqBuffer ) {
		throw excStandard( "twMoog::setBufferSize(): Not enough memory for mixer input channels." );
	}
}

twMoog::twMoog( tw303aEnvironment &env0,  double reso )
	: twComponent( env0 )
{
	resonance = reso;
	freqBuffer = NULL;
	setBufferSize( env.getBufferSize() );
}

void twMoog::init()
{
	twComponent::init();
	out1 = out2 = out3 = out4 = in1 = in2 = in3 = in4 = 0.0;
}

void twMoog::createOutputLatches()
{
	pOutputLatches[0] = new twStreamingLatch( *this, 0, 0 );
}

length_t twMoog::calcOutputTo( sample_t *pDest, length_t length, idx_t /* idx */ )
{
	length_t realRead;
	sample_t *pCurr = pDest;
	sample_t *pFreq = freqBuffer;

	// read data to destination
	realRead = ((twLatchStreamingOutput *)pInputPlugs[0]) -> readStreamingData(
		pDest,
		length
		);
	if( realRead != length ) {
		throw new excStandard( "twMoog::calcOutputTo(): Sources did not provide sufficient data." );
	}

	realRead = ((twLatchStreamingOutput *)pInputPlugs[1]) -> readStreamingData(
		freqBuffer,
		length
		);
	if( realRead != length ) {
		throw new excStandard( "twMoog::calcOutputTo(): Sources did not provide sufficient data." );
	}

    double freqCorrect = 1.16 / (double)(env.getSRate()/2);
	
	for( offset_t i=0; i<(offset_t)realRead; i++ ) {
		double input = (double) *pCurr;
		//input *=4;
		double f = ((double)(*pFreq++))  * freqCorrect;
//		double f = ((double)(10000))  * freqCorrect;
		double res = (double)resonance;
		double fb = res * (1.0 - 0.15 * f * f);

		input -= out4 * fb;
		input *= 0.35013 * (f*f)*(f*f);

		out1 = input + 0.3 * in1 + (1. - f) * out1; // Pole 1
		in1  = input;
		out2 = out1 + 0.3 * in2 + (1. - f) * out2;  // Pole 2
		if( out1>32767.0 ) out1 = 32767.0; 
		else if( out1<-32768.0 ) out1 = -32768.0; 
		in2  = out1;

		out3 = out2 + 0.3 * in3 + (1. - f) * out3;  // Pole 3
		if( out2>32767.0 ) out2 = 32767.0;
		else if( out2<-32768.0 ) out2 = -32768.0; 
		in3  = out2;

		out4 = out3 + 0.3 * in4 + (1. - f) * out4;  // Pole 4
		if( out3>32767.0 ) out3 = 32767.0;
		else if( out3<-32768.0 ) out3 = -32768.0; 
		in4  = out3;

		if( out4>32767.0 ) out4 = 32767.0;
		else if( out4<-32768.0 ) out4 = -32768.0;

		//cout << *pCurr << " " << (sample_t) out4 << endl;
		*pCurr++ = (sample_t)out4;
	}
	return realRead;
}
