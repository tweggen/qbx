
#ifndef _TW_PIPE_
#define _TW_PIPE_

#include "twcomponent.h"

class tw303aEnvironment;

/**
 * Something.
 */
class twPipe : public twComponent {
  private:
  protected:
    // Phase 3: IOVector-based interface (type-safe, page-backed)
    virtual length_t calcOutputTo( IOVector& dest, idx_t idx ) override;

    sample_t *inBuffer;
    
  public:
    void createOutputLatches( void );
    
    void init( void );
    idx_t getNInputs() { return 1; }
    idx_t getNOutputs() { return 1; }
    char *getInputName( idx_t ) { return 0; }
    char *getOutputName( idx_t ) { return 0; }
    
    void setBufferSize( length_t length );
    twPipe( tw303aEnvironment &env0 );
    virtual void reset() override;
};

#endif
