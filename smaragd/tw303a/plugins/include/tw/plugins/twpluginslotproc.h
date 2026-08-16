#ifndef _TWPLUGINSLOTPROC_H_
#define _TWPLUGINSLOTPROC_H_

#include "tw/core/twtypes.h"
#include "tw/plugins/twplugin.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

class tw303aEnvironment;

namespace audio {

class twPluginInsert;

// Where one plugin slot stands (proposal 08 M3/M4).
//
// M3 only ever produces Active and Unsupported. Missing is declared HERE, with
// the other two, so that M4 (serialization + the missing-plugin placeholder)
// adds behaviour and persistence to an existing type instead of introducing a
// second, competing one.
enum class twPluginSlotState {
    Active = 0,     // a real plugin instance is processing
    Missing,        // the descriptor did not resolve; createNullPlugin() stands in
    Unsupported     // resolved, but its channel layout has no defined mapping
};

// How the slot maps the track's N CHANNELS onto the plugin's own channel count.
// This is the channel-mismatch table of proposal 08 §Layer 3, PRESERVED
// SEMANTICALLY by proposal 36 B4 and re-derived from PAGE WIDTH instead of bus
// count — the number moved, what a user hears did not.
enum class twPluginSlotMode {
    Transparent,    // no plugin, or Unsupported: input copied to output per channel
    Direct,         // nIn == nOut == nChannels: one instance, all channels coherently
    DualMono,       // a 1->1 plugin on N channels: N independent instances
    MonoFold        // a 2->2 plugin on ONE channel: feed both inputs, average outputs
};

// The stateful core of one plugin slot: it owns the twPlugin instance(s), the
// bypass flag, the prepare() state, the block chunking and the channel-mismatch
// mapping.
//
// WHY IT IS STILL NOT A twComponent, AFTER B4 RETIRED THE REASON IT WAS BORN.
// Until proposal 36 the frozen page was one MONO page per component, so N
// channels were N parallel component instances and a stereo-linked plugin —
// which must see all its channels in ONE process() call — could not be a
// component at all. Hence the split: N per-bus twPluginInsert TAPS around one
// out-of-band processor with its own all-bus page cache. B4 makes the page
// itself N channels wide, so ONE twPluginInsert now expresses the whole slot,
// and the tap fan-out and the all-bus cache are gone.
//
// What is left here is deliberate and is NOT graph machinery: plugin LIFETIME
// and STATE. The instance(s), prepare()/activate() bookkeeping, the mismatch
// mapping, the chunking, the position-continuity reset and the parameter/state
// surface the app writes to all outlive any particular component — proposal 08
// invariant 18's "re-resolution is setFactory(), not a new processor" depends on
// exactly that, because a slot's identity in the graph is its processor and
// swapping it would mean re-wiring every chain from the UI. Folding this into
// twPluginInsert would put a plugin's lifetime inside a component's, which is
// the thing M4 went out of its way to avoid.
//
// Threading: every public method is safe from any non-realtime thread.
// setChannelCount() (and therefore the initial construction path) calls
// twPlugin::prepare(), which for CLAP is activate() — annotated [main-thread] —
// so it must be reached from the UI thread. Nothing here may be called from the
// realtime audio callback; the RT path reads ready pages (twRtThreadGuard).
class twPluginSlotProcessor {
public:
    // The block size the host promises never to exceed. Pages are
    // twOutputPage::FRAME_CAPACITY (65536) frames, which no real plugin accepts
    // in one call, so prepare() declares this and process() never sees more.
    static constexpr length_t kChunkFrames = 4096;

    // Produces a FRESH plugin instance per call. A factory rather than one
    // instance because the dual-mono mapping needs N of them, and the mapping
    // is only known once the channel count is.
    using Factory = std::function<std::unique_ptr<twPlugin>()>;

    twPluginSlotProcessor( tw303aEnvironment &env, Factory factory,
                           const twPluginIoLayout &declaredIo );
    ~twPluginSlotProcessor();

    twPluginSlotProcessor( const twPluginSlotProcessor & )            = delete;
    twPluginSlotProcessor &operator=( const twPluginSlotProcessor & ) = delete;

    // --- configuration (UI thread) -----------------------------------------

