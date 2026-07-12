#ifndef _TWPLUGIN_H_
#define _TWPLUGIN_H_

#include <cstdint>
#include <string>
#include <vector>

namespace audio {

struct twPluginIoLayout {
    std::uint16_t audioInputs  = 0;   // channel counts (mono wires)
    std::uint16_t audioOutputs = 0;
};

struct twPluginParamInfo {
    std::uint32_t id;
    std::string   name;
    double        minValue, maxValue, defaultValue;
    bool          isStepped = false;
};

// Host-facing plugin interface. Deliberately narrow; format-specific behavior
// (native editor, note input) lives behind capability-queried extension interfaces.
class twPlugin {
public:
    virtual ~twPlugin() = default;

    virtual const twPluginIoLayout &ioLayout() const = 0;

    // Real-time: called on the audio thread. De-interleaved, one buffer per
    // channel; nframes <= the value passed to prepare().
    virtual void prepare( std::uint32_t sampleRate, std::uint32_t maxBlock ) = 0;
    virtual void process( const float *const *in, float *const *out,
                          std::uint32_t nframes ) = 0;
    virtual void reset() = 0;

    // Parameters (host-drawn UI + automation/undo).
    virtual std::size_t        paramCount() const = 0;
    virtual twPluginParamInfo  paramInfo( std::size_t i ) const = 0;
    virtual double             getParam( std::uint32_t id ) const = 0;
    virtual void               setParam( std::uint32_t id, double v ) = 0; // RT-safe path

    // Opaque state chunk for serialization.
    virtual std::vector<std::uint8_t> saveState() const = 0;
    virtual bool loadState( const std::vector<std::uint8_t> & ) = 0;

    virtual std::uint32_t reportedLatency() const { return 0; }

    // Capabilities — keep the core narrow; query for the rest.
    virtual bool supportsNativeEditor() const { return false; }
    virtual bool acceptsNotes()         const { return false; } // future: instruments
};

}  // namespace audio

#endif
