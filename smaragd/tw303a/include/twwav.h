
#ifndef _TWWAV_H_
#define _TWWAV_H_

#include "twcomponent.h"
#include <stdio.h>

class twWav : public twComponent {
	private:
		FILE *fp;
		length_t totalLength;

	protected:

		virtual length_t calcOutputTo( sample_t *pDest, length_t length, idx_t idx );

	public:
		~twWav();
		twWav( tw303aEnvironment &env0, char const * fileName, length_t length );

		virtual void createOutputLatches( void );

		virtual char *getInputName ( idx_t  ) { return 0; }
		virtual char *getOutputName ( idx_t ) { return 0; }
		virtual idx_t getNInputs() { return 1; }
		virtual idx_t getNOutputs() { return 0; }

		void setBufferSize( length_t ) {};
		int writeLoop();
};

#endif
