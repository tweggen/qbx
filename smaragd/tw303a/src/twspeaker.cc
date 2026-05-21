
//#define QBX_MAC_OSX_10_2 1
//#define QBX_LINUX_OSS

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <memory.h>
#include <sys/ioctl.h>
#include <sys/errno.h>
#include <iostream>

#include <syslog.h>

#include <qsocketnotifier.h>

#ifdef QBX_LINUX_OSS
# include <linux/soundcard.h>
# warning Using Linux Sound.
#endif

#ifdef QBX_MAC_OSX_10_2
#include <CoreServices/CoreServices.h>
#include <CoreAudio/CoreAudio.h>
#include <AudioUnit/AudioUnit.h>
#include <AudioToolbox/DefaultAudioOutput.h>
#include <AudioToolbox/AudioConverter.h>
#endif

#include "sapplication.h"
#include "twspeaker.h"

#ifdef QBX_MAC_OSX_10_2
OSStatus	
twSpeaker::MyRenderer(
	   void 				*inRefCon, 
	   AudioUnitRenderActionFlags 	*ioActionFlags, 
	   const AudioTimeStamp 		*inTimeStamp, 
	   UInt32 						inBusNumber, 
	   UInt32 						inNumberFrames, 
	   AudioBufferList 			*ioData)

{
    twSpeaker *me = (twSpeaker *) inRefCon;
    length_t maxSamples = me->env.getBufferSize();
    //    sample_t *sampleBuffer = (sample_t *)alloca( maxSamples );
    int toRead = inNumberFrames;
    // Look down the over the number of frames to read.
    // FIXME: Currently, we directly render into the output buffer,
    // as we know, that sample_t matches the chosen output data format.
    // Hope, this won't change!
    sample_t *dest = reinterpret_cast<sample_t *>( ioData->mBuffers[0].mData );
    //    int bytes = ioData->mBuffers[channel].mDataByteSize;
    // fprintf( stderr, "toRead %d bytes.\n", (int)toRead );
    while( toRead>0 ) {
        length_t readNow = toRead;
        if( readNow > maxSamples ) readNow = maxSamples;

        length_t realRead = ((twLatchStreamingOutput *)me->pInputPlugs[0])
            ->readStreamingData(
				dest,
				readNow
				);
        //	fprintf( stderr, "Read %d bytes.\n", (int)realRead );
        // Now we have calculated the samples. Assuming, everything is
        // 32bit signed integer, now prepare the output.
        
        // FIXME: We assume to always read something.
        if( realRead <= 0 ) break;		
        dest += realRead;
        toRead -= realRead;
    }
    //fprintf( stderr, "After loop \n" );
    toRead = inNumberFrames;
    // Clip here.
#if 0 // If we desire some clipping.
    dest = ioData->mBuffers[0].mData;
    while( toRead-- ) {
        sample_t a = *dest;
        if( a > 32767 ) a = 32767;
        if( a < -32768 ) a = -32768;
        *dest++ = a << 16;
    }
#endif
    // Create mono output.
    for (UInt32 channel = 1; channel < ioData->mNumberBuffers; channel++)
        memcpy (ioData->mBuffers[channel].mData, ioData->mBuffers[0].mData, ioData->mBuffers[0].mDataByteSize);
    //    fprintf( stderr, "MyRenderer: Called.\n" );
    return noErr;
}

static AudioUnit theOutputUnit;

