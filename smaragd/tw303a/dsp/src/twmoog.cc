
#include <stdlib.h>

#include "tw/dsp/twmoog.h"
#include <vector>
#include "tw/pages/io_vector.h"
#include <vector>

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
	pOutputLatches_[0] = std::make_shared<twStreamingLatch>( *this, 0, 0 );
}

// Phase 3: IOVector-based interface (type-safe, page-backed rendering)
length_t twMoog::calcOutputTo( IOVector& dest, idx_t /* idx */ )
{
	// Read inputs into temp buffers
	std::vector<sample_t> audioBuffer(dest.length());
	std::vector<sample_t> freqBuffer_tmp(dest.length());

	// Read audio input
	length_t realRead = static_cast<twLatchStreamingOutput*>(pInputPlugs_[0].get())->readStreamingData(
		audioBuffer.data(), dest.length());
	if( realRead != dest.length() ) {
		throw new excStandard( "twMoog::calcOutputTo(): Audio source did not provide sufficient data." );
	}

	// Read frequency input
	realRead = static_cast<twLatchStreamingOutput*>(pInputPlugs_[1].get())->readStreamingData(
		freqBuffer_tmp.data(), dest.length());
	if( realRead != dest.length() ) {
		throw new excStandard( "twMoog::calcOutputTo(): Frequency source did not provide sufficient data." );
	}

	// Apply Moog filter DSP to audio, write to IOVector output
	double freqCorrect = 1.16 / (double)(env.getSRate()/2);
	sample_t *pCurr = audioBuffer.data();
	sample_t *pFreq = freqBuffer_tmp.data();

	for( offset_t i = 0; i < (offset_t)realRead; i++ ) {
		double input = (double) *pCurr;
		double f = ((double)(*pFreq++)) * freqCorrect;
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

		*pCurr++ = (sample_t)out4;
	}

	// Copy result to IOVector destination
	return dest.copyFrom(IOVector::CreateFromBuffer(audioBuffer.data(), realRead), 0, realRead);
}

