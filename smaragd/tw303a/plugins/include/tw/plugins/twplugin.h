#ifndef _TWPLUGIN_H_
#define _TWPLUGIN_H_

#include <cstdint>
#include <memory>
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

// The PLACEHOLDER a slot runs when its real plugin could not be instantiated
// (proposal 08 M4): an inert pass-through reporting the DECLARED layout of the
// descriptor that was saved with the project — NOT the layout of nothing.
//
// Reporting the declared I/O is the whole point. twPluginSlotProcessor derives
// the channel-mismatch mapping from the instance's own ioLayout(), so a slot
// with no instance at all would fall back to Transparent and the graph would
// have a different shape than it will have once the plugin is installed. With
// the placeholder the mapping, the instance count and the prepare() bookkeeping
// are exactly what the real plugin will get, so a reload changes only what
// process() computes.
//
// It has no parameters and its state chunk is empty: a Missing slot's authority
// on state is the app's stored blob, which must survive verbatim (a user must
// not lose their settings by opening a project on a machine without the
// plugin). SPluginSlot::saveState() therefore never reads a non-Active slot.
std::unique_ptr<twPlugin> createNullPlugin( const twPluginIoLayout &io );

}  // namespace audio

#endif
