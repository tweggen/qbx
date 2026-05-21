
#ifndef _TW_CONSTANT_
#define _TW_CONSTANT_

#include "twosc.h"

class tw303aEnvironment;

class twConstant
    : public twComponent
{
private:
    sample_t constant;
protected:
    virtual length_t calcOutputTo( sample_t *pDest, length_t length, idx_t idx );
public:
    void createOutputLatches( void );
    
    virtual void init( void );
    
    virtual idx_t getNInputs() const { return 0; }
    virtual idx_t getNOutputs() const { return 1; }
    virtual const char *getInputName( idx_t ) const { return 0; }
    virtual const char *getOutputName( idx_t ) const { return "White noise output."; }
    
    twConstant( tw303aEnvironment &env, sample_t constant );
};

#endif