// ________________________________________________________________________________
//
// TestDefaultAU
//
void twSpeaker::TestDefaultAU( void *userData )
{
	OSStatus err = 0;

	// Open the default output unit
	ComponentDescription desc;
	desc.componentType = kAudioUnitType_Output;
	desc.componentSubType = kAudioUnitSubType_DefaultOutput;
	desc.componentManufacturer = kAudioUnitManufacturer_Apple;
	desc.componentFlags = 0;
	desc.componentFlagsMask = 0;
	
	Component comp = FindNextComponent(NULL, &desc);
	if (comp == NULL) { printf ("FindNextComponent\n"); return; }
	
	err = OpenAComponent(comp, &theOutputUnit);
	if (comp == NULL) { printf ("OpenAComponent=%ld\n", err); return; }

	// Initialize it
	err = AudioUnitInitialize(theOutputUnit);
	if (err) { printf ("AudioUnitInitialize=%ld\n", err); return; }
	
	// Set up a callback function to generate output to the output unit
	AURenderCallbackStruct input;
	input.inputProc = MyRenderer;
	input.inputProcRefCon = userData;

	err = AudioUnitSetProperty (theOutputUnit, 
				    kAudioUnitProperty_SetRenderCallback, 
				    kAudioUnitScope_Input,
				    0, 
				    &input, 
				    sizeof(input));
	if (err) { printf ("AudioUnitSetProperty-CB=%ld\n", err); return; }
	
	
	// We tell the Output Unit what format we're going to supply data to it
	// this is necessary if you're providing data through an input callback
	// AND you want the DefaultOutputUnit to do any format conversions
	// necessary from your format to the device's format.
	AudioStreamBasicDescription streamFormat;
#if 1
	streamFormat.mSampleRate = 44100;		//	the sample rate of the audio stream
	streamFormat.mFormatID = kAudioFormatLinearPCM;			//	the specific encoding type of audio stream
	streamFormat.mFormatFlags = 
	    kLinearPCMFormatFlagIsFloat 
	    | kLinearPCMFormatFlagIsBigEndian
	    | kLinearPCMFormatFlagIsPacked
	    | kAudioFormatFlagIsNonInterleaved;		//	flags specific to each format
	streamFormat.mBytesPerPacket = 4; // Is sizeof( float ) better? What about PPC64?	
	streamFormat.mFramesPerPacket = 1;	
	streamFormat.mBytesPerFrame = 4;		
	streamFormat.mChannelsPerFrame = 1;	
	streamFormat.mBitsPerChannel = 32;	
#else
	streamFormat.mSampleRate = sSampleRate;		//	the sample rate of the audio stream
	streamFormat.mFormatID = theFormatID;			//	the specific encoding type of audio stream
	streamFormat.mFormatFlags = theFormatFlags;		//	flags specific to each format
	streamFormat.mBytesPerPacket = theBytesInAPacket;	
	streamFormat.mFramesPerPacket = theFramesPerPacket;	
	streamFormat.mBytesPerFrame = theBytesPerFrame;		
	streamFormat.mChannelsPerFrame = sNumChannels;	
	streamFormat.mBitsPerChannel = theBitsPerChannel;	
#endif	
	printf("Rendering source:\n\t");
	printf ("SampleRate=%f,", streamFormat.mSampleRate);
	printf ("BytesPerPacket=%ld,", streamFormat.mBytesPerPacket);
	printf ("FramesPerPacket=%ld,", streamFormat.mFramesPerPacket);
	printf ("BytesPerFrame=%ld,", streamFormat.mBytesPerFrame);
	printf ("BitsPerChannel=%ld,", streamFormat.mBitsPerChannel);
	printf ("ChannelsPerFrame=%ld\n", streamFormat.mChannelsPerFrame);
	
	err = AudioUnitSetProperty (theOutputUnit,
				    kAudioUnitProperty_StreamFormat,
				    kAudioUnitScope_Input,
				    0,
				    &streamFormat,
				    sizeof(AudioStreamBasicDescription));
	if (err) { printf ("AudioUnitSetProperty-SF=%4.4s, %ld\n", (char*)&err, err); return; }
	
	Float64 outSampleRate;
	UInt32 size = sizeof(Float64);
	err = AudioUnitGetProperty (theOutputUnit,
				    kAudioUnitProperty_SampleRate,
				    kAudioUnitScope_Output,
				    0,
				    &outSampleRate,
				    &size);
	if (err) { printf ("AudioUnitSetProperty-GF=%4.4s, %ld\n", (char*)&err, err); return; }
	//	expNumFrames = UInt32(sSampleRate / outSampleRate * 512.0);

}

