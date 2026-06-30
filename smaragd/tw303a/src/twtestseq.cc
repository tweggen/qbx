#if 0
#include <iostream>
#include "twsyslog.h"

#include "twtestseq.h"

sample_t freqTable[] = {
	52325, 55436, 58733, 62225, 65925, 69845,
	73999, 78399, 83061, 88000, 93233, 98776
};

static inline sample_t freq( short note )
{
	if( note==0 ) return 0;
	if( note<60 ) return freqTable[ note%12 ] >> (5-note/12);
	else  return freqTable[ note%12 ] << (note/12-5);
}

void twTestSeq::init()
{
	twComponent::init();
}

void twTestSeq::createOutputLatches()
{
	// use default buffer size
	pOutputLatches[0] = new twStreamingLatch( *this, 0, 0 );
}

// Phase 3: IOVector-based interface (type-safe, page-backed rendering)
length_t twTestSeq::calcOutputTo( IOVector& dest, idx_t idx )
{
	// Use temp buffer and delegate to raw-pointer version, then copy result
	sample_t *buffer = (sample_t *)alloca(dest.length() * sizeof(sample_t));
	length_t result = calcOutputTo(buffer, dest.length(), idx);
	return dest.copyFrom(IOVector::CreateFromBuffer(buffer, result), 0, result);
}

twTestSeq::twTestSeq( tw303aEnvironment &env0, sample_t constant0 )
	: twComponent( env0 ), constant( constant0 )
{
	currPos = 0;
	constant = prevConstant = freq( 12 );
}


twTestSeq::twTestSeq( tw303aEnvironment &env0, sample_t constant0, sample_t portamento0 )
	: twComponent( env0 ), constant( constant0 ), portamento( portamento0 )
{
	currPos = 0;
	constant = prevConstant = freq( 12 );
}

void twTestSeq::reset()
{
	// Test sequencer: reset playback position
	currPos = 0;
}
#endif
