
#ifndef _TW_SIMPLE_SAW_
#define _TW_SIMPLE_SAW_

#include "twosc.h"

class twSimpleSaw : public twOsc {
	private:
	protected:
		virtual length_t calcOutputTo( sample_t *pDest, length_t length, idx_t idx );

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
