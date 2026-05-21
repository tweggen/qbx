
#include <stdlib.h>
#include <stddef.h>
#include <qfile.h>
#include <qstring.h>

#include "twwavinput.h"

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
    if( file_.handle() >= 0 ) {
        return nSamples_;
    }
    return -1;
}

length_t twWavInput::setCacheSize( length_t newMax )
{
    if( newMax<env.getBufferSize() ) newMax = env.getBufferSize();
    return maxCacheSize_;
}

length_t twWavInput::getCacheSize() const
{
    return maxCacheSize_;
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
    return (const char *)fileName_.data();
}

idx_t twWavInput::getNInputs() const
{
    return 0;
}

idx_t twWavInput::getNOutputs() const
{
    return 4;
}

/**
 * OK, this is a very dumb implementation.
 * And did I mention it is inefficient?
 */
length_t twWavInput::calcOutputTo( sample_t *pDest, length_t length, idx_t idx )
{
    // FIXME: Fill cache here! Reading the data every time is inefficient
    // (although linux does a good job caching).

//      qWarning( "twWavInput::calcOutputTo(): Called for offset = %d.\n", 
//                playOffset_ );
    int orgChannels = orgChannels_;
    int neededReadLength = orgChannels*2 /* for the bits */ * length;
    short *psrc, *readData = (short *) alloca( neededReadLength );   
    psrc = readData+idx;
    file_.seek( dataStart_ + playOffset_*orgChannels*2 );
    int didRead = file_.read( (char *)readData, neededReadLength );
    sample_t *pd2 = pDest;
    psrc = readData;
    if( didRead<0 ) didRead = 0;
    didRead /= orgChannels*2;
    int i;
    for( i=0; i<didRead; ++i ) {
	unsigned char *x = (unsigned char *)psrc;
	*pd2++ = (sample_t) ((short)(x[0] | (x[1]<<8))) / 32768.;
	//        *pd2++ = *psrc;
        psrc+=orgChannels;
    }
    // FIXME: memset!!!!
    for( ;i<length; ++i ) {
        *pd2++ = 0;
    }
    return length;
}

/**
 * This function is not very tolerant.
 * It loads the first 1024 bytes expects fmt chunk to be there.
 * data chunk must at least give its length within the bounds.
 */
int twWavInput::findWaveProperties()
{
#define SLEN 8192
    unsigned char s[SLEN];

#if 0
    struct STRU_format {
        unsigned short wFormatTag;         // Format category
        unsigned short wChannels;          // Number of channels
        unsigned dwSamplesPerSec;    // Sampling rate
        unsigned dwAvgBytesPerSec;   // For buffer estimation
        unsigned short wBlockAlign;        // Data block size
        // format special fields
        unsigned short wBitsPerSample;	// Sample size
    } *fmtHdr;
#else
    struct STRU_format {
	unsigned char wFormatTag[2];
	unsigned char wChannels[2];
	unsigned char dwSamplesPerSec[4];
	unsigned char dwAvgBytesPerSecond[4];
	unsigned char wBlockAlign[2];
	unsigned char wBitsPerSample[2];
    } *fmtHdr;

# define EX_SHORT(x) ((x)[0]|((x)[1]<<8))
# define EX_LONG(x) ((x)[0]|((x)[1]<<8)|((x)[2]<<16)|((x)[3]<<24))
#endif

    int chunkLength = 0;
    if( !file_.seek( 0 ) ) return -1;
    memset( s, 0, SLEN );
    file_.read( (char *)s, SLEN );
    if( ::strncmp( (char *)s, "RIFF", 4 ) ) return -2;
    if( ::strncmp( (char *)s+8, "WAVEfmt ", 8 ) ) return -3;
    chunkLength = EX_LONG(((unsigned char *)(s+16)));
    fmtHdr = (STRU_format *) ((int *)(s+20));
    orgChannels_ = EX_SHORT(fmtHdr->wChannels);
    if( orgChannels_ <= 0 ) return -5;
    orgRate_ = EX_LONG(fmtHdr->dwSamplesPerSec);
    orgBits_ = EX_SHORT(fmtHdr->wBitsPerSample);
    if( orgBits_ < 8 ) return -6;
    qWarning( "File \"%s\" has %d channels, %d Hz, %d bits per sample.\n",
              (const char *) fileName_.data(), orgChannels_, orgRate_, orgBits_ );
    s[SLEN-1] = 0;
    char *data = NULL;
    // We could flag here, wether the data chunk starts at a getpagesize()
    // boundary for platforms using mmap.
    for( int i=0; i<(SLEN-4); i++ ) {
        if( !strncmp( (const char *)(s+i), "data", 4 ) ) {
            data = (char *)s+i;
            break;
        }
    }
    if (!data ) return -7;
    nSamples_ = (EX_LONG(((unsigned char *)(data+4))) / orgChannels_) / (orgBits_/8);
    fprintf( stderr, "nSamples_ = %d:%d.\n", (int)(nSamples_>>32), (int)nSamples_ );
    dataStart_ = data-(char *)s + 8;
    return 0;
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
      orgChannels_( 0 ),
      outputChannels_( 0 ),
      cache_( 0 ),
      maxCacheSize_( 0 ),
      cacheSize_( 0 ),
      dataStart_( -1 ),
      nSamples_( 0 ),
      cacheStart_( (offset_t)-1 ),
      fileName_( fileName ),
      playOffset_( 0 )
{
    if( !fileName.isEmpty() ) {
        file_.setFileName( fileName_ );
    }
    if( !file_.open( QIODevice::ReadOnly ) ) {
        qWarning( "Error opening file \"%s\".\n", (const char*) fileName.data() );
        return;
    }
    // Probe wave file.
    int res = findWaveProperties();
    if( res < 0 ) {
        qWarning( "File is not a wave file: %d\n", res );
        file_.close();
        return;
    }
    maxCacheSize_ = env.getBufferSize();
    // For now, use a fix cache size.
    cacheSize_ = maxCacheSize_;
    cache_ = (sample_t *) ::calloc( sizeof( sample_t ), cacheSize_ );
    if( !cache_ ) {
        qWarning( "Unable to allocate %d bytes of cache.\n", (int)(sizeof( sample_t ) * cacheSize_) );
        file_.close();
        return;
    }    
}

twWavInput::~twWavInput()
{
    if( file_.handle()>=0 ) file_.close();
    if( cache_ ) {
        ::free( cache_ );
        cache_ = NULL;
    }
}
