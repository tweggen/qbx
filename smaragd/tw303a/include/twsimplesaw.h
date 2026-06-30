
#ifndef _TW_SIMPLE_SAW_
#define _TW_SIMPLE_SAW_

#include "twosc.h"

class twSimpleSaw : public twOsc {
	private:
	protected:
		// Phase 3: New IOVector-based interface (type-safe, page-backed)
		virtual length_t calcOutputTo( IOVector& dest, idx_t idx ) override;

		// DEPRECATED: Raw-pointer interface (will be removed in v1.0)
		// See: docs/COMPONENT_MIGRATION_GUIDE.md for migration path
		[[deprecated("Use IOVector-based calcOutputTo() or freezePage() instead")]]
		virtual length_t calcOutputTo( sample_t *pDest, length_t length, idx_t idx ) override;

		sample_t *freqBuffer;

	public:
		void createOutputLatches( void );

		virtual void init( void );
		virtual char *getInputName( idx_t ) { return 0; }
		virtual char *getOutputName( idx_t ) { return 0; }
		twSimpleSaw( tw303aEnvironment & );

		void setBufferSize( length_t newSize );
};

#endif
