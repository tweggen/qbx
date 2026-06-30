
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
    // Phase 3: New IOVector-based interface (type-safe, page-backed)
    virtual length_t calcOutputTo( IOVector& dest, idx_t idx ) override;

    // DEPRECATED: Raw-pointer interface (will be removed in v1.0)
    // See: docs/COMPONENT_MIGRATION_GUIDE.md for migration path
    [[deprecated("Use IOVector-based calcOutputTo() or freezePage() instead")]]
    virtual length_t calcOutputTo( sample_t *pDest, length_t length, idx_t ldx ) override;
    virtual void reset() override;

private:
    // Helpers: do work outside lock (caller must hold mutex)
    int setNPlugs_nolock(idx_t n);
    length_t calcOutputTo_nolock(sample_t *pDest, length_t length, idx_t idx);
    int seekTo_nolock(offset_t offset);
    twLatchOutput *linkOutput_nolock(idx_t idx);

    int nInputs_;
};

#endif
