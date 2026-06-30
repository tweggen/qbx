
#ifndef _TW_CONSTANT_
#define _TW_CONSTANT_

#include "twosc.h"

class tw303aEnvironment;

class twConstant
    : public twComponent
{
    virtual void reset() override;

    // Tier 2 Enhancement #3: Push-based renderFrames() for sources
    // Bypass calcOutputTo() for simple stateless sources
    virtual length_t renderFrames(sample_t *output, length_t length,
                                   const sample_t *input, length_t inputLength,
                                   idx_t idx) override;

private:
    sample_t constant;
protected:
    // Phase 3: New IOVector-based interface (type-safe, page-backed)
    virtual length_t calcOutputTo( IOVector& dest, idx_t idx ) override;

    // DEPRECATED: Raw-pointer interface (will be removed in v1.0)
    // See: docs/COMPONENT_MIGRATION_GUIDE.md for migration path
    [[deprecated("Use IOVector-based calcOutputTo() or freezePage() instead")]]
    virtual length_t calcOutputTo( sample_t *pDest, length_t length, idx_t idx ) override;
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
