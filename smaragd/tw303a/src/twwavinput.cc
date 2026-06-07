
#include <stdlib.h>
#include <string.h>

#include <qstring.h>

#include "twwavinput.h"
#include "twsamplesource.h"

void twWavInput::createOutputLatches()
{
    pOutputLatches[0] = new twStreamingLatch( *this, 0, 0 );
}

int twWavInput::setNOutputs( idx_t )
{
    return -1;
}

length_t twWavInput::getLength() const
{
    return source_ ? source_->length() : -1;
}

// Cache sizing is obsolete now that the whole file is resident; kept as no-op
// stubs so the public API is unchanged.
length_t twWavInput::setCacheSize( length_t )
{
    return 0;
}

length_t twWavInput::getCacheSize() const
{
    return 0;
}

bool twWavInput::isSeekable() const
{
    return true;
}

int twWavInput::seekTo( offset_t newOffset )
{
    playOffset_ = newOffset;
    return 0;
}

const char *twWavInput::getInputName( idx_t ) const
{
    return NULL;
}

const char *twWavInput::getOutputName( idx_t ) const
{
    return (const char *) fileName_.data();
}

idx_t twWavInput::getNInputs() const
{
    return 0;
}

idx_t twWavInput::getNOutputs() const
{
    return 4;
}

twRandomSource *twWavInput::getSource() const
{
    return source_;
}

/**
 * Serve audio by random-reading the resident source at the current play
 * position. This is the shared/back-compat cursor; it does not auto-advance,
 * matching the historical contract where callers seek before every block.
 */
length_t twWavInput::calcOutputTo( sample_t *pDest, length_t length, idx_t idx )
{
    if( !source_ ) {
        memset( pDest, 0, sizeof( sample_t ) * length );
        return length;
    }
    source_->read( playOffset_, pDest, length, idx );
    return length;
}

void twWavInput::init()
{
    twComponent::init();
}

void twWavInput::setBufferSize( length_t )
{
}

twWavInput::twWavInput( tw303aEnvironment &env, QString fileName )
    : twComponent( env ),
      source_( NULL ),
      loaded_( false ),
      playOffset_( 0 ),
      fileName_( fileName )
{
    if( fileName.isEmpty() ) {
        return;
    }
    source_ = new twSampleSource( env, fileName_ );
    if( !source_->wasLoaded() ) {
        qWarning( "twWavInput: failed to load \"%s\".\n",
                  (const char *) fileName_.toUtf8().constData() );
        delete source_;
        source_ = NULL;
        return;
    }
    loaded_ = true;
}

twWavInput::~twWavInput()
{
    if( source_ ) {
        delete source_;
        source_ = NULL;
    }
}