#endif

/**
 * Open the audio output device.
 * 
 * FIXME: We still should read the audio output settings from the application.
 */
int twSpeaker::openDevice()
{
    fprintf( stderr, "twSpeaker::openDevice(): Called.\n" );
#ifdef QBX_LINUX_OSS
    fd = open( "/dev/dsp", O_WRONLY );
    if( fd<0 ) {
        throw excStandard( "twSpeaker::initDevice():open(): "
                           "Unable to open Audio Device." );
    }   
    try {
        int
            /* sndRate,
            sndChannels,
            sndBits,*/
            sndFragment;
//            sndBlkSize;        
        if (ioctl(fd, SNDCTL_DSP_RESET))
            throw excStandard( "twSpeaker::initDevice():AudioInitDevice(): "
                               "ioctl SNDCTL_DSP_RESET: error." );        
        realChannels_ = 1;
        if (ioctl(fd, SNDCTL_DSP_CHANNELS, &realChannels_))
            throw excStandard( "twSpeaker::initDevice():AudioInitDevice(): "
                               "ioctl SNDCTL_DSP_CHANNELS error." );        
        realRate_ = 44100;
        if (ioctl(fd, SNDCTL_DSP_SPEED, &realRate_)) 
            throw excStandard( "twSpeaker::initDevice():AudioInitDevice(): "
                               "ioctl SNDCTL_DSP_SPEED: error." );        
        realBits_ = 16;
        if (ioctl(fd, SNDCTL_DSP_SETFMT, &realBits_)) 
            throw excStandard( "twSpeaker::initDevice():AudioInitDevice(): "
                               "ioctl SNDCTL_DSP_CHANNELS error." );        
        sndFragment = 0x30000 | 11; // Maximum of four fragments of 1024 bytes.
        if(ioctl(fd, SNDCTL_DSP_SETFRAGMENT, &sndFragment))
            throw excStandard( "twSpeaker::initDevice():AudioInitDevice(): "
                               "ioctl SNDCTL_DSP_FRAGMENT error." );        
        fprintf( stderr, "Set audio output to a maximum of %d fragments of %d bytes each.\n",
                 sndFragment>>16, sndFragment&0xffff );
    } catch( excStandard e ) {
        close( fd );
        fd = -1;
        throw e;
    }
    // Start writing.
#endif
#if defined( QBX_LINUX_ALSA )
    int rc;
    unsigned int val;
    int dir;

    rc = snd_pcm_open(&alsaHandle_, "default",
                    SND_PCM_STREAM_PLAYBACK, 0);
    if (rc < 0) {
        throw excStandard(
            "twSpeaker::initDevice():open(): "
            "Unable to open Audio Device." );
    }

    snd_pcm_hw_params_t *params;

    /* Allocate a hardware parameters object. */
    snd_pcm_hw_params_alloca(&params);

    /* Fill it in with default values. */
    snd_pcm_hw_params_any(alsaHandle_, params);

    /* Set the desired hardware parameters. */

    /* Interleaved mode */
    snd_pcm_hw_params_set_access(
        alsaHandle_, params,
        SND_PCM_ACCESS_RW_INTERLEAVED);

    /* Signed 16-bit little-endian format */
    snd_pcm_hw_params_set_format(
        alsaHandle_, params,
        SND_PCM_FORMAT_S16_LE);

    realChannels_ = 2;
    /* Two channels (stereo) */
    snd_pcm_hw_params_set_channels(
        alsaHandle_,
        params,
        2
        );

    /* 44100 bits/second sampling rate (CD quality) */
    val = 44100;
    snd_pcm_hw_params_set_rate_near(
        alsaHandle_,
        params, &val, &dir);

    alsaBufferSize_ = 1024;
    alsaPeriodSize_ = 64;

    snd_pcm_hw_params_set_buffer_size_near( alsaHandle_, params, &alsaBufferSize_ );
    snd_pcm_hw_params_set_period_size_near( alsaHandle_, params, &alsaPeriodSize_, NULL );

    /* Write the parameters to the driver */
    rc = snd_pcm_hw_params( alsaHandle_, params );
    if (rc < 0) {
        throw excStandard(
            "unable to set hw parameters");
    }

#endif
    return 0;
}


