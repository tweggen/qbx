
#ifndef _TW_TESTSEQ_
#define _TW_TESTSEQ_

#include "twosc.h"

class twTestSeq : public twComponent {
	private:
		sample_t constant, prevConstant;
		offset_t currPos;
		sample_t portamento;
	protected:
		virtual length_t calcOutputTo( sample_t *pDest, length_t length, idx_t idx );
	public:
		void createOutputLatches( void );

		virtual void init( void );

		virtual idx_t getNInputs () { return 0; }
		virtual idx_t getNOutputs () { return 1; }
		virtual char *getInputName( idx_t ) { return 0; }
		virtual char *getOutputName( idx_t ) { return 0; }

		twTestSeq( tw303aEnvironment &, sample_t constant );
		twTestSeq( tw303aEnvironment &, sample_t constant, sample_t portamento0 );
};

#endif
