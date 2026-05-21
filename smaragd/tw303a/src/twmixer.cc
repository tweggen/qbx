
#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>
#include <string.h>
#include <math.h>

#include "twmixer.h"

const char *twMixer::getInputName( idx_t ) const
{
    return "Signal input";
}

const char *twMixer::getOutputName( idx_t ) const
{
    return "Signal sum";
}

void twMixer::init()
{
	twComponent::init();
}

void twMixer::createOutputLatches()
{
#ifdef DEBUG_COMPONENT
	syslog( LOG_DEBUG, "twMixer::createOutputLatches(): entered." );
#endif
	// use default buffer size
#ifdef DEBUG_COMPONENT
	syslog( LOG_DEBUG, "twMixer::createOutputLatches(): creating streaming latch..." );
#endif
	pOutputLatches[0] = new twStreamingLatch( *this, 0, 0 );
#ifdef DEBUG_COMPONENT
	syslog( LOG_DEBUG, "twMixer::createOutputLatches(): leaving." );
#endif
}

length_t twMixer::calcOutputTo( sample_t *pDest, length_t length, idx_t /* idx */ )
{
    length_t realRead = 0;
    InputProperties *props = inputProperties_;
    
    // delete destination buffer (our accu)
    memset( pDest, 0, sizeof( sample_t ) * length );
    // add channels
    for( idx_t ch=0; ch<mixerInputs_; ch++, props++ ) {        
        if( !pInputPlugs[ch] ) continue;
        float factor = props->volumeFactor_;
//        qWarning( "twMixer::Calcing channel %d. input plug $%08x. factor = %d", 
//                  ch, pInputPlugs[ch], (int) factor );
        realRead = ((twLatchStreamingOutput *)pInputPlugs[ch])->readStreamingData( inBuffer, length );
        if( realRead!=length ) {
            throw new excStandard( "twMixer::calcOutputTo(): Source did not provide sufficient data." );
        }
        sample_t *pCurr = pDest, *pSrc = inBuffer;
        for( offset_t i=0; i<(offset_t)length; i++ ) {
            *pCurr++ += *pSrc++ * factor;
        }
    }
    
    return realRead;
}

/**
 * Change the level of a given input.
 */
int twMixer::setInputLevel( idx_t i, double volume )
{
    if( i>=mixerInputs_ ) {
        fprintf( stderr, "Unable to set input level of %d>%d.\n",
                 i, mixerInputs_ );
        return -1;
    }
    if( !inputProperties_ ) {
        fprintf( stderr, "Unable to set input level of %d, no input properties "
                 "allocated yet.\n",
                 i );
        return -1;
    }
    inputProperties_[i].volume_ = volume;
    inputProperties_[i].volumeFactor_ = 
        (sample_t) pow( 10., volume/20. );
    // FIXME: Set something to enable float/fixed calculation.
    // fprintf( stderr, "Volume set to %f db, volume factor set to $%x.\n",
    //         inputProperties_[i].volume_, 
    //         inputProperties_[i].volumeFactor_ );
    return 0;
}

/**
 * Change the number of mixer inputs.
 * The number of channels can only be shrunken, if there is nothing connected.
 * Technically, the number of inputs never becomes smaller.
 */
int twMixer::setNInputs( idx_t n )
{
    if( n<=0 ) return -2;    
    if( n<=mixerInputs_ ) {
        // FIXME: Decrease the actual number of channels
        // connected, but don't fool around
        // in the data structures.
        return 0;
    }
    twLatchOutput **newInputPlugs = (twLatchOutput **) ::calloc (sizeof (twLatchOutput *), n );
    if( !newInputPlugs ) return -1; 
    if( mixerInputs_ ) {        
        memcpy( newInputPlugs, pInputPlugs, sizeof( twLatchOutput *)*mixerInputs_ );
    }
    twLatchOutput **oldPlugs = pInputPlugs;
    pInputPlugs = newInputPlugs;   
    mixerInputs_ = n;
    ::free( oldPlugs );

    // (Re)alloc input properties.
    if( !inputProperties_ ) {
        inputProperties_ = (InputProperties *) ::calloc( n, sizeof( InputProperties ) );
    } else {
        inputProperties_ = (InputProperties *) ::realloc( inputProperties_, n*sizeof( InputProperties ) );
    }
    return 0;
}

void twMixer::setBufferSize( length_t /* len */ )
{
    if( inBuffer ) free( inBuffer );
    
    // FIXME: Why do we read buffer size from environment?
    inBuffer = (sample_t *) calloc( sizeof(sample_t), env.getBufferSize() );
    if( !inBuffer ) {
        throw excStandard( "twMixer::setBufferSize(): Not enough memory for mixer input channels." );
    }
}

twMixer::twMixer( tw303aEnvironment &env0, idx_t inputs )
    : twComponent( env0 ), mixerInputs_(inputs), inputProperties_( NULL )
{
	// alloc frequency buffer
	inBuffer = NULL;
	setBufferSize( env.getBufferSize() );
        // FIXME: This also is written in setNChannels
        inputProperties_ = (InputProperties *) ::calloc( inputs, sizeof( InputProperties ) );
}

