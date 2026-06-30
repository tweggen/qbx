
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
    // Phase 3: New IOVector-based interface (type-safe, page-backed)
    virtual length_t calcOutputTo( IOVector& dest, idx_t idx ) override;

    // DEPRECATED: Raw-pointer interface (will be removed in v1.0)
    // See: docs/COMPONENT_MIGRATION_GUIDE.md for migration path
    [[deprecated("Use IOVector-based calcOutputTo() or freezePage() instead")]]
    virtual length_t calcOutputTo( sample_t *pDest, length_t length, idx_t idx ) override;
    //virtual length_t processData( sample_t *pDest, length_t length ) = 0;

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
