
#include <iostream>
#include "twsyslog.h"

#include "twconstant.h"

void twConstant::init()
{
	twComponent::init();
}

void twConstant::createOutputLatches()
{
	pOutputLatches[0] = new twStreamingLatch( *this, 0, 0 );
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


