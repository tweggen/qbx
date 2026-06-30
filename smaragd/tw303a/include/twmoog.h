
#ifndef _TW_MOOG_
#define _TW_MOOG_

#include "twosc.h"

class tw303aEnvironment;

class twMoog : public twComponent {
	private:
		double resonance;
		double out1, out2, out3, out4;
		double in1, in2, in3, in4;
		
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
		idx_t getNInputs() { return 2; }
		idx_t getNOutputs() { return 1; }
		virtual char *getInputName( idx_t  ) { return 0; }
		virtual char *getOutputName( idx_t  ) { return 0; }
		twMoog( tw303aEnvironment &env0, double reso );
		void setBufferSize( length_t newSize );
};

#endif