/**
 * Shut down the audio output device.
 */
int twSpeaker::closeDevice()
{
#ifdef QBX_LINUX_OSS
    if( fd<0 ) {
        printf( "twSpeaker::closeDevice(): Warning: Device was closed.\n" );
        return 0;
    }
    close( fd );
    fd = -1;
#endif
#ifdef QBX_LINUX_ALSA
    if( alsaHandle_ ) {
        snd_pcm_drain( alsaHandle_ );
        snd_pcm_close( alsaHandle_ );
        alsaHandle_ = NULL;
    }
#endif
    return 0;
}

void twSpeaker::startOutput()
{
    if( isPlaying_ ) return;    
    qWarning( "twSpeaker::startOutput(): Called.\n" );

    printf( "opening device.\n" );
    // Open the output device.
    openDevice();


    lastPosSet_ = 0;
    bufferSize_ = sizeof( signed short ) * env.getBufferSize() * realChannels_;
    bufferValid_ = 0;

    /*
     * For all platforms: Allocate the ourput buffer.
     */
    try {
        outBuffer_ = (signed short *) malloc( bufferSize_ );
        if( !outBuffer_ ) {
            throw excStandard( "twSpeaker::startOutput(): not enough memory" );
        }
        
        sampleBuffer_ = (sample_t *) malloc( env.getBufferSize() * sizeof( sample_t ) );
        if( !sampleBuffer_ ) {
            throw excStandard( "twSpeaker::speakerOutput(): not enough memory" );
        }
    } catch( excStandard e ) {
        if( outBuffer_ ) free( outBuffer_ );
        if( sampleBuffer_ ) free( sampleBuffer_ );
        throw e;
    }

#ifdef QBX_MAC_OSX_10_2 
    int err;
	// Start the rendering
	// The DefaultOutputUnit will do any format conversions to the format of the default device
	err = AudioOutputUnitStart(theOutputUnit);
	if (err) { printf ("AudioOutputUnitStart=%d\n", err); return; }
#endif

#ifdef QBX_LINUX_OSS
     // Now that the fd is open, setup the qsocketnotifier.
    socketNotifier_ = new QSocketNotifier( fd, QSocketNotifier::Write, this );
    QObject::connect( socketNotifier_, SIGNAL( activated( int ) ),
                      this, SLOT( audioOutputReady() ) );

    socketNotifier_->setEnabled( true );

    printf( "twSpeaker::startOutput(): Created socket notifier.\n" );
    fflush( stdout );

    audioOutputReady();
#endif

#ifdef QBX_LINUX_ALSA
    snd_pcm_prepare( alsaHandle_ );
    alsaWriteChunk_( 2*alsaPeriodSize_ );

    snd_async_add_pcm_handler(&alsaPcmCallbackHandle_, alsaHandle_, &twSpeaker::alsaPcmHandlerStatic_, this);
    snd_pcm_start( alsaHandle_ );
#endif
    isPlaying_ = true;
}

