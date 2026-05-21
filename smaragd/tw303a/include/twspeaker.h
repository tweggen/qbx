
#ifndef _TWSPEAKER_H_
#define _TWSPEAKER_H_

#include "twcomponent.h"

#ifdef QBX_LINUX_ALSA
# define ALSA_PCM_NEW_HW_PARAMS_API
# include <alsa/asoundlib.h>
# warning Using ALSA sound
#endif

class QSocketNotifier;

class twSpeaker
    : public twComponent
{
    Q_OBJECT
private:
    int fd;
    int openDevice();
    int closeDevice();
    int __init();
    bool isPlaying_;

    QSocketNotifier *socketNotifier_;
    signed short *outBuffer_;
    sample_t *sampleBuffer_;

#ifdef QBX_MAC_OSX_10_2
    void TestDefaultAU( void *UserData );
static OSStatus	
MyRenderer(
	   void 				*inRefCon, 
	   AudioUnitRenderActionFlags 	*ioActionFlags, 
	   const AudioTimeStamp 		*inTimeStamp, 
	   UInt32 						inBusNumber, 
	   UInt32 						inNumberFrames, 
	   AudioBufferList 			*ioData);
 
#endif
    
protected:    
    virtual length_t calcOutputTo( sample_t *pDest, length_t length, idx_t idx );
    
public:
    ~twSpeaker();
    twSpeaker( tw303aEnvironment & );
    
    virtual void createOutputLatches( void );
    
    virtual const char *getInputName ( idx_t  ) const { return 0; }
    virtual const char *getOutputName ( idx_t  ) const { return 0; }
    virtual idx_t getNInputs() const { return 2; }
    virtual idx_t getNOutputs() const { return 0; }
    
    int speakerLoop();
    void setBufferSize( length_t  ) {};
    
    bool isPlaying();
public slots:
    void startOutput();
    void stopOutput();

#if defined( QBX_LINUX_OSS )
private slots:
    void audioOutputReady();
#endif

private:
    // The last position we wrote out.
    int lastPosSet_;
    int bufferSize_;
    int bufferValid_;

    int realChannels_;
    int realRate_;
    int realBits_;

#if defined( QBX_LINUX_ALSA )
    snd_pcm_t* alsaHandle_;
    snd_pcm_uframes_t alsaBufferSize_;
    snd_pcm_uframes_t alsaPeriodSize_;
    snd_async_handler_t* alsaPcmCallbackHandle_;
    void fillBuffer();
    void alsaWriteChunk_( snd_pcm_uframes_t chunkSize );
    static void alsaPcmHandlerStatic_( snd_async_handler_t* pcmCallback );
    void alsaPcmHandler();
#endif
};

#endif
