
#ifndef _TWREWIRE_H
#define _TWREWIRE_H

#include "twcomponent.h"

/**
 * Patch-bay component with N paired inputs and outputs (output[i] = input[i]).
 *
 * Owns one twStreamingLatch per output index. calcOutputTo() pulls from the
 * matching input plug, or fills with silence if no input is wired. This
 * lets downstream consumers (e.g. the speaker) wire to a stable latch once
 * and have the rewire transparently swap its source over time, instead of
 * holding a snapshot pointer to whichever component happened to be wired
 * in at link time.
 */
class twRewire
    : public twComponent
{
    Q_OBJECT
public:
    twRewire( tw303aEnvironment &env );
    virtual ~twRewire();

    virtual twLatchOutput *linkOutput( idx_t idx );
    virtual void allocPlugs();
    virtual void init();
    virtual void createOutputLatches();

    virtual int setNPlugs( idx_t );

    virtual idx_t getNInputs() const;
    virtual idx_t getNOutputs() const;
    virtual const char *getInputName( idx_t ) const;
    virtual const char *getOutputName( idx_t ) const;

    virtual int seekTo( offset_t offset );

    virtual void setBufferSize( length_t ) {};

protected:
    virtual length_t calcOutputTo( sample_t *pDest, length_t length, idx_t ldx );
private:
    int nInputs_;
};

#endif
