#include "tw/plugins/twpluginauxtap.h"

#include "tw/core/twlog.h"
#include "tw/graph/tw303aenv.h"

#include <algorithm>
#include <cstring>
#include <sstream>
#include <vector>

namespace audio {

namespace {

twPluginAuxTap::Mapping decideMapping( idx_t outChannels, std::uint16_t busChannels )
{
    using M = twPluginAuxTap::Mapping;
    const idx_t a = (idx_t)busChannels;
    if( a <= 0 || outChannels <= 0 ) return M::Refused;
    if( a == outChannels ) return M::Copy;
    if( a == 1 && outChannels > 1 ) return M::Spread;
    if( a == 2 && outChannels == 1 ) return M::Fold;
    return M::Refused;
}

}  // namespace

twPluginAuxTap::twPluginAuxTap( tw303aEnvironment &env, idx_t outChannels,
                                std::uint16_t bus, std::uint16_t busChannels )
    : twComponent( env ),
      outChannels_( outChannels < 1 ? 1 : outChannels ),
      bus_( bus ),
      busChannels_( busChannels ),
      mapping_( decideMapping( outChannels < 1 ? 1 : outChannels, busChannels ) )
{
    if( mapping_ == Mapping::Refused ) {
        // Once, at wiring: the table is consulted here and nowhere else.
        TW_LOGW( "plugins", "[aux] no defined mapping for a %u-channel aux bus %u onto "
                 "a %d-channel page (proposal 55 D4); the tap is REFUSED and renders "
                 "silence", (unsigned)busChannels_, (unsigned)bus_, (int)outChannels_ );
    }
}

const char *twPluginAuxTap::mappingName( Mapping m )
{
    switch( m ) {
    case Mapping::Copy:    return "copy";
    case Mapping::Spread:  return "spread";
    case Mapping::Fold:    return "fold";
    case Mapping::Refused: return "refused";
    }
    return "?";
}

std::string twPluginAuxTap::describe() const
{
    std::ostringstream os;
    os << "bus=" << bus_ << "|busChannels=" << busChannels_
       << "|width=" << outChannels_ << "|mapping=" << mappingName( mapping_ );
    return os.str();
}

void twPluginAuxTap::createOutputLatches()
{
    pOutputLatches_.resize( 1 );
    pOutputLatches_[0] = std::make_shared<twStreamingLatch>( shared_from_this(), 0, 4096 );
}

int twPluginAuxTap::seekTo( offset_t offset )
{
    // As twPluginInsert::seekTo: freezePage_nolock seeks to the page start just
    // before the render, and the producer is read POSITIONALLY, so nothing is
    // forwarded upstream.
    renderPos_.store( offset, std::memory_order_relaxed );
    return 0;
}

length_t twPluginAuxTap::renderCore_( sample_t **outCh, idx_t nCh, offset_t startPos,
                                      length_t frames )
{
    for( idx_t c = 0; c < nCh; ++c ) std::fill( outCh[c], outCh[c] + frames, 0.0f );
    if( mapping_ == Mapping::Refused ) return frames;

    std::shared_ptr<twOutputPage> up = fetchInputPage( 0, startPos );
    const bool haveUp = up && up->validAspects != 0 && up->startPosition == startPos;
    std::shared_ptr<twOutputPage> side = haveUp ? up->aux : nullptr;

    // Find THIS bus in the layout the side page itself carries -- never in a
    // layout read from anywhere else, which could belong to another mask.
    const twAuxBusSlice *slice = nullptr;
    if( side ) {
        for( const twAuxBusSlice &s : side->auxSlices )
            if( s.bus == bus_ ) { slice = &s; break; }
    }
    if( !slice || slice->channels != busChannels_ ||
        (std::size_t)slice->offset + slice->channels > (std::size_t)side->channels() ||
        side->startPosition != startPos ) {
        silentRenders_.fetch_add( 1, std::memory_order_relaxed );
        return frames;
    }

    const length_t m = std::min<length_t>(
        frames, std::min<length_t>( (length_t)side->validFrames,
                                    (length_t)side->channelFrames() ) );
    if( m <= 0 ) return frames;

    const idx_t off = (idx_t)slice->offset;
    switch( mapping_ ) {
    case Mapping::Copy:
        for( idx_t c = 0; c < nCh; ++c )
            std::memcpy( outCh[c], side->channelPtr( off + c ),
                         (std::size_t)m * sizeof( sample_t ) );
        break;
    case Mapping::Spread: {
        const sample_t *s = side->channelPtr( off );
        for( idx_t c = 0; c < nCh; ++c )
            std::memcpy( outCh[c], s, (std::size_t)m * sizeof( sample_t ) );
        break;
    }
    case Mapping::Fold: {
        const sample_t *a = side->channelPtr( off );
        const sample_t *b = side->channelPtr( off + 1 );
        sample_t       *d = outCh[0];
        for( length_t i = 0; i < m; ++i ) d[i] = 0.5f * ( a[i] + b[i] );
        break;
    }
    case Mapping::Refused:
        break;
    }
    return frames;
}

length_t twPluginAuxTap::renderPageWide( twOutputPage &page, length_t frames,
                                         const sample_t *, length_t )
{
    length_t n = std::min<length_t>( frames, (length_t)page.channelFrames() );
    if( n <= 0 ) return 0;
    const idx_t nCh = (idx_t)page.channels();
    std::vector<sample_t *> outCh( (std::size_t)nCh );
    for( idx_t c = 0; c < nCh; ++c ) outCh[c] = page.channelPtr( c );
    return renderCore_( outCh.data(), nCh, page.startPosition, n );
}

length_t twPluginAuxTap::renderPageNarrow( twOutputPage &page, length_t frames,
                                           const sample_t *, length_t )
{
    length_t n = std::min<length_t>( frames, (length_t)page.channelFrames() );
    if( n <= 0 ) return 0;
    sample_t *outCh[1] = { page.channelPtr( 0 ) };
    return renderCore_( outCh, 1, page.startPosition, n );
}

length_t twPluginAuxTap::renderFrames( sample_t *output, length_t length,
                                       const sample_t *, length_t, idx_t idx )
{
    if( !output || length <= 0 || idx != 0 ) return 0;
    sample_t *outCh[1] = { output };
    return renderCore_( outCh, 1, renderPos_.load( std::memory_order_relaxed ), length );
}

}  // namespace audio
