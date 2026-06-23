
#include <stdlib.h>
#include "twsyslog.h"

#include "twwhitenoise.h"

#undef DEBUG_CALC_OUTPUT

void twWhiteNoise::init()
{
    twOsc::init();
}

void twWhiteNoise::createOutputLatches()
{
#ifdef DEBUG_COMPONENT
    fprintf( sterr, "twWhiteNoise::createOutputLatches(): entered." );
#endif
    // use default buffer size
#ifdef DEBUG_COMPONENT
    fprintf( sterr, "twWhiteNoise::createOutputLatches(): creating streaming latch..." );
#endif
    pOutputLatches[0] = new twStreamingLatch( *this, 0, 0 );
#ifdef DEBUG_COMPONENT
    fprintf( sterr, "twWhiteNoise::createOutputLatches(): leaving." );
#endif
}

length_t twWhiteNoise::calcOutputTo( sample_t *pDest, length_t, idx_t )
{
    int i, a, b;
    sample_t *pCurr = pDest;
    sample_t *pCurrFreq = freqBuffer;
    length_t realRead;
    
    realRead = ((twLatchStreamingOutput *)pInputPlugs[0])->readStreamingData(
        freqBuffer,
        env.getBufferSize() );
    if( realRead==0 ) {
        throw new excStandard( "twWhiteNoise::calcOutputTo(): Source did not provide data." );
    }
    
#ifdef DEBUG_CALCOUTPUT
    fprintf( sterr, "twWhiteNoise::calOutputTo(): Starting at %d, calcing %d.", currPos, length );
#endif
    a = currPos;
    b = a+realRead;
    
    for( i=a; i<b; i++ ) {
        if( *pCurrFreq++  ) {
            *pCurr++ = (rand() & 0xffff)-0x8000;
        } else {
            // shut up on pCurrFreq = 0;
            *pCurr++ = 0;
        }
    }
    currPos += realRead;
    return realRead;
}

void twWhiteNoise::setBufferSize( length_t )
{
    if( freqBuffer ) free( freqBuffer );
    
    freqBuffer = (sample_t *) calloc( sizeof(sample_t), env.getBufferSize() );
    if( !freqBuffer ) {
        throw excStandard( "twWhiteNoise::setBufferSize(): Not enough memory for mixer input channels." );
    }
}

twWhiteNoise::twWhiteNoise( tw303aEnvironment &env0 ) 
    : twOsc( env0 )
{
    // start at time 0
    currPos = 0;
    // alloc frequency buffer
    freqBuffer = NULL;
    setBufferSize( env.getBufferSize() );
}
