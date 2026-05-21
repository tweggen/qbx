
#include <stddef.h>
#include <stdlib.h>

#include "twgrainspec.h"

twSingleGrainSpec *twGrainSpec::getGrain( int idx )
{
    return grains_+idx;
}

void twGrainSpec::setGrains( twSingleGrainSpec *spec, int nGrains )
{
    if( !spec ) return;
    if( grains_ ) {
        ::free( grains_ );
        grains_ = NULL;
        nGrains_= 0;
    }
    nGrains_ = nGrains;
    grains_ = spec;

    // Now calculate total length.
    totalLength_ = 0;
    for( int i=0; i<nGrains; ++i ) {
        totalLength_ += spec[i].length_ 
            - (spec[i].overlapIn_+spec[i].overlapOut_ );
    }
}

int twGrainSpec::getNGrains() const
{
    return nGrains_;
}

twSingleGrainSpec *twGrainSpec::getGrainArray() const
{
    return grains_;
}

void twGrainSpec::init()
{
    twSingleGrainSpec *o = grains_;
    grains_ = NULL;
    setGrains( o, nGrains_ );
}

twGrainSpec::~twGrainSpec()
{
    delete grains_;
}

twGrainSpec::twGrainSpec( twComponent &source, 
                          twSingleGrainSpec *grains, 
                          int nGrains )
    : nGrains_( nGrains ),
      grains_( grains ),
      source_( source ),
      totalLength_( 0 )
{    
}
