
#ifndef _TWREWIRE_H
#define _TWREWIRE_H

#include "tw/graph/twcomponent.h"

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
public:
    twRewire( tw303aEnvironment &env );
    virtual ~twRewire();

    virtual twLatchOutput *linkOutput( idx_t idx ) override;
    virtual void allocPlugs() override;
    virtual void init() override;
    virtual void createOutputLatches() override;

    virtual int setNPlugs( idx_t );

    virtual idx_t getNInputs() const override;
    virtual idx_t getNOutputs() const override;
    virtual const char *getInputName( idx_t ) const override;
    virtual const char *getOutputName( idx_t ) const override;

    virtual int seekTo( offset_t offset ) override;

    virtual void setBufferSize( length_t ) override {};

protected:
    // Phase 3: IOVector-based interface (type-safe, page-backed)
    virtual length_t calcOutputTo( IOVector& dest, idx_t idx ) override;
    virtual void reset() override;

    // Teardown protocol
    virtual void teardown() override;

private:
    // Helpers: do work outside lock (caller must hold mutex)
    int setNPlugs_nolock(idx_t n);
    int seekTo_nolock(offset_t offset);
    twLatchOutput *linkOutput_nolock(idx_t idx);

    int nInputs_;
};

#endif
