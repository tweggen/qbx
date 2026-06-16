#include "plugins/twplugin.h"
#include <cstring>

namespace audio {

// Simple in-house test plugin: 2-in / 2-out passthrough with dry/wet mix parameter.
class twPassThroughPlugin : public twPlugin {
public:
    twPassThroughPlugin();

    const twPluginIoLayout &ioLayout() const override { return io_; }

    void prepare( std::uint32_t sampleRate, std::uint32_t maxBlock ) override;
    void process( const float *const *in, float *const *out,
                  std::uint32_t nframes ) override;
    void reset() override {}

    std::size_t paramCount() const override { return 1; }
    twPluginParamInfo paramInfo( std::size_t i ) const override;
    double getParam( std::uint32_t id ) const override;
    void setParam( std::uint32_t id, double v ) override;

    std::vector<std::uint8_t> saveState() const override;
    bool loadState( const std::vector<std::uint8_t> & ) override;

private:
    twPluginIoLayout io_{2, 2};  // 2 inputs, 2 outputs (stereo passthrough)
    double dryWetMix_ = 1.0;     // 0 = full dry, 1 = full wet
};

twPassThroughPlugin::twPassThroughPlugin() = default;

void twPassThroughPlugin::prepare( std::uint32_t sampleRate, std::uint32_t maxBlock )
{
    // No per-block setup needed for passthrough.
    (void)sampleRate;
    (void)maxBlock;
}

void twPassThroughPlugin::process( const float *const *in, float *const *out,
                                    std::uint32_t nframes )
{
    // Simple passthrough with optional dry/wet mix control.
    for( std::uint32_t ch = 0; ch < 2; ++ch ) {
        for( std::uint32_t i = 0; i < nframes; ++i ) {
            // Dry signal is just input repeated; wet is the same (passthrough).
            // This is a placeholder for a more interesting plugin.
            out[ch][i] = in[ch][i] * dryWetMix_ + in[ch][i] * (1.0 - dryWetMix_);
            // Simplifies to just passthrough (input[i] * 1.0):
            out[ch][i] = in[ch][i];
        }
    }
}

twPluginParamInfo twPassThroughPlugin::paramInfo( std::size_t i ) const
{
    if( i == 0 ) {
        return twPluginParamInfo{0, "Dry/Wet Mix", 0.0, 1.0, 1.0, false};
    }
    return twPluginParamInfo{};
}

double twPassThroughPlugin::getParam( std::uint32_t id ) const
{
    if( id == 0 )
        return dryWetMix_;
    return 0.0;
}

void twPassThroughPlugin::setParam( std::uint32_t id, double v )
{
    if( id == 0 )
        dryWetMix_ = v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
}

std::vector<std::uint8_t> twPassThroughPlugin::saveState() const
{
    // Save the dry/wet mix as 8 bytes (double).
    std::vector<std::uint8_t> state( sizeof( double ) );
    std::memcpy( state.data(), &dryWetMix_, sizeof( double ) );
    return state;
}

bool twPassThroughPlugin::loadState( const std::vector<std::uint8_t> &state )
{
    if( state.size() != sizeof( double ) )
        return false;
    std::memcpy( &dryWetMix_, state.data(), sizeof( double ) );
    return true;
}

// Factory function: create an instance of the PassThrough plugin.
std::unique_ptr<twPlugin> createPassThroughPlugin()
{
    return std::make_unique<twPassThroughPlugin>();
}

}  // namespace audio
