
#ifndef _TWOSC_H_
#define _TWOSC_H_

#include "tw/graph/twcomponent.h"

class tw303aEnvironment;

class twOsc
    : public twComponent 
{
    virtual void reset() override;
private:
protected:
    offset_t currPos;
public:
    virtual const char *getInputName( idx_t ) const override { return 0; }
    virtual const char *getOutputName( idx_t ) const override { return 0; }
    virtual idx_t getNInputs() const override { return 1; }
    virtual idx_t getNOutputs() const override { return 1; }
    
    twOsc( tw303aEnvironment &env0 ) : twComponent( env0 ){};
    ~twOsc() {} ;
};

#endif
