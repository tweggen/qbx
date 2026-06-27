
#include <iostream>
#include "twsyslog.h"

#include "twconstant.h"

void twConstant::reset()
{
	// Stateless component: nothing to reset
}

void twConstant::init()
{
	twComponent::init();
}

void twConstant::createOutputLatches()
{
	pOutputLatches[0] = new twStreamingLatch( *this, 0, 0 );
}

length_t twConstant::renderFrames(sample_t *output, length_t length,
                                   const sample_t *input, length_t inputLength,
                                   idx_t idx)
{
    // Tier 2 Enhancement #3: Direct render for stateless sources
    // twConstant ignores input and just fills output with constant value
    // Faster than calcOutputTo() because no latch access needed
    (void)input;
    (void)inputLength;

    for (length_t i = 0; i < length; ++i) {
        output[i] = constant;
    }
    return length;
}

length_t twConstant::calcOutputTo( sample_t *pDest, length_t length, idx_t /* idx */ )
{
    int i; //, a, b;
    sample_t *pCurr = pDest;

    for( i=0; i<length; i++ ) {
        *pCurr++ = constant;
    }
    return length;
}

twConstant::twConstant( tw303aEnvironment &env0, sample_t constant0 )
	: twComponent( env0 ), constant( constant0 )
{
}