void twSpeaker::stopOutput()
{
   printf( "stopOutput().\n" );
    if( !isPlaying_ ) return;
#ifdef QBX_MAC_OSX_10_2
    verify_noerr(AudioOutputUnitStop(theOutputUnit));
#endif
#ifdef QBX_LINUX_OSS
    socketNotifier_->setEnabled( false );
    close( fd );
    delete socketNotifier_;
    ::free( outBuffer_ );
    ::free( sampleBuffer_ );
#endif
#ifdef QBX_LINUX_ALSA
    snd_pcm_drop( alsaHandle_ );
    snd_async_del_handler( alsaPcmCallbackHandle_ );
#endif
    isPlaying_ = false;
}

#ifdef QBX_LINUX_ALSA
void twSpeaker::alsaPcmHandler()
{
    snd_pcm_sframes_t avail;

    avail = snd_pcm_avail_update( alsaHandle_ );
    alsaWriteChunk_( avail );
}

void twSpeaker::alsaPcmHandlerStatic_( snd_async_handler_t* pcmCallback )
{
    void* privateData = snd_async_handler_get_callback_private( pcmCallback );
    twSpeaker* me = static_cast<twSpeaker*>(privateData);
    me->alsaPcmHandler();
}
#endif

bool twSpeaker::isPlaying()
{
    return isPlaying_;
}

int twSpeaker::__init()
{
    isPlaying_ = false;
    return 0;
}

twSpeaker::twSpeaker( tw303aEnvironment &env0 )
    : twComponent( env0 )
{
    __init();
#ifdef QBX_MAC_OSX_10_2
    // Setup OS X audio and pass this class as user data.
    TestDefaultAU( (void*) this );
#endif
#ifdef QBX_LINUX_ALSA
    alsaHandle_ = NULL;
#endif
}

twSpeaker::~twSpeaker()
{
#ifdef QBX_LINUX_OSS
	if( fd ) close( fd );
#endif
#ifdef QBX_MAC_OSX_10_2
    CloseComponent(theOutputUnit);
#endif
}

length_t twSpeaker::calcOutputTo( sample_t *, length_t, idx_t )
{
	return 0;
}

void twSpeaker::createOutputLatches()
{
#ifdef DEBUG_COMPONENT
	syslog( LOG_DEBUG, "twSpeaker::createOutputLatches(): entered." );
#endif
}

#ifdef QBX_LINUX_ALSA
/**
 * If our output buffer is empty, fill it again.
 */
void twSpeaker::fillBuffer()
{
    length_t realRead;
    sample_t *pSrc = sampleBuffer_;
    short *pDest = outBuffer_;    
    /*
     * Compute how much should be read.
     */
    length_t toRead = 1024;

    offset_t formerPos = SApplication::app().getGlobalLocatorPos();
    // FIXME: This does not necessarily fit.
    realRead = ((twLatchStreamingOutput *)pInputPlugs[0]) -> readStreamingData(
        sampleBuffer_, toRead/sizeof( sample_t )
        );
    // sample_t maxVal = 0;

    if( realChannels_ <= 1 ) {
        for( int i=0; i<realRead; i++ ) {
            sample_t a = *pSrc++;
            a *= 32767.;
            if( a<-32768 ) a=-32768;
            else if( a>32767 ) a = 32767;
            *pDest++ = (short) a;
        }
    } else if( realChannels_ == 2 ) {
        for( int i=0; i<realRead; i++ ) {
            sample_t a = *pSrc++;
            short shorta;
            a *= 32767;
            if( a<-32768 ) a=-32768;
            else if( a>32767 ) a = 32767;
            shorta = (short) a;
            *pDest++ = shorta;
            *pDest++ = shorta;
        }
    }

    if( realRead>0 ) {
        SApplication::app().setGlobalLocatorPos( formerPos+realRead );
        bufferValid_ = realRead * realChannels_ * sizeof( signed short );
        lastPosSet_ = 0;
//        SApplication::app().setSpeakerMaxVal( maxVal );
    }
    
    
}

