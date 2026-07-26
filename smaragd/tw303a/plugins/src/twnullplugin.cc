#include "tw/plugins/twplugin.h"

#include <algorithm>
#include <cstring>

namespace audio {

// The missing/failed-plugin placeholder (proposal 08 M4). See the comment on
// createNullPlugin() in tw/plugins/twplugin.h for WHY this exists rather than
// simply leaving the slot without an instance.
//
// process() copies channel-wise and zeroes any output the declared layout has
// no input for, so the slot is bit-transparent for every mapping the processor
// can derive from a symmetric layout, and silent-but-valid for one it cannot.
class twNullPlugin : public twPlugin {
public:
    explicit twNullPlugin( const twPluginIoLayout &io ) : io_( io ) {}

    const twPluginIoLayout &ioLayout() const override { return io_; }

    void prepare( std::uint32_t sampleRate, std::uint32_t maxBlock ) override
    {
        (void) sampleRate;
        (void) maxBlock;
    }

    void process( const float *const *in, float *const *out,
                  std::uint32_t nframes ) override
    {
        const std::uint32_t nIn  = io_.audioInputs;
        const std::uint32_t nOut = io_.audioOutputs;
        const std::uint32_t nCopy = std::min( nIn, nOut );
        for( std::uint32_t c = 0; c < nCopy; ++c ) {
            if( !out[c] ) continue;
            if( in && in[c] ) std::memcpy( out[c], in[c], nframes * sizeof( float ) );
            else              std::memset( out[c], 0, nframes * sizeof( float ) );
        }
        for( std::uint32_t c = nCopy; c < nOut; ++c )
            if( out[c] ) std::memset( out[c], 0, nframes * sizeof( float ) );
    }

    void reset() override {}

    std::size_t paramCount() const override { return 0; }
    twPluginParamInfo paramInfo( std::size_t ) const override
    {
        return twPluginParamInfo{};
    }
    double getParam( std::uint32_t ) const override { return 0.0; }
    void   setParam( std::uint32_t, double ) override {}

    // Deliberately empty, and loadState() deliberately accepts nothing: the
    // stored blob belongs to the plugin that is NOT here. SPluginSlot keeps it
    // verbatim and re-serializes it unchanged; letting the placeholder claim to
    // own state would be how a user loses their settings.
    std::vector<std::uint8_t> saveState() const override { return {}; }
    bool loadState( const std::vector<std::uint8_t> & ) override { return false; }

private:
    twPluginIoLayout io_;
};

std::unique_ptr<twPlugin> createNullPlugin( const twPluginIoLayout &io )
{
    return std::make_unique<twNullPlugin>( io );
}

}  // namespace audio
