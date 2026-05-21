
#ifndef _TWREWIRE_H
#define _TWREWIRE_H

#include "twcomponent.h"

/**
 * An object of this class the same number of inputs and outputs.
 * It presents a collection of input objects as an nChannel output 
 * object. 
 *
 * It does this without buffering, just forwarding output latches
 * from the various sources.
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

    virtual void setBufferSize( length_t ) {};

protected:
    virtual length_t calcOutputTo( sample_t *pDest, length_t length, idx_t ldx );
private:
    int nInputs_;
};

#endif
