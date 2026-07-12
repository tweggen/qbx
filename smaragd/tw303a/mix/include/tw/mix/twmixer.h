
#ifndef _TW_MIXER_
#define _TW_MIXER_

#include "tw/graph/twcomponent.h"

class tw303aEnvironment;

class twMixer : public twComponent {
    virtual void reset() override;
    virtual void teardown() override;
private:
    idx_t mixerInputs_;
    struct InputProperties {
        double volume_;
        sample_t volumeFactor_;
    };
    InputProperties *inputProperties_;

    // Helpers: do work outside lock (caller must hold mutex)
    int setNInputs_nolock(idx_t n);
    void setBufferSize_nolock(length_t newSize);
    int seekTo_nolock(offset_t offset);

protected:
    sample_t *inBuffer;

public:
    // Phase 3: IOVector-based interface (type-safe, page-backed)
    virtual length_t calcOutputTo( IOVector& dest, idx_t idx ) override;

    void createOutputLatches( void );

    void init( void );
    int setNInputs( idx_t );
    int setInputLevel( idx_t, double );
    virtual int seekTo( offset_t offset );
    virtual idx_t getNInputs() const { return mixerInputs_; }
    virtual idx_t getNOutputs() const { return 1; }
    virtual const char *getInputName( idx_t ) const;
    virtual const char *getOutputName( idx_t ) const;
    
    twMixer( tw303aEnvironment &env, idx_t inputs );
    void setBufferSize( length_t newSize );
};

#endif
