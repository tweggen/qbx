#include "twplugininsert.h"
#include "plugins/twplugin.h"
#include "io_vector.h"
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

// Phase 3: IOVector-based interface (type-safe, page-backed rendering)
length_t twPluginInsert::calcOutputTo( IOVector& dest, idx_t port )
{
    std::lock_guard<std::mutex> lock(mutex());

    if( !producedThisBlock_ ) {
        // Pull every input bus into de-interleaved scratch (one mono wire each).
        for( idx_t c = 0; c < getNInputs(); ++c )
            pullInput( c, inScratch_[c].data(), dest.length() );

        // Bypass = copy in->out; process plugin otherwise.
        if( bypass_ ) {
            copyChannels( inScratch_, outScratch_, getNInputs(), dest.length() );
        } else {
            // Prepare scratch buffers with input data pointers.
            std::vector<const float *> inPtrs( getNInputs() );
            std::vector<float *> outPtrs( getNOutputs() );
            for( idx_t c = 0; c < getNInputs(); ++c )
                inPtrs[c] = inScratch_[c].data();
            for( idx_t c = 0; c < getNOutputs(); ++c )
                outPtrs[c] = outScratch_[c].data();

            plugin_->process( inPtrs.data(), outPtrs.data(), dest.length() );
        }
        producedThisBlock_ = true;
    }

    // Write requested output port to IOVector destination
    if( port < getNOutputs() ) {
        return dest.copyFrom(IOVector::CreateFromBuffer(outScratch_[port].data(), dest.length()), 0, dest.length());
    }
    return 0;
}

// Legacy: Raw-pointer interface
length_t twPluginInsert::calcOutputTo( sample_t *dst, length_t len, idx_t port )
{
    std::lock_guard<std::mutex> lock(mutex());
    return calcOutputTo_nolock(dst, len, port);
}

// Caller must hold mutex()
// CRITICAL: Lock ensures exactly one processing per block, even if calcOutputTo()
// is called concurrently for multiple output ports (common with multi-channel plugins).
// Prevents double-processing race where both threads see producedThisBlock_=false.
length_t twPluginInsert::calcOutputTo_nolock( sample_t *dst, length_t len, idx_t port )
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

void twPluginInsert::setBypass( bool bypass )
{
    std::lock_guard<std::mutex> lock(mutex());
    setBypass_nolock(bypass);
}

// Caller must hold mutex()
void twPluginInsert::setBypass_nolock( bool bypass )
{
    bypass_ = bypass;
}

int twPluginInsert::seekTo( offset_t offset )
{
    // Forward seek to input plugs (the previous stage in the chain)
    if( pInputPlugs ) {
        for( idx_t i = 0; i < getNInputs(); ++i ) {
            if( pInputPlugs[i] ) {
                twLatch &latch = pInputPlugs[i]->getParentLatch();
                twComponent &comp = latch.getComponent();
                comp.seekTo( offset );
            }
        }
    }

    return 0;
}

void twPluginInsert::reset()
{
	std::lock_guard<std::mutex> lock(mutex());
	reset_nolock();
}

// Caller must hold mutex()
void twPluginInsert::reset_nolock()
{
	// Plugin insert propagates reset to its plugin
	producedThisBlock_ = false;
	if (plugin_) {
		plugin_->reset();
	}
}

}  // namespace audio
