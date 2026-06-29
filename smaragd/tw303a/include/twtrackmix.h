
#ifndef _TW_TRACKMIX_H
#define _TW_TRACKMIX_H

#include <qobject.h>
#include <atomic>

#include "twcomponent.h"

class tw303aEnvironment;
class STrack;

class twTrackMix
    : public twComponent 
{
    Q_OBJECT
public:
    twTrackMix( tw303aEnvironment &env, STrack &perTrack );
    ~twTrackMix();

public slots:
    virtual void setBufferSize( length_t );

public:
    virtual bool isSeekable() const;
    virtual int seekTo( offset_t );
    virtual void createOutputLatches();

    virtual idx_t getNInputs() const;
    virtual idx_t getNOutputs() const;
    virtual const char *getInputName( idx_t ) const;
    virtual const char *getOutputName( idx_t ) const;
    
    STrack &getTrack() const;

    virtual length_t calcOutputTo( sample_t *, length_t, idx_t );

protected:

    virtual void reset() override;

private:
    // Helpers: protect childLinks() iteration against concurrent modification.
    // These use the inherited mutex() from twComponent base class to avoid
    // introducing a second mutex which could cause deadlock.
    int seekTo_nolock(offset_t newOffset);
    length_t calcOutputTo_nolock(sample_t *buffer, length_t playLen, idx_t outChannel);

    STrack &track_;
    std::atomic<offset_t> playOffset_{ 0 };  // Atomic: protects race between UI seek and audio render
    
};

#endif
