#include "tw/playback/twliveplan.h"

#include "tw/core/twlog.h"
#include "tw/graph/twcomponent.h"
#include "tw/mix/twmixer.h"
#include "tw/mix/twrewire.h"

#include <cmath>

bool twLivePlan::finalize()
{
    base_.clear();
    arena_.clear();

    if( blockFrames <= 0 ) {
        TW_LOGE( "playback", "[live] plan has no block size; refusing to finalize" );
        outputTrack = -1;
        return false;
    }
    if( tracks.empty() ) {
        outputTrack = -1;
        return true;   // an empty plan is legal: the pump simply produces nothing
    }
    if( outputTrack < 0 || outputTrack >= (int)tracks.size() ) {
        TW_LOGE( "playback", "[live] plan names no output track (%d of %d)",
                 outputTrack, (int)tracks.size() );
        outputTrack = -1;
        return false;
    }

    // TOPOLOGICAL ORDER IS A PRECONDITION, not a hope: the pump renders the
    // vector front to back and a folder reads its live children's buffers, so a
    // child that came later would be read before it was written. Checking it
    // here costs nothing and turns a silent one-block-old mix into a build-time
    // refusal.
    for( std::size_t i = 0; i < tracks.size(); ++i ) {
        for( int child : tracks[i].liveChildren ) {
            if( child < 0 || child >= (int)i ) {
                TW_LOGE( "playback", "[live] track %d names live child %d — not "
                         "strictly earlier in the plan", (int)i, child );
                outputTrack = -1;
                return false;
            }
        }
    }

    // Two planar blocks per track: the insert chain ping-pongs between them,
    // because twPluginSlotProcessor::render() forbids `in` and `out` aliasing.
    std::size_t total = 0;
    base_.reserve( tracks.size() );
    for( twLiveTrackPlan &t : tracks ) {
        if( t.channels < 1 ) t.channels = 1;
        base_.push_back( total );
        total += (std::size_t)t.channels * 2u * (std::size_t)blockFrames;
    }
    arena_.assign( total, 0.0f );

    // TWO blocks by default: one for the block the RT is on, one of slack for
    // a late pump wake-up. 0 is a legal explicit value (see the header).
    if( leadFrames < 0 ) leadFrames = 2 * blockFrames;
    return true;
}

float *twLivePlan::scratch( int track, int which ) const
{
    if( track < 0 || track >= (int)base_.size() ) return nullptr;
    if( which != 0 && which != 1 ) return nullptr;
    const twLiveTrackPlan &t = tracks[(std::size_t)track];
    const std::size_t half = (std::size_t)t.channels * (std::size_t)blockFrames;
    return const_cast<float *>( arena_.data() ) + base_[(std::size_t)track]
           + (std::size_t)which * half;
}

namespace twlive {

twMasterShape checkMasterShape( const twMixer *mixer, const twRewire *root,
                                idx_t width, const twMasterChainState &chain )
{
    twMasterShape out;
    auto closure = [&out]( const char *why ) {
        out.mode   = twMasterMode::Closure;
        out.reason = why;
        return out;
    };

    if( !mixer ) return closure( "no master mixer" );
    if( !root )  return closure( "no master rewire" );
    if( width < 1 ) width = 1;

    // 1. The SUM must be a sum, at the project's width. A mixer rendering a
    //    different number of channels than the root publishes is not the shape
    //    the algebra was derived for.
    if( mixer->getOutputChannels() != width )
        return closure( "master mixer width != project width" );
    if( root->getOutputChannels() != width )
        return closure( "master rewire width != project width" );

    // 2. UNITY. Every wired input contributes at exactly 0 dB; anything else is
    //    a per-input scale, and `master(a u b) == master(a) + master(b)` still
    //    holds for a LINEAR scale — but a scale is a fader, faders grow curves,
    //    and the moment one is automated the equality is over. The precondition
    //    is deliberately strict: it costs one re-check per plan build and it is
    //    the only thing standing between the split and a silent doubling.
    for( idx_t i = 0; i < mixer->getNInputs(); ++i ) {
        const double db = mixer->inputLevel( i );
        if( std::fabs( db ) > 1e-9 )
            return closure( "a master input level is not unity" );
    }

    // 3. IDENTITY MAP. An empty map IS the identity; an explicit one has to
    //    spell it out.
    const std::vector<idx_t> map = root->channelMap();
    if( !map.empty() ) {
        if( (idx_t)map.size() != width )
            return closure( "master rewire map is not the project width" );
        for( idx_t c = 0; c < width; ++c )
            if( map[(std::size_t)c] != c )
                return closure( "master rewire map is not the identity" );
    }

    // 4. THE MASTER LANE ITSELF (proposal 45 D4a). Checked LAST so the older,
    //    cheaper reasons keep their own wording, and stated as four distinct
    //    reasons rather than one: whoever reads the refusal in the status line
    //    needs to know which of their master controls turned monitoring off.
    if( chain.insertCount > 0 )
        return closure( "the master lane has inserts" );
    if( chain.muted )
        return closure( "the master lane is muted" );
    if( chain.automated )
        return closure( "the master lane's volume or mute is automated" );
    if( chain.gainDb < -1e-9 || chain.gainDb > 1e-9 )
        return closure( "the master fader is not at unity" );

    return out;   // LinearSplit
}

}  // namespace twlive
