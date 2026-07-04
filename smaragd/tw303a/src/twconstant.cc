
#include <iostream>
#include "twsyslog.h"

#include "twconstant.h"
#include "io_vector.h"

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
	pOutputLatches_[0] = std::make_shared<twStreamingLatch>( *this, 0, 0 );
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

// Phase 3: IOVector-based interface (type-safe, page-backed rendering)
length_t twConstant::calcOutputTo( IOVector& dest, idx_t /* idx */ )
{
    // Direct fill using IOVector's fillConstant operation
    // No allocation or intermediate buffers needed
    return dest.fillConstant(0, dest.length(), constant);
}

twConstant::twConstant( tw303aEnvironment &env0, sample_t constant0 )
	: twComponent( env0 ), constant( constant0 )
{
}