    // Declare how many CHANNELS this slot processes — the width of the pages
    // its insert will be handed. Re-derives the channel-mismatch policy,
    // re-instantiates, and prepare()s. Idempotent, and it SHRINKS as readily as
    // it grows: proposal 36 B4 retired the per-bus component instantiation that
    // made a shrink a rewiring problem, so a project going 8 -> 2 is now one
    // call with no components created or destroyed.
    void setChannelCount( idx_t nChannels );
    idx_t channelCount() const;

    // Replace the instantiation factory and re-derive everything (proposal 08
    // M4). This is how a slot whose plugin was MISSING becomes Active after a
    // rescan found it: the insert and the twPluginChain holding it are left
    // completely alone, because they only ever reference this processor. The
    // caller re-applies its stored state chunk afterwards.
    void setFactory( Factory factory );

    // Register the slot's insert. Held WEAKLY: the insert owns the processor,
    // so a strong ref here would be a cycle. It is how bumpParamEpoch() reaches
    // the pages an edit has to stale.
    void attachTap( const std::shared_ptr<twPluginInsert> &tap );

    twPluginSlotMode  mode()  const;
    twPluginSlotState state() const;

    void setBypass( bool bypass );
    bool bypass() const { return bypass_.load( std::memory_order_acquire ); }

    // Host-side access for parameters and state chunks. plugins() returns every
    // instance, which is what the dual-mono mapping needs (a parameter edit has
    // to reach all N). Both return the live pointers; the caller must not
    // outlive the processor.
    twPlugin               *plugin()  const;   // channel 0's instance, or nullptr
    std::vector<twPlugin *> plugins() const;

    // Anything that changes what process() would produce (bypass, a parameter,
    // a state chunk) must land here, or the insert's cached pages are served
    // unchanged and the edit is inaudible. The caller still owns the DOWNSTREAM
    // path (SObject::invalidateRenderPath()).
    //
    // Before B4 this ALSO had to move a key for the processor's own all-bus page
    // cache. That cache existed to stop N sibling taps re-rendering one page N
    // times; with one insert there are no siblings, the component page cache
    // above is the only cache, and this is now just "stale the insert".
    void bumpParamEpoch();

    // --- the render --------------------------------------------------------

    // Process `len` frames of every channel. `in` and `out` are channelCount()
    // PLANAR buffers of at least `len` frames, supplied by the caller — which
    // is the insert, holding its upstream page and its own output page. They
    // must not alias; the insert's are two different pages.
    //
    // `startPos` drives the position-continuity check: the plugin is stateful,
    // so a page that does not start exactly where the last one ended is a
    // discontinuity and resets it. `positional = false` for the legacy pull,
    // which has no page identity at all.
    void render( const sample_t *const *in, sample_t **out, length_t len,
                 offset_t startPos, bool positional, int sampleRate );

private:
    // All _nolock helpers require mutex_.
    void  bumpParamEpoch_nolock();
    void  rebuild_nolock();
    void  ensureScratch_nolock();
    void  resetInstances_nolock();
    void  runChunked_nolock( const sample_t *const *in, sample_t **out, length_t len );

    tw303aEnvironment &env_;
    Factory            factory_;
    twPluginIoLayout   declaredIo_;

    mutable std::mutex mutex_;

    idx_t              nChannels_ = 0;
    twPluginSlotMode   mode_   = twPluginSlotMode::Transparent;
    twPluginSlotState  state_  = twPluginSlotState::Active;
    int                preparedRate_ = 0;

    std::vector<std::unique_ptr<twPlugin>> instances_;
    std::weak_ptr<twPluginInsert>          tap_;

    // The two extra output buffers the MonoFold mapping folds down from. One
    // CHUNK each, not one page: the fold happens inside the chunk loop.
    std::vector<std::vector<sample_t>> foldOut_;

    // Pointer arrays handed to twPlugin::process(). Members, not locals, so the
    // per-chunk loop allocates nothing (CONTRACT invariant 2).
    std::vector<const sample_t *> inPtrs_;
    std::vector<sample_t *>       outPtrs_;

    std::atomic<bool> bypass_{ false };

    // Position continuity: the plugin is stateful, so a page that does not
    // start exactly where the last one ended is a discontinuity and resets it.
    offset_t lastEnd_        = 0;
    bool     haveLastEnd_    = false;

    // "Unsupported" is logged once per slot, not once per page.
    bool     loggedUnsupported_ = false;
};

}  // namespace audio

#endif
