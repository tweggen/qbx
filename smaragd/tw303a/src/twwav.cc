
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <memory.h>

#include <iostream>

#include "twsyslog.h"

#include "twconvert.h"
#include "twwav.h"

#define WAVE_FORMAT_PCM 1

twWav::twWav( tw303aEnvironment &env0, char const *fileName, length_t length )
	: twComponent( env0 ), totalLength( length )
{
	struct STRU_format {
		unsigned short wFormatTag;         // Format category
		unsigned short wChannels;          // Number of channels
		unsigned dwSamplesPerSec;    // Sampling rate
		unsigned dwAvgBytesPerSec;   // For buffer estimation
		unsigned short wBlockAlign;        // Data block size
// format special fields
	    unsigned short wBitsPerSample;	// Sample size
	} wav_format = {
		WAVE_FORMAT_PCM, 1,
		(unsigned) env.getSRate(),          // dwSamplesPerSec
		(unsigned) env.getSRate() * 1 * 2,  // dwAvgBytesPerSec = rate * ch * bytes
		2, 16
	};
	struct {     
		char  id[4];  	// identifier string = "RIFF"
		int   len;    	// remaining length after this header
	} riff_hdr;
	struct {     
		char  id[4];  	// identifier string = "RIFF"
		int   len;    	// remaining length after this header
	} chunk_hdr;
	char wave_id[4];

	FILE *fp_out = fp=fopen( fileName, "wb");
	if (!fp){
		throw excStandard( "Error opening output file." );
	}
	// RIFF header schreiben. memcpy (not strncpy) — WAV chunk IDs are
	// fixed-width 4-byte ASCII tags with no NUL terminator.
	memcpy( riff_hdr.id, "RIFF", 4 );
	// L�nge nach dem RIFF-hdr
	riff_hdr.len=sizeof(wave_id)+sizeof(chunk_hdr)+16+sizeof(chunk_hdr)+length*sizeof(short);
	if (fwrite( &riff_hdr, sizeof(riff_hdr), 1, fp_out ) != 1 ){
		throw excStandard( "Error writing RIFF header." );
	}
	// WAVE ID schreiben
	memcpy( wave_id, "WAVE", 4 );
	if (fwrite( wave_id, sizeof(wave_id), 1, fp_out ) != 1 ){
		throw excStandard( "Error writing WAVE header." );
	}

	// Chunk header fmt schreiben
	memcpy( chunk_hdr.id, "fmt ", 4 );
	chunk_hdr.len=16;
	if (fwrite( &chunk_hdr, sizeof(chunk_hdr), 1, fp_out ) != 1 ){
		throw excStandard( "Error writing fmt header." );
	}
	// Format schreien
	if (fwrite( &(wav_format), sizeof(wav_format), 1, fp_out ) != 1 ){
		throw excStandard( "Error writing format chunk." );
	}

	// chunk header data schreiben
	memcpy( chunk_hdr.id, "data", 4 );
	// chunk_hdr.len=stru_wav->data_len*2;
	// ???
	chunk_hdr.len=length * sizeof( short ) * getNInputs();
	if (fwrite( &chunk_hdr, sizeof(chunk_hdr), 1, fp_out ) != 1 ){
		throw excStandard( "Error writing data header." );
	} 
}

twWav::~twWav()
{
	if( fp ) {
		fclose( fp );
		fp = NULL;
	}
}

length_t twWav::calcOutputTo( sample_t *, length_t, idx_t )
{
    // nothing to render.
    return 0;
}



void twWav::createOutputLatches()
{
}

int twWav::writeLoop()
{
	signed short *outBuf = NULL;
	sample_t *sampleBuffer = NULL;
	length_t toWrite = totalLength;

	try {
		outBuf = (signed short *) malloc( 2 * env.getBufferSize() );
		if( !outBuf ) {
			throw excStandard( "twWav::writeLoop(): not enough memory" );
		}

		sampleBuffer = (sample_t *) malloc( env.getBufferSize() * sizeof( sample_t ) );
		if( !sampleBuffer ) {
			throw excStandard( "twWav::writeLoop(): not enough memory" );
		}
	} catch( excStandard &e ) {
		if( outBuf ) free( outBuf );
		if( sampleBuffer ) free( sampleBuffer );
		throw;
	}

	// Mono Float32 from the synth → mono Int16 on disk, via the shared
	// converter (replaces the former hand-rolled clip loop).
	twFormat srcFmt;
	srcFmt.sampleType = twSampleType::Float32;
	srcFmt.channels   = 1;
	twFormat dstFmt = srcFmt;
	dstFmt.sampleType = twSampleType::Int16;

	toWrite = totalLength;
	while( toWrite>0 ) {
		length_t realRead;
		length_t readNow = toWrite<env.getBufferSize()?toWrite:env.getBufferSize();

		realRead = ((twLatchStreamingOutput *)pInputPlugs[0]) -> readStreamingData(
			sampleBuffer,
			readNow
			);

		twConvertFrames( srcFmt, sampleBuffer, dstFmt, outBuf, realRead );

		fwrite( outBuf, realRead, sizeof( short ), fp );
		toWrite -= realRead;
	}

	free( outBuf );
	free( sampleBuffer );
	return 0;
}


