#ifndef _TWPLUGIN_H_
#define _TWPLUGIN_H_

#include "tw/plugins/twpluginevents.h"
#include "tw/plugins/twplugineditor.h"

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

// twplugineditor.h is INCLUDED, not forward-declared, and the reason is worth
// recording because the opposite was tried first: createEditor() returns a
// std::unique_ptr<twPluginEditor> with an inline `return nullptr;` default, and
// destroying that temporary instantiates default_delete, which needs a COMPLETE
// type. A forward declaration fails to compile in every TU that includes this
// header — eight of them, none of which mention editors.
//
// The include costs nothing measurable: twplugineditor.h pulls <cstdint>,
// <memory> and <vector>, all three of which this header already had, and it
// carries no Qt, no platform header and — by plugins/CONTRACT.md inv. 4 — no
// format type. There was no narrowness to protect.

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

    // Display-only formatting of a parameter value in the plugin's own units
    // (e.g. "-6.0 dB", "440 Hz", "Sine"). Empty string == the plugin offers no
    // formatter; the host then falls back to a numeric rendering. Best-effort,
    // UI-thread callable, NEVER on the audio path. `v` is in the SAME domain the
    // host holds for this parameter (native for CLAP/AU, normalized [0,1] for
    // VST3) — exactly what getParam() returns.
    virtual std::string paramValueText( std::uint32_t id, double v ) const { return {}; }

    // Opaque state chunk for serialization.
    virtual std::vector<std::uint8_t> saveState() const = 0;
    virtual bool loadState( const std::vector<std::uint8_t> & ) = 0;

    virtual std::uint32_t reportedLatency() const { return 0; }

    // Capabilities — keep the core narrow; query for the rest.
    //
    // supportsNativeEditor() stays the CHEAP PROBE: the FX strip asks it to
    // decide which editor to open, without instantiating anything.
    virtual bool supportsNativeEditor() const { return false; }

    // The plugin's OWN user interface (proposal 33). Null means "no native
    // editor" — the placeholder, every GUI-less plugin, and any backend that
    // has not implemented one — and the host then falls back to the generic
    // parameter editor. Never a hard failure.
    //
    // UI/MAIN THREAD ONLY, and the returned object is main-thread-only in every
    // method (all three formats require it: CLAP annotates clap_plugin_gui
    // [main-thread], VST3's IPlugView is UI-thread, an AU Cocoa view is AppKit).
    //
    // The editor is a SEPARATE object with an INDEPENDENT lifetime rather than
    // more virtuals here, which is what keeps this interface narrow and what
    // makes reloadPlugin() clean: destroy the editor, ask the NEW instance for a
    // fresh one, never re-initialise in place on a dangling this. It must be
    // destroyed BEFORE the twPlugin that produced it — every format requires the
    // view to die before the instance.
    //
    // Under DualMono a slot holds N instances (twpluginslotproc.cc:484-491) and
    // only instances_[0] gets an editor. Every parameter change the editor
    // reports must therefore be replayed onto the others, which is what routing
    // it through SSetPluginParamAction buys — see proposal 33 §2.
    virtual std::unique_ptr<twPluginEditor> createEditor() { return nullptr; }

    // Kept as a FORWARDER for one release (proposal 37 §5.1). New code asks
    // capabilities(); a backend overrides capabilities() and gets this for
    // free, and the pre-36 call sites keep compiling and keep meaning what they
    // meant. Do not override both in one backend.
    virtual bool acceptsNotes() const { return capabilities().acceptsNotes; }

    // --- events (proposal 37 P2) --------------------------------------------

    // What this instance can do with events. Derived once at instantiation by
    // every backend, never per call.
    virtual twPluginCapabilities capabilities() const { return twPluginCapabilities{}; }

    // AUDIO OUTPUT buses. Bus 0 is the main bus and always agrees with
    // ioLayout().audioOutputs; buses above it are aux outputs, numbered in the
    // format's own order with the main bus taken out. Since proposal 55 M1 a
    // ROUTED aux bus reaches the host: the slot processor hands process() a
    // non-null outBuses[b] for it (see setRoutedOutBuses). A plugin with no
    // audio output at all reports 0 buses.
    virtual std::size_t audioOutBusCount() const
    {
        return ioLayout().audioOutputs > 0 ? 1u : 0u;
    }
    virtual twPluginBusInfo audioOutBus( std::size_t i ) const
    {
        twPluginBusInfo b;
        if( i == 0 && ioLayout().audioOutputs > 0 ) {
            b.channels = ioLayout().audioOutputs;
            b.isMain   = true;
        }
        return b;
    }

    // AUDIO INPUT buses (proposal 55 §3.1). Bus 0 is the main input and
    // agrees with ioLayout().audioInputs; buses above it are the format's
    // non-main inputs (a sidechain) in its own order. Since proposal 55 M3 the
    // FIRST non-main input, bus 1, is fed when the slot is KEYED: the slot
    // processor hands processBuses() a non-null inBuses[1] for it (see
    // setRoutedInBuses). A plugin with no audio input at all reports 0 buses.
    virtual std::size_t audioInBusCount() const
    {
        return ioLayout().audioInputs > 0 ? 1u : 0u;
    }
    virtual twPluginBusInfo audioInBus( std::size_t i ) const
    {
        twPluginBusInfo b;
        if( i == 0 && ioLayout().audioInputs > 0 ) {
            b.channels = ioLayout().audioInputs;
            b.isMain   = true;
        }
        return b;
    }

    // WHICH AUX OUTPUT BUSES THE HOST WILL READ (proposal 55 M1): bit b set =
    // bus b is routed (bit 0, the main bus, is ignored). Takes effect at the
    // NEXT prepare(), which the slot processor calls right after a change --
    // a format that must ACTIVATE a bus before it may be used (VST3's
    // activateBus) does it there, never per block. In process() a routed bus
    // gets a non-null outBuses[b] whose channels the plugin must fill; an
    // unrouted one gets nullptr and the backend substitutes its own scratch,
    // as every backend has always done for every non-main bus. The default
    // does nothing: a backend that never writes an aux bus needs no mask.
    virtual void setRoutedOutBuses( std::uint32_t busMask ) { (void)busMask; }

    // WHICH AUX INPUT BUSES THE HOST WILL FEED (proposal 55 M3): bit b set =
    // input bus b carries host audio (a sidechain key); bit 0, the main
    // input, is ignored. The input twin of setRoutedOutBuses, with the same
    // contract: it takes effect at the NEXT prepare(), which the slot
    // processor calls right after a change (VST3 activates the bus there),
    // and in processBuses() a fed bus gets a non-null inBuses[b] while every
    // other non-main input gets nullptr and the backend's own silent scratch.
    // The default does nothing: a plugin that never reads a sidechain needs
    // no mask.
    virtual void setRoutedInBuses( std::uint32_t busMask ) { (void)busMask; }

    // How long the plugin keeps producing after its input goes silent (a
    // reverb tail, a synth release). Frames, at the prepared rate. Used to size
    // the instrument pre-roll and the project end (proposal 37 D4); 0 means
    // "no tail" and is the honest answer for a pure gain.
    virtual std::uint32_t tailFrames() const { return 0; }

    // The EVENT-AWARE render call.
    //
    //   in        - de-interleaved inputs, ioLayout().audioInputs channels
    //   outBuses  - outBuses[bus][channel], audioOutBusCount() buses; an
    //               entry above 0 may be nullptr (an unrouted aux bus)
    //   nframes   - <= the maxBlock passed to prepare()
    //   events    - host-owned, SORTED, times CHUNK-RELATIVE (0..nframes-1)
    //   eventsOut - host-owned sink; overflow is counted and dropped
    //   ctx       - project position / transport, with validFlags to say which
    //               fields are real
    //
    // The DEFAULT forwards to the legacy process() with the main bus, which is
    // what keeps every pre-36 backend, the placeholder and the tests compiling
    // and BYTE-IDENTICAL: the legacy path is not merely equivalent, it is the
    // same code. A backend that overrides this must leave the legacy overload
    // producing exactly what it produced before (the effect goldens compare
    // bytes), which in practice means the legacy one delegates here with an
    // empty list and an invalid context.
    virtual void process( const float *const *in, float *const *const *outBuses,
                          std::uint32_t nframes, const twEventList &events,
                          twEventOut &eventsOut, const twProcessContext &ctx )
    {
        (void)events;
        (void)eventsOut;
        (void)ctx;
        process( in, ( outBuses && audioOutBusCount() > 0 ) ? outBuses[0] : nullptr,
                 nframes );
    }

    // The BUS-AWARE render call (proposal 55 D5, M3): the event-aware call
    // with the INPUT side bus-indexed too.
    //
    //   inBuses   - inBuses[bus][channel], audioInBusCount() entries; entry 0
    //               is the main input (what `in` is above), an entry above 0
    //               is non-null only for a bus in the mask setRoutedInBuses()
    //               gave (a keyed sidechain), and may be read only then
    //
    // Everything else is the event-aware call's. A distinct NAME rather than
    // a third `process` overload: a generator passes `in` as a literal
    // nullptr, which would make the two pointer overloads ambiguous, and a
    // backend overriding one `process` would hide the other.
    //
    // The DEFAULT forwards inBuses[0] to the event-aware call - plugins inv.
    // 33's byte-identity trick again: a backend that does not override this
    // changes by not one instruction, and the slot processor calls it only
    // while a key is APPLIED (an unkeyed slot makes exactly the calls it made
    // before M3).
    virtual void processBuses( const float *const *const *inBuses,
                               float *const *const *outBuses, std::uint32_t nframes,
                               const twEventList &events, twEventOut &eventsOut,
                               const twProcessContext &ctx )
    {
        process( ( inBuses && audioInBusCount() > 0 ) ? inBuses[0] : nullptr, outBuses,
                 nframes, events, eventsOut, ctx );
    }
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
// `caps` lets the placeholder also report the DECLARED event shape, for the same
// reason it reports the declared I/O: a slot whose missing plugin was an
// instrument must keep looking like one (proposal 37 P2). Defaulted, so every
// pre-36 call site is unchanged and still gets an inert audio pass-through.
std::unique_ptr<twPlugin> createNullPlugin( const twPluginIoLayout &io,
                                            const twPluginCapabilities &caps
                                                = twPluginCapabilities{} );

}  // namespace audio

#endif
