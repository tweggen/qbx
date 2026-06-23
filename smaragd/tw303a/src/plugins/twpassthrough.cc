#include "plugins/twplugin.h"
#include <cstring>
#include <memory>

namespace audio {

// Test plugin: 2-in / 2-out bit-crusher.
// Downsamples by repeating every 8th sample 8 times, creating a lo-fi effect.
class twPassThroughPlugin : public twPlugin {
public:
    twPassThroughPlugin();

    const twPluginIoLayout &ioLayout() const override { return io_; }

    void prepare( std::uint32_t sampleRate, std::uint32_t maxBlock ) override;
    void process( const float *const *in, float *const *out,
                  std::uint32_t nframes ) override;
    void reset() override {
        samplePos_ = 0;
        heldValueL_ = 0.0f;
        heldValueR_ = 0.0f;
    }

    std::size_t paramCount() const override { return 1; }
    twPluginParamInfo paramInfo( std::size_t i ) const override;
    double getParam( std::uint32_t id ) const override;
    void setParam( std::uint32_t id, double v ) override;

    std::vector<std::uint8_t> saveState() const override;
    bool loadState( const std::vector<std::uint8_t> & ) override;

private:
    twPluginIoLayout io_{2, 2};  // 2 inputs, 2 outputs (stereo)
    double dryWetMix_ = 1.0;     // 0 = dry (passthrough), 1 = full wet (bit-crushed)

    // State for bit-crushing: track position in 8-sample hold period
    int samplePos_ = 0;
    float heldValueL_ = 0.0f;
    float heldValueR_ = 0.0f;
};

twPassThroughPlugin::twPassThroughPlugin() = default;

void twPassThroughPlugin::prepare( std::uint32_t sampleRate, std::uint32_t maxBlock )
{
    (void)sampleRate;
    (void)maxBlock;
}

void twPassThroughPlugin::process( const float *const *in, float *const *out,
                                    std::uint32_t nframes )
{
    // Bit-crusher: output every 8th sample, repeated 8 times (downsampling).
    // On sample positions 0, 8, 16, etc., latch the current input.
    // For all 8 positions, output the held value (mixed with dry).
    for( std::uint32_t i = 0; i < nframes; ++i ) {
        #if 0
        out[0][i] = in[0][i];
        out[1][i] = in[1][i];
        #else
        // Every 8 samples, latch the current input as the held value
        if( samplePos_ == 0 ) {
            heldValueL_ = in[0][i];
            heldValueR_ = in[1][i];
        }

        // Output: mix between dry (input) and wet (bit-crushed held value)
        out[0][i] = in[0][i] * (1.0f - (float)dryWetMix_) + heldValueL_ * (float)dryWetMix_;
        out[1][i] = in[1][i] * (1.0f - (float)dryWetMix_) + heldValueR_ * (float)dryWetMix_;

        // Advance position in the 8-sample hold cycle
        samplePos_ = (samplePos_ + 1) % 8;
        #endif
    }
}

twPluginParamInfo twPassThroughPlugin::paramInfo( std::size_t i ) const
{
    if( i == 0 ) {
        return twPluginParamInfo{0, "Dry/Wet Mix", 0.0, 1.0, 0.0, false};
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

// Factory function: create an instance of the bit-crusher plugin.
std::unique_ptr<twPlugin> createPassThroughPlugin()
{
    return std::make_unique<twPassThroughPlugin>();
}

}  // namespace audio
