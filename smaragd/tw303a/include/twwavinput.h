
#ifndef _TWWAVINPUT_H
#define _TWWAVINPUT_H

#include <qfile.h>
#include <mutex>
#include "twcomponent.h"

class tw303aEnvironment;

/**
 * Audio input component that reads WAV file samples.
 *
 * Thread affinity: AUDIO THREAD ONLY
 * - calcOutputTo() is called from audio thread callback
 * - file_ (QFile) is accessed without synchronization
 * - seekTo() is called from audio thread
 *
 * CRITICAL: This class is NOT thread-safe. The QFile handle is accessed directly
 * from the audio render callback without any mutex or thread-local protection.
 * If UI thread accesses the same SPlainWave object via getPreview(), it will race
 * against calcOutputTo() accessing the same file_.
 */
class twWavInput
    : public twComponent
{
public:
    twWavInput( tw303aEnvironment &env, QString fileName );
    virtual ~twWavInput();

    virtual void createOutputLatches();

    virtual int setNOutputs( idx_t );
    virtual length_t getLength() const;
    virtual length_t setCacheSize( length_t );
    virtual length_t getCacheSize() const;

    virtual bool isSeekable() const;
    virtual int seekTo( offset_t );

    virtual const char *getInputName( idx_t ) const;
    virtual const char *getOutputName( idx_t ) const;
    virtual idx_t getNInputs() const;
    virtual idx_t getNOutputs() const;
    virtual void setBufferSize( length_t );
    virtual void init();
    
    virtual length_t calcOutputTo( sample_t *pDest, length_t length, idx_t idx );

    bool wasLoaded() const { return dataStart_ != -1; }

protected:

private:
    int findWaveProperties();

    idx_t orgChannels_;
    int orgRate_;
    int orgBits_;
    idx_t outputChannels_;

    sample_t *cache_;
    int maxCacheSize_;
    int cacheSize_;
    // Where does the data chunk start inside the file?
    long dataStart_;
    length_t nSamples_;
    offset_t cacheStart_;
    QString fileName_;
    QFile file_;
    mutable std::mutex fileMutex_;  // Protects file_ access from concurrent threads (UI redraw + audio)

    offset_t playOffset_;
};

#endif
