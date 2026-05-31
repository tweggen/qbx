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

length_t twTestSeq::calcOutputTo( sample_t *pDest, length_t length, idx_t /*idx*/ )
{
    int i; //, a, b;
    sample_t *pCurr = pDest;
    
    int nFreqs = 256;
    sample_t noteTable[256] = {
        
        12, 12, 24, 12, 12, 24, 12, 12,
        24, 12, 12, 24, 12, 12, 24, 12,
        12, 12, 24, 12, 12, 24, 12, 12,
        24, 12, 12, 24, 12, 12, 24, 12,
        12, 12, 24, 12, 12, 24, 12, 12,
        24, 12, 12, 24, 12, 12, 24, 12,
        12, 12, 24, 12, 12, 24, 12, 12,
        24, 12, 0,  0,  0,  0,  0,  0, 
        
        12, 12, 24, 12, 12, 24, 12, 12,
        15, 15, 27, 15, 17, 36, 17, 48,
        12, 12, 24, 12, 12, 24, 12, 12,
        15, 15, 27, 15, 17, 36, 17, 48,
        12, 12, 24, 12, 12, 24, 12, 12,
        15, 15, 27, 15, 17, 36, 17, 48,
        12, 12, 24, 12, 12, 24, 12, 12,
        15, 15, 27, 15, 17, 36, 17, 48,
        
        12, 12, 24, 12, 12, 24, 12, 12,
        15, 15, 27, 15, 17, 36, 17, 48,
        12, 12, 24, 12, 12, 24, 12, 12,
        15, 15, 27, 15, 17, 36, 17, 48,
        12, 12, 24, 12, 12, 24, 12, 12,
        15, 15, 27, 15, 17, 36, 17, 48,
        12, 12, 24, 12, 12, 24, 12, 12,
        15, 15, 27, 15, 17, 36, 17, 48,
        
        12, 12, 24, 12, 12, 24, 12, 12,
        15, 15, 27, 15, 17, 36, 17, 48,
        12, 12, 24, 12, 12, 24, 12, 12,
        15, 15, 27, 15, 17, 36, 17, 48,
        12, 12, 24, 12, 12, 24, 12, 12,
        15, 15, 27, 15, 17, 36, 17, 48,
        12, 12, 24, 12, 12, 24, 12, 12,
        15, 15, 27, 15, 17, 36, 17, 48
        
//		 9+36,  0,  9+36,  0, 16+36,  0, 12+36,  0,
//	 	17+36,  0, 12+36,  0, 16+36,  0, 12+36,  0,
//		14+36,  0, 14+36,  0, 21+36,  0, 17+36,  0,
//		22+36,  0, 17+36,  0, 21+36,  0, 17+36,  0,
//		 9+36,  0,  9+36,  0, 16+36,  0, 12+36,  0,
//	 	17+36,  0, 12+36,  0, 16+36,  0, 12+36,  0,
//		 9+36,  0,  9+36,  0,  0   ,  0,  9+36,  0,
//		 9+36,  0,  0   ,  0,  0   ,  0,     0,  0
    };
    int freqLength = (env.getSRate()*60)/(120*4);
    
    for( i=0; i<length; i++ ) {
        constant = freq( noteTable[ ((currPos/freqLength)%nFreqs) ] );
        // yo, great for simplesaw
        constant = (prevConstant*portamento+constant*(4096-portamento))/4096;
        prevConstant = constant;
        
        if( currPos%freqLength > 4500 ) *pCurr++ = 0;
        else *pCurr++ = constant;
        currPos++;
    }
    return length;
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
#endif
