#include "twplugininsert.h"
#include "plugins/twplugin.h"
#include <cstring>
#include <algorithm>

namespace audio {

twPluginInsert::twPluginInsert( tw303aEnvironment &env, std::unique_ptr<twPlugin> plugin )
    : twComponent( env ), plugin_( std::move( plugin ) )
{
    const auto &io = plugin_->ioLayout();
    inScratch_.resize( io.audioInputs );
    outScratch_.resize( io.audioOutputs );

    for( auto &buf : inScratch_ )
        buf.resize( 4096 );
    for( auto &buf : outScratch_ )
        buf.resize( 4096 );

    // allocPlugs() and createOutputLatches() are called by init()
}

twPluginInsert::~twPluginInsert() = default;

void twPluginInsert::createOutputLatches()
{
    // Create output latches for each output port.
    idx_t nOut = getNOutputs();
    pOutputLatches = new twLatch *[nOut];
    for( idx_t i = 0; i < nOut; ++i )
        pOutputLatches[i] = new twStreamingLatch( *this, i, 4096 );
}

idx_t twPluginInsert::getNInputs() const
{
    return plugin_->ioLayout().audioInputs;
}

idx_t twPluginInsert::getNOutputs() const
{
    return plugin_->ioLayout().audioOutputs;
}

length_t twPluginInsert::calcOutputTo( sample_t *dst, length_t len, idx_t port )
{
    if( !producedThisBlock_ ) {
        // Pull every input bus into de-interleaved scratch (one mono wire each).
        for( idx_t c = 0; c < getNInputs(); ++c )
            pullInput( c, inScratch_[c].data(), len );

        // Bypass = copy in->out; process plugin otherwise.
        if( bypass_ ) {
            copyChannels( inScratch_, outScratch_, getNInputs(), len );
        } else {
            // Prepare scratch buffers with input data pointers.
            std::vector<const float *> inPtrs( getNInputs() );
            std::vector<float *> outPtrs( getNOutputs() );
            for( idx_t c = 0; c < getNInputs(); ++c )
                inPtrs[c] = inScratch_[c].data();
            for( idx_t c = 0; c < getNOutputs(); ++c )
                outPtrs[c] = outScratch_[c].data();

            plugin_->process( inPtrs.data(), outPtrs.data(), len );
        }
        producedThisBlock_ = true;
    }

    // Serve the requested output port from cache.
    if( port < getNOutputs() )
        std::memcpy( dst, outScratch_[port].data(), len * sizeof( sample_t ) );
    return len;
}

length_t twPluginInsert::pullInput( idx_t port, sample_t *dst, length_t len )
{
    if( pInputPlugs == nullptr || pInputPlugs[port] == nullptr )
        return 0;

    auto *input = static_cast<twLatchStreamingOutput *>(pInputPlugs[port]);
    return input->readStreamingData( dst, len );
}

void twPluginInsert::copyChannels( const std::vector<std::vector<sample_t>> &in,
                                   std::vector<std::vector<sample_t>> &out,
                                   idx_t nChan, length_t len )
{
    for( idx_t c = 0; c < nChan && c < out.size(); ++c ) {
        std::copy( in[c].begin(), in[c].begin() + len, out[c].begin() );
    }
}

}  // namespace audio
