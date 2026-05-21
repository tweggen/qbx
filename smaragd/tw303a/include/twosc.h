
#ifndef _TWOSC_H_
#define _TWOSC_H_

#include "twcomponent.h"

class tw303aEnvironment;

class twOsc
    : public twComponent 
{
private:
protected:
    offset_t currPos;
public:
    virtual const char *getInputName( idx_t ) const { return 0; }
    virtual const char *getOutputName( idx_t ) const { return 0; }
    virtual idx_t getNInputs() const { return 1; }
    virtual idx_t getNOutputs() const { return 1; }
    
    twOsc( tw303aEnvironment &env0 ) : twComponent( env0 ){};
    ~twOsc() {} ;
};

#endif