void twSpeaker::alsaWriteChunk_( snd_pcm_uframes_t chunkSize )
{
    int res;
    bool reenable = true;

    int inBuffer = bufferValid_ - lastPosSet_;
    if (0 == inBuffer) {
        fillBuffer();
        inBuffer = bufferValid_;
    }

    int toWrite = inBuffer;
    if (toWrite > (int)chunkSize) {
        toWrite = chunkSize;
    }

    res = snd_pcm_writei( alsaHandle_, outBuffer_, toWrite );

    if( res<0 ) {
        printf( "twSpeaker::alsaWriteChunk_(): Write error %d.\n", res );            
    } else if( res>=0 ) {
        if (res>0) {
            lastPosSet_ += res;
        } else {
            fprintf( stderr, "twSpeaker::alsaWriteChunk_(): 0 bytes written." );
        }
    }
    if( reenable ) {
    } else {
    }
}
#endif

#ifdef QBX_LINUX_OSS
void twSpeaker::audioOutputReady()
{
    length_t realRead;
    sample_t *pSrc = sampleBuffer_;
    short *pDest = outBuffer_;    
    int res;
    bool reenable = true;

//    printf( "twSpeaker::audioOutputReady(): Called;\n" );
//    fflush( stdout );

    offset_t formerPos = SApplication::app().getGlobalLocatorPos();
    // FIXME: This does not necessarily fit.
    realRead = ((twLatchStreamingOutput *)pInputPlugs[0]) -> readStreamingData(
        sampleBuffer_, 1024/sizeof( sample_t )
        //env.getBufferSize() 
        );
    // sample_t maxVal = 0;

    if( realChannels_ <= 1 ) {
        for( int i=0; i<realRead; i++ ) {
            sample_t a = *pSrc++;
	        a *= 32767.;
            if( a<-32768 ) a=-32768;
            else if( a>32767 ) a = 32767;
            *pDest++ = (short) a;
        }
    } else if( realChannels_ == 2 ) {
        for( int i=0; i<realRead; i++ ) {
            sample_t a = *pSrc++;
            short shorta;
	        a *= 32767;
            if( a<-32768 ) a=-32768;
            else if( a>32767 ) a = 32767;
            shorta = (short) a;
            *pDest++ = shorta;
            *pDest++ = shorta;
        }
    }

    if( realRead>0 ) {
        SApplication::app().setGlobalLocatorPos( formerPos+realRead );
//        SApplication::app().setSpeakerMaxVal( maxVal );
    }
    
    
    res = write( fd, outBuffer_, realRead*2*realChannels_ );
//    printf( "Write took %d bytes.\n", res );
    if( res<0 ) {
        if( errno==EAGAIN ) {        
            // No need to enable/disable anything.
            reenable = false;
        } else {
                      printf( "twSpeaker::audioOutputReady(): Write error.\n" );            
        }
    } else if( res>0 ) {        
    }
    if( reenable ) {
        socketNotifier_->setEnabled( false );
        socketNotifier_->setEnabled( true );
    }
}
#endif

#ifdef QBX_LINUX_OSS
int twSpeaker::speakerLoop()
{
    signed short *outBuf = NULL;
    sample_t *sampleBuffer = NULL;
    
    while( 1 ) {
        length_t realRead;
        sample_t *pSrc = sampleBuffer;
        short *pDest = outBuf;
        

        realRead = ((twLatchStreamingOutput *)pInputPlugs[0]) -> readStreamingData(
            sampleBuffer,
            env.getBufferSize()
            );
        
        for( int i=0; i<realRead; i++ ) {
            sample_t a = *pSrc++;
	    a *= 32767;
            if( a<-32768 ) a=-32768;
            else if( a>32767 ) a = 32767;
            
            *pDest++ = (short)a;
        }
        
        printf( "Before dsp write.\n" );
        write( fd, outBuf, realRead*2 );
        printf( "After dsp write.\n" );
    }
    
    free( outBuf );
    free( sampleBuffer );
    return 0;
}
#endif
