
#include "twcomponent.h"
#include "tw303aenv.h"

tw303aEnvironment::tw303aEnvironment()
    : candidateRates_{ 44100, 48000, 88200, 96000 }
{
    sampleRate = 44100;
}


tw303aEnvironment::~tw303aEnvironment()
{
	//foo
}

void tw303aEnvironment::setSRate( int rate )
{
    if( rate <= 0 || rate == sampleRate ) return;
    int oldRate = sampleRate;
    sampleRate = rate;
    emit sampleRateChanged( oldRate, rate );
}

void tw303aEnvironment::setCandidateRates( std::vector<std::uint32_t> rates )
{
    if( rates.empty() || rates == candidateRates_ ) return;
    candidateRates_ = std::move( rates );
    emit candidateRatesChanged();
}

