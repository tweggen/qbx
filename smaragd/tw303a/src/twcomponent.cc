#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <string.h>

#include <iostream>

#include <syslog.h>

#include "twcomponent.h"

#define DEBUG_COMPONENT

offset_t twComponent::tellPos() const
{
    return 0;
}

bool twComponent::isSeekable() const
{
    return false;
}

int twComponent::seekTo( offset_t )
{
    return -1;
}

void twComponent::allocPlugs()
{
    if( getNInputs()>0 ) {
        pInputPlugs = (twLatchOutput **) ::calloc (sizeof (twLatchOutput *), getNInputs() );
        if( !pInputPlugs ) {
            throw excStandard(
                "twComponent::twComponent(): "
                "Not enough memory to create input plug pointer table." );
        }
    } else {
        pInputPlugs = NULL;
    }
    if( getNOutputs()>0 ) {
        pOutputLatches = (twLatch **)  calloc (sizeof (twLatch *), getNOutputs() );
        if( !pOutputLatches ) {
            throw excStandard(
                "twComponent::twPlugs(): "
                "Not enough memory to create output plug pointer table." );
        }
    } else {
        pOutputLatches = NULL;
    }
}

void twComponent::init()
{
#ifdef DEBUG_COMPONENT
    syslog( LOG_DEBUG, "twComponent::init(): entered." );
#endif
    
    allocPlugs();
    createOutputLatches();
    
#ifdef DEBUG_COMPONENT
    syslog( LOG_DEBUG, "twComponent::init(): leaving." );
#endif
}

/**
 *	@method twComponent.linkOutput
 *		Connect to a components output.
 *	@synopsis twLatchOutput *twComponent::linkOutput(
 *      idx_t idx )
 *	@param </i><tt>idx_t</tt><i>idx
 *		The index of the output to connect to.
 *	@desc
 *		Every component has an arbitrary number of outputs.
 *		An arbitrary number of components may connect to this
 *		output by using the twLatchOutput returned by this function.
 *	@returns
 *		A pointer to the twLatchObject, which can be used to retrieve data.
 *		or <br>NULL, if an error occured.
 */
twLatchOutput * twComponent::linkOutput( idx_t idx )
{
    return pOutputLatches[idx]->addOutput();
}

void twComponent::setInput( idx_t idx, twLatchOutput *newOutput )
{
    if( pInputPlugs[idx] ) {
        pInputPlugs[idx]->getParentLatch().deleteOutput( pInputPlugs[idx] );
        pInputPlugs[idx] = NULL;
        if( !newOutput ) --inputsSet_;
    } else {
        if( newOutput ) ++inputsSet_;
    }
    pInputPlugs[idx] = newOutput;
}

/**
 * Use of this method is dangerous. However, it allows nice things.
 * take care of dangling references.
 */
twLatchOutput *twComponent::getInputPlug( idx_t idx ) const
{
    if( idx<0 || idx>getNInputs() ) return NULL;
    return pInputPlugs[idx];
} 

/**
 * Initialize everything needed for playback operation.
 * Subclasses should call their parent classes' implementation.
 */
int twComponent::doInitOperation( int /* initId */ ) 
{
    return 0;
}

int twComponent::initOperation( int initId )
{
    int oldId = currentOperation_;
    if( initId == oldId ) return 0;
    currentOperation_ = initId;
    int res = doInitOperation( initId );
    if( res<0 ) {
        currentOperation_ = oldId;
        return res;
    }
    return 0;
}

twComponent::~twComponent ()
{
    printf( "twComponent dtor.\n" );
    if( pInputPlugs ) free( pInputPlugs );
    if( pOutputLatches ) free( pOutputLatches );
}

/**
 *	@method twComponent.twComponent
 *		Constructor for twComponent object.
 *	@synopsis twComponent::twComponent()
 *	@desc
 *		Every part of the synthesis network is a component. In C++, all components
 *		are derived from the twComponent class.
 *		Each twComponent object has several inputs and several outputs.
 *		Every input is a pointer to a twLatchOutput object, every output is a
 *		pointer to a twLatch object.
 *		<br>Each component will allocate pointers for the input/output table,
 *		any derived class will have to initialize all of the objects,
 *		as we do not know, which kind of latches will be used.
 *	@returns
 *		(nothing, constructor)
 */
twComponent::twComponent( tw303aEnvironment &env0 )
    : currentOperation_( -1 ),
      inputsSet_(0),
      env( env0 ),
      pOutputLatches(0),
      pInputPlugs(0)
{    
}
