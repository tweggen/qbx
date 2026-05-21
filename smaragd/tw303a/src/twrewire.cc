
#include <stdlib.h>
#include "twrewire.h"

const char *twRewire::getInputName( idx_t ) const
{
    return "Rewire input";   
}

const char *twRewire::getOutputName( idx_t ) const
{
    return "Rewire output";
}

void twRewire::init()
{
    twComponent::init();
}

length_t twRewire::calcOutputTo( sample_t *pDest, length_t length, idx_t idx )
{
    length_t realRead;
    realRead = ((twLatchStreamingOutput *)pInputPlugs[idx]) -> readStreamingData(
            pDest, length );
    return realRead;
}

int twRewire::setNPlugs( idx_t n )
{
    twLatchOutput **newPlugs, **oldPlugs = pInputPlugs;

    // no change?
    if( pInputPlugs && n==nInputs_ ) return 0;

    if( n<0 ) {
        return -1;
    }
    if( n<nInputs_ ) {
        // Look, if the remaining inputs are empty.
        for( int i=n; n<nInputs_; n++ ) {
            if( pInputPlugs[i] ) return -2;
        }
    }
    newPlugs = (twLatchOutput **) ::calloc( sizeof( twLatchOutput * ), n );
    if( pInputPlugs ) {
        if( n<nInputs_ ) {
            ::memcpy( newPlugs, pInputPlugs, n*sizeof( twLatchOutput *) );
        } else {
            ::memcpy( newPlugs, pInputPlugs, nInputs_*sizeof( twLatchOutput *) );
        }
    }
    pInputPlugs = newPlugs;
    if( pInputPlugs ) {
        ::free( oldPlugs );
    }
    nInputs_ = n;
    // I do not have to reallocate the input/output plugs, as I do not
    // have own output latches.
    // FIXME: Emit something?
    return 0;
}

idx_t twRewire::getNInputs() const
{
    return nInputs_;
}

idx_t twRewire::getNOutputs() const
{
    return nInputs_;
}

/**
 * Overridden, because I do not posses own plugs.
 * twComponent ctor will call it, I will set the current number of 
 * plugs to my default one.
 */
void twRewire::allocPlugs()
{
    setNPlugs( getNInputs() );
}

/**
 * Overridden, because we do not need to create output latches.
 */
void twRewire::createOutputLatches()
{
    return;
}

twLatchOutput *twRewire::linkOutput( idx_t idx )
{
    if( idx<0 || idx>=nInputs_ ) return NULL;
    if( !pInputPlugs[idx] ) return NULL;    
    return pInputPlugs[idx]->getParentLatch().getComponent().linkOutput( 0 );
}

twRewire::~twRewire()
{    
}

twRewire::twRewire( tw303aEnvironment &env0 )
    : twComponent( env0 )
{
    setBufferSize( env.getBufferSize() );
    // Default
    nInputs_ = 1;
}
