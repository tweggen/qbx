
#ifdef _TW303A_STANDALONE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stddef.h>
#include <iostream>

#include "twsyslog.h"

#include "tw303aenv.h"

#include "twcomponent.h"
#include "twsimplesaw.h"
#include "twsaw.h"
#include "twspeaker.h"
#include "twconstant.h"
#include "twmixer.h"
#include "twmoog.h"
#include "twtestseq.h"
#include "twwav.h"
#include "twwhitenoise.h"
#include "twpipe.h"

// channels frequency length

class TestString
    : public QObject 
{
private:
    char *s;
public:
    TestString (char *s0):s (s0) {}
    TestString ():s ("uninitialized") {}
    
    void print () {
        cout << s << endl;
    }
};

void testRun( length_t bufSize )
{
    tw303aEnvironment *pEnv = new tw303aEnvironment();
    pEnv->setBufferSize( bufSize );
    
#undef TESTSEQ
#ifdef TESTSEQ
    twComponent *pSoundFreq = (twComponent *) new twTestSeq( *pEnv, 100, 0 );
    twComponent *pSoundFreqDirty = (twComponent *) new twTestSeq( *pEnv, 100, 4000 );
    twComponent *pOscDirty = (twComponent *) new twSimpleSaw( *pEnv );
    twComponent *pOscClean = (twComponent *) new twSaw(*pEnv );
    twComponent *pLFOFreq = (twComponent *) new twConstant( *pEnv, (25*120)/120 );
    twComponent *pLFO = (twComponent *) new twSaw( *pEnv, 10, 11 );
    twComponent *pEnvFreq = (twComponent *) new twConstant( *pEnv, (800*120)/120 );
    twComponent *pEnvel = (twComponent *) new twSaw( *pEnv, 40000, 100 );
    twComponent *pMixer = (twComponent *) new twMixer( *pEnv, 2 );
    twComponent *pSpeaker = (twComponent *) new twSpeaker( *pEnv );
    // test comment
    twComponent *pMoog = (twComponent *) new twMoog( *pEnv, 1.0 );
    twComponent *pMoogEnv = (twComponent *) new twMoog( *pEnv, 1.0 );
    twComponent *pWave = (twComponent *) new twWav( *pEnv, "out.wav", (pEnv->getSRate()*32)*120/120 );
    twComponent *pFreqMix = (twComponent *) new twMixer( *pEnv, 2 );
    twComponent *pReverb = (twComponent *) new twPipe( *pEnv );
    
    // initialize everything
    pSoundFreq->init();
    pSoundFreqDirty->init();
    pOscDirty->init();
    pOscClean->init();
    pMixer->init();
    pMoog->init();
    pMoogEnv->init();
    pSpeaker->init();
    pFreqMix->init();
    pReverb->init();
    
    pLFOFreq->init();
    pLFO->init();
    
    pEnvFreq->init();
    pEnvel->init();
    
    pWave->init();
    
    // wire the filter LFO to ots frequency constant
    pLFO->setInput( 0, pLFOFreq->linkOutput( 0 ) );
    pEnvel->setInput( 0, pEnvFreq->linkOutput( 0 ) );
    // wire the constant output to the oscillator
    pOscDirty->setInput( 0, pSoundFreqDirty->linkOutput( 0 ) );
//	pOscClean->setInput( 0, pSoundFreqDirty->linkOutput( 0 ) );
    pOscClean->setInput( 0, pSoundFreq->linkOutput( 0 ) );
    
    //pMixer->setInput( 0, pOscDirty->linkOutput( 0 ) );
    //pMixer->setInput( 1, pOscClean->linkOutput( 0 ) );
    
    pFreqMix->setInput( 0, pEnvel->linkOutput( 0 ) );
    pFreqMix->setInput( 1, pLFO->linkOutput( 0 ) );
    
    //pMoogEnv->setInput( 0, pMixer->linkOutput( 0 ) );
    pMoogEnv->setInput( 0, pOscClean->linkOutput( 0 ) );
    pMoogEnv->setInput( 1, pFreqMix->linkOutput( 0 ) );
    
    //pReverb->setInput( 0, pMoogEnv->linkOutput( 0 ) );
    
    // wire the mixer output to the speaker.
    
    //pWave->setInput( 0, pReverb->linkOutput( 0 ) );
    
    //pSpeaker->setInput( 0, pReverb->linkOutput( 0 ) );
    //
    pSpeaker->setInput( 0, pMoogEnv->linkOutput( 0 ) );
    
    ((twSpeaker *)pSpeaker)->speakerLoop();
    //	((twWav *)pWave)->writeLoop();
    
//	delete ((twWav*) pWave);
#else
    twComponent *pEnvelFreq = (twComponent *) new twConstant( *pEnv, 400 );
    twComponent *pOscFreq = (twComponent *) new twConstant( *pEnv, 100*200 );
    twComponent *pWhiteNoise = (twComponent *) new twWhiteNoise( *pEnv );
    twComponent *pEnvel = (twComponent *) new twSaw( *pEnv, 5000, 10 );
    //twComponent *pEnvel = (twComponent *) new twConstant( *pEnv, 100 );
    twComponent *pMoog = (twComponent *) new twMoog( *pEnv, 3.6 );
    twComponent *pWave = (twComponent *) new twWav( *pEnv, "noisesweep.wav", pEnv->getSRate()*4 );
    
    pEnvelFreq->init();
    pOscFreq->init();
    pEnvel->init();
    pWhiteNoise->init();
    pMoog->init();
    pWave->init();

    pEnvel->setInput( 0, pEnvelFreq->linkOutput( 0 ) );
    pWhiteNoise->setInput( 0, pOscFreq->linkOutput( 0 ) );
    pMoog->setInput( 0, pWhiteNoise->linkOutput( 0 ) );
    pMoog->setInput( 1, pEnvel->linkOutput( 0 ) );
    pWave->setInput( 0, pMoog->linkOutput( 0 ) );
    ((twWav *)pWave)->writeLoop();
#endif
}

void main (int argc, char **argv)
{
    cout<< "Hallo" << endl;
    try {
        int bufSize = 4096;
        if( argc==2 ) bufSize = atoi( argv[1] );
        testRun( bufSize );
    } catch ( excStandard a ) {
        syslog( LOG_WARNING, "Uncaught exception: %s", a.getMsg() );
        cout << "Uncaught exception: " << a.getMsg() << endl;
    }
}

#endif
