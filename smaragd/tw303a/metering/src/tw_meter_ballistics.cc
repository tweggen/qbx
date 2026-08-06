#include "tw/metering/tw_meter_ballistics.h"

#include <cmath>

float twMeterBallistics::twLinToDb( float lin, float floorDb )
{
    if( !(lin > 0.0f) ) return floorDb;          // also catches NaN
    const float db = 20.0f * std::log10( lin );
    if( !std::isfinite( db ) ) return floorDb;
    return db < floorDb ? floorDb : db;
}

twMeterBallistics::twMeterBallistics( const twMeterBallisticsConfig &cfg )
    : cfg_( cfg )
{
    reset();
}

void twMeterBallistics::reset()
{
    peakDb_       = cfg_.floorDb;
    holdDb_       = cfg_.floorDb;
    meanSquare_   = 0.0f;
    clipped_      = false;
    lastSec_      = 0.0;
    holdUntilSec_ = 0.0;
    started_      = false;
}

float twMeterBallistics::rmsDb() const
{
    // meanSquare_ is s^2, so the amplitude is its square root — take the dB of
    // the amplitude, not of the power, or the reading reads half as loud.
    return twLinToDb( std::sqrt( meanSquare_ ), cfg_.floorDb );
}

void twMeterBallistics::advance_( double nowSec, float targetMeanSquare )
{
    if( !started_ ) {
        // First call latches the clock: there is no dt to integrate over yet,
        // so nothing decays and the RMS integrator adopts the target outright.
        started_      = true;
        lastSec_      = nowSec;
        holdUntilSec_ = nowSec;
        meanSquare_   = targetMeanSquare;
        return;
    }

    double dt = nowSec - lastSec_;
    if( dt <= 0.0 ) {
        // Clock stood still or went backwards (a re-entrant tick, or a monotonic
        // clock reset). Never integrate a negative dt — that would make the
        // meter climb on its own.
        lastSec_ = nowSec;
        return;
    }

    const double prevSec = lastSec_;
    lastSec_ = nowSec;

    // --- peak: dB-linear fall, exactly integrable, hence tick-rate independent
    peakDb_ -= cfg_.decayDbPerSec * (float)dt;
    if( peakDb_ < cfg_.floorDb ) peakDb_ = cfg_.floorDb;

    // --- held tick: only the portion of dt that lies PAST the hold deadline
    // decays. Splitting the step this way is what keeps the hold frame-rate
    // independent too; gating on "now >= deadline" would make the result depend
    // on where the tick boundaries happened to fall.
    double holdDecaySec = 0.0;
    if( prevSec >= holdUntilSec_ )      holdDecaySec = dt;
    else if( nowSec > holdUntilSec_ )   holdDecaySec = nowSec - holdUntilSec_;
    if( holdDecaySec > 0.0 ) {
        holdDb_ -= cfg_.holdDecayDbPerSec * (float)holdDecaySec;
        if( holdDb_ < cfg_.floorDb ) holdDb_ = cfg_.floorDb;
    }

    // --- RMS: one-pole toward the target with alpha derived from the ACTUAL dt.
    // 1 - exp(-dt/tau) is the exact solution of the continuous filter, so N
    // small steps and one big step of the same total duration agree to within
    // floating-point noise (metering_test asserts this).
    if( cfg_.rmsTauSec > 0.0f ) {
        const float alpha = (float)(1.0 - std::exp( -dt / (double)cfg_.rmsTauSec ));
        meanSquare_ += alpha * (targetMeanSquare - meanSquare_);
    } else {
        meanSquare_ = targetMeanSquare;
    }
    if( meanSquare_ < 0.0f ) meanSquare_ = 0.0f;
}

void twMeterBallistics::push( const twLevelSample &s, double nowSec )
{
    if( s.frames == 0 ) {           // "no measurement" is not "silence"
        idle( nowSec );
        return;
    }

    advance_( nowSec, s.meanSquare );

    const float pdb = twLinToDb( s.peak, cfg_.floorDb );
    if( pdb > peakDb_ ) peakDb_ = pdb;      // instantaneous attack

    if( peakDb_ > holdDb_ ) {
        holdDb_       = peakDb_;
        holdUntilSec_ = nowSec + (double)cfg_.holdSec;
    }

    if( s.clipped ) clipped_ = true;        // latches until clearClip()
}

void twMeterBallistics::idle( double nowSec )
{
    advance_( nowSec, 0.0f );
}
