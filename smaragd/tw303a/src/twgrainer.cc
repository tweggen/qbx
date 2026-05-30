
#include <stdlib.h>

#include "twgrainer.h"

bool twGrainer::isSeekable() const
{
    return true;
}

offset_t twGrainer::tellPos() const
{
    return pos_;
}

int twGrainer::seekTo( offset_t pos ) 
{
    pos_ = pos;
    return 0;
}

int twGrainer::findValidGrain( offset_t fromPos, int fromIdx,
                               offset_t *alreadyDone )
{
    (void) fromPos; (void) alreadyDone;
    int n = grainSpec_->getNGrains();
    for( ;fromIdx<n;fromIdx++ ) {
        twSingleGrainSpec *curr = grainSpec_->getGrain( fromIdx );
        (void) curr;
    }
    return -1;
}

/**
 * Caculate the grain data to a given destination.
 */
length_t twGrainer::calcOutputTo( sample_t *pDest, length_t length, idx_t idx )
{
    length_t toGo = length;
    twSingleGrainSpec *sgFirst = NULL;
    twSingleGrainSpec *sgSecond = NULL;
    // If the first/second grainspec are valid
    length_t firstToGo = 0, secondToGo = 0;
    offset_t firstDone, secondDone;
    int firstIdx, secondIdx;
    length_t thisToGo;
    int startIdx = -1;
    offset_t currPos;
    // FIXME: This function ignores pitches.        
    sample_t *currPtr = pDest;
    sample_t *firstBuffer = (sample_t *) 
        alloca( env.getBufferSize()*sizeof( sample_t ) );
    sample_t *secondBuffer = (sample_t *)
        alloca( env.getBufferSize()*sizeof( sample_t ) );

    // No matter what we do, we need to clear output memory first.
    ::memset( pDest, 0, length*sizeof( sample_t ) );
    if( !sourceComponent_ || !grainSpec_ ) {
        return length;
    }

    // FIXME: The following implementation is one of the most inefficient
    // you could imagine.

    currPos = pos_;
    startIdx = -1;
    while( toGo > 0 ) {
        if( firstToGo <= 0 ) {
            firstIdx = findValidGrain( /* grainSpec_ ,*/ currPos, startIdx, &firstDone );
            if( firstIdx>=0 ) {
                sgFirst = grainSpec_->getGrain( firstIdx );
                firstToGo = sgFirst->length_ - firstDone;
                startIdx = firstIdx+1;
                firstDone = 0;
                sourceComponent_->seekTo( sgFirst->startOffset_ );
                sourceComponent_->calcOutputTo( firstBuffer, firstToGo, idx );
            }
        }
        if( !(secondToGo <= 0) ) {
            secondIdx = findValidGrain( /* grainSpec_, */ currPos, startIdx, &secondDone );
            if( secondIdx>=0 ) {
                sgSecond = grainSpec_->getGrain( secondIdx );
                secondToGo = sgSecond->length_ - secondDone;
                startIdx = secondIdx+1;
                sourceComponent_->seekTo( sgSecond->startOffset_ );
                sourceComponent_->calcOutputTo( secondBuffer, secondToGo, idx );
            }
        }
        if( firstToGo<=0 && secondToGo<=0 ) break;
        thisToGo = toGo;
        // Now we know, how long to run.
        if( thisToGo>firstToGo ) thisToGo = firstToGo;
        if( thisToGo>secondToGo ) thisToGo = secondToGo;
        
        if( !thisToGo ) goto endLoop;

        // int untilFadeOut;
        int fromFadeIn, fromFadeOut;
        sample_t *s; 
        sample_t *d; 
        // Which phase?
        
        fromFadeOut = sgFirst->overlapOut_ - firstToGo;
        fromFadeIn = firstDone - sgFirst->overlapIn_;
        s = firstBuffer + firstDone;
        d = currPtr;

        if( fromFadeIn<0 ) {
            // Fade in.
            int l = sgFirst->overlapIn_;
            fromFadeIn = -fromFadeIn;
            if( thisToGo>fromFadeIn ) thisToGo = fromFadeIn;
            fromFadeIn = l-fromFadeIn;
            // Main phase. Just add the data to the buffer.
            while( thisToGo-- ) {
                sample_t a = *s++;
                a = (a*fromFadeIn)/l;
                fromFadeIn++;
            }
        } else {
            if( fromFadeOut<0 ) {       
                // Main phase. Just add the data to the buffer.
                fromFadeOut = -fromFadeOut;
                if( thisToGo>fromFadeOut ) thisToGo = fromFadeOut;
                while( thisToGo-- ) {
                    *d++ += *s++;
                }
            } else {
                // Fade out.
                int l = sgFirst->overlapOut_;
                fromFadeOut = l-fromFadeOut;
                // Main phase. Just add the data to the buffer.
                while( thisToGo-- ) {
                    sample_t a = *s++;
                    a = (a*fromFadeOut)/l;
                    fromFadeOut--;
                }
            }
        }
      endLoop:
        toGo -= thisToGo;
        currPtr += thisToGo;
        firstToGo -= thisToGo;
        secondToGo -= thisToGo;
        firstDone += thisToGo;
        secondDone += thisToGo;
        currPos += thisToGo;
    } 
    pos_ = currPos;
    return length-toGo;
} 

void twGrainer::init()
{
    twComponent::init();
}

void twGrainer::setNChannels( idx_t nChannels )
{
    // FIXME: Realloc input/output channels, ensure that
    // the source component has enough channels.
    nChannels_ = nChannels;
}

idx_t twGrainer::getNInputs() const
{
    return nChannels_;
}

idx_t twGrainer::getNOutputs() const
{
    return nChannels_;
}

const char *twGrainer::getInputName( idx_t ) const
{
    return "Grainer channel input";
}

const char *twGrainer::getOutputName( idx_t ) const
{
    return "Grainer channel output";
}

length_t twGrainer::getDuration() const
{
    return myDuration_;
}

twGrainSpec *twGrainer::getGrainSpec() const
{
    return grainSpec_;
}

bool twGrainer::isLooped() const
{
    return isLooped_;
}

void twGrainer::setGrainSpec( twGrainSpec *grainSpec )
{
    grainSpec_ = grainSpec;
    myDuration_ = grainSpec->getTotalLength();
    emit grainSpecChanged( grainSpec );
}

void twGrainer::setLooped( bool f )
{
    isLooped_ = f;
    // FIXME: signal?
}

void twGrainer::setStretchFactor( double factor )
{
    // FIXME: Define.
    if( factor<0.00001 ) {
        factor = 0.00001;
    }
    stretchFactor_ = factor;
}

void twGrainer::setPitchOffset( double offset )
{
    pitchOffset_ = offset;
}

/**
 * Load all grain parts into memory, allowing fast playback.
 * At this point, this method invokes the child's doInitOperation
 * and after that, reads the data.
 */
int twGrainer::doInitOperation( int /*initId*/ )
{
    // FIXME: Write me.
    return 0;
}

void twGrainer::createOutputLatches()
{
}

void twGrainer::setBufferSize( length_t l )
{
    twComponent::setBufferSize( l );
}

void twGrainer::setSourceComponent( twComponent *comp )
{
    sourceComponent_ = comp;
}

twGrainer::~twGrainer()
{
}

twGrainer::twGrainer( tw303aEnvironment &env )
    : twComponent( env ),
      isLooped_( false ),
      grainSpec_( NULL ),
      myDuration_( 0 ),
      nChannels_( 2 ),
      pos_( 0 ),
      stretchFactor_( 1.5 ),
      pitchOffset_( 1.0 ),
      sourceComponent_( NULL )
{
}
