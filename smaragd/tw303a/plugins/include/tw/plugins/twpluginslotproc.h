#ifndef _TWPLUGINSLOTPROC_H_
#define _TWPLUGINSLOTPROC_H_

#include "tw/core/twtypes.h"
#include "tw/events/twautomationcurve.h"
#include "tw/events/tweventsource.h"
#include "tw/events/twtempomap.h"
#include "tw/plugins/twplugin.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
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
//
// The GENERATOR rows (proposal 37 P3b, design 4.3) were added when a 0-input
// plugin stopped being Unsupported. They are the same table asked of a plugin
// that has no audio input at all, and they all share one extra rule: the
// slot's audio input is NOT consumed by the plugin, so the host SUMS it onto
// the generator's output ("pass-through sum", D3) and an audio clip on an
// instrument track stays audible. `x + 0.0f == x`, so an instrument with no
// notes leaves the render byte-identical to the no-instrument one.
enum class twPluginSlotMode {
    Transparent,    // no plugin, or Unsupported: input copied to output per channel
    Direct,         // nIn == nOut == nChannels: one instance, all channels coherently
    DualMono,       // a 1->1 plugin on N channels: N independent instances
    MonoFold,       // a 2->2 plugin on ONE channel: feed both inputs, average outputs

    DirectGen,      // 0 -> C on a C-channel page: one generator, channel for channel
    MonoSpread,     // 0 -> 1 on a C>1 page: the mono voice on every channel
    GenFold,        // 0 -> 2 on a ONE-channel page: average the pair down
    WideGen         // 0 -> M with M > C: outs 0..C-1 to the page, the surplus
                    // into the slot's own buffer for the aux taps of 5.4 (P9)
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

    // --- the instrument slot (proposal 37 P3b, design 4.3) -------------------

    // True when the mapping is one of the GENERATOR rows, i.e. the plugin has
    // no audio input and produces its output from EVENTS. Derived in
    // rebuild_nolock() from the instance's own ioLayout(), never from the
    // descriptor: a project whose scan cache is stale must not be able to make
    // the host feed a plugin buffers it did not ask for.
    bool isGenerator() const;

    // THE FEED. A track's twEventSource (its own event clip set merged with the
    // feeds of the children that bubble events up, design 3.2.1) — the
    // processor cannot tell a merge from a plain clip set and does not care.
    //
    // Held by shared_ptr and swapped under mutex_, so a render already holding
    // one keeps a live source for its whole duration. The APP refreshes the
    // merge's SOURCES on the main thread (STrack::eventFeed()); nothing here
    // ever walks the model.
    void setEventSource( std::shared_ptr<const twEventSource> source );
    bool hasEventSource() const;

    // The transport half of twProcessContext (F5). A value copy, set from the
    // project's tempo map on the UI thread; `valid` false means "we do not know
    // the tempo", which is reported as such rather than as a default 120.
    void setTempoMap( const twTempoMap &map, bool valid = true );

    // How long the plugin keeps producing after its last event — the max over
    // the instances. Sizes the pre-roll AND the project end (D4 / 3.1).
    std::uint32_t tailFrames() const;

    // FORGET WHERE WE WERE. The run barrier (D4, built in P3c) needs this: an
    // epoch bump does NOT clear lastEnd_, so a render whose first page starts
    // exactly where the previous run stopped would otherwise CONTINUE that
    // run's voices instead of chasing them. Reached through SPluginSlot.
    void forgetContinuity();

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

    // --- automation (proposal 37 P5, design D5 / §4.5) ----------------------

    // How often a CONTINUOUS segment is re-stated inside a chunk. 64 frames is
    // 1.33 ms at 48 kHz: fine enough that a plugin which does NOT interpolate
    // between parameter points still sounds like a ramp, coarse enough that a
    // 4096-frame chunk costs 64 events per automated parameter and stays well
    // inside twEventLimits::kMaxEventsPerBlock.
    static constexpr length_t kAutoRampFrames = 64;

    // The lanes, by parameter id, in the plugin's HOST-FACING domain (native
    // for CLAP/AU, normalized for VST3 — plugins inv. 26, the same domain
    // set-plugin-param writes). Swapped WHOLE under mutex_; a render already
    // running keeps the map it started with.
    //
    // THE EPOCH IS THE HASH (design D5). Every swap bumps automationEpoch_ AND
    // stales the insert's pages through bumpParamEpoch_nolock() — an automated
    // parameter is not observable in any other way, so a curve that changed
    // without a bump would be served from cache and be completely inaudible.
    void setParamCurves(
        std::map<std::uint32_t, std::shared_ptr<const twAutomationCurve> > curves );
    bool hasParamCurves() const;
    /// Monotonic; enters the insert's page stamp by way of bumpParamEpoch().
    std::uint64_t automationEpoch() const
    {
        return automationEpoch_.load( std::memory_order_acquire );
    }

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

    // The pre-roll floor and ceiling of design D4's
    //     K = min( max(4096, tailFrames(), P - start(earliest held note)), 4 s )
    // The floor is one chunk — enough for a filter to settle; the ceiling
    // bounds the cost, and a note held longer than four seconds has converged.
    static constexpr length_t kMinPreRollFrames = 4096;
    static constexpr int      kMaxPreRollSeconds = 4;

private:
    // All _nolock helpers require mutex_.
    void  bumpParamEpoch_nolock();
    void  rebuild_nolock();
    void  ensureScratch_nolock();
    void  resetInstances_nolock();
    // `startPos` and `positional` are what let an automated EFFECT place its
    // parameter events: a chunk's events are chunk-relative, but which values
    // they carry depends on where the chunk IS.
    void  runChunked_nolock( const sample_t *const *in, sample_t **out, length_t len,
                             offset_t startPos, bool positional );

    // Fill autoEvents_ with this chunk's ParamValue events, times relative to
    // the chunk: the value AT the chunk start ("chase" — pages render out of
    // order, so a plugin can never be assumed to already hold it), then one
    // event per breakpoint inside the chunk, then a kAutoRampFrames grid over
    // continuous stretches. Redundant repeats are dropped, which is what makes
    // a STEP lane cost two events per chunk instead of sixty-four.
    void  buildAutomationChunk_nolock( offset_t chunkStart, length_t n );

    // --- the generator half (proposal 37 P3b) -------------------------------
    // All of these require mutex_.

    // Render `len` frames of the generator into `out` and SUM `in` onto it.
    // `block` carries the events for the whole span, times relative to
    // `startPos`; `injectChase` turns the block's chase set into events at
    // offset 0 of the first chunk (true only for a pre-roll — a page render
    // never re-issues the chase, its state having just been rebuilt).
    void  runGenerator_nolock( const sample_t *const *in, sample_t **out,
                               length_t len, offset_t startPos,
                               const twEventBlock &block, bool injectChase,
                               int sampleRate );
    // reset() + chase(P-K) + K discarded frames (D4). No-op when K works out
    // to zero (a page at position 0 has nothing behind it to rebuild).
    void  preRoll_nolock( offset_t pos, int sampleRate );
    // The chase set, as the events that put a plugin into that state: every
    // controller first, then the note-ons, all at offset 0.
    void  chaseToEvents_nolock( const twEventState &chase,
                                std::vector<twEvent> &out ) const;
    void  ensureGenScratch_nolock( length_t frames );

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

    // --- automation (proposal 37 P5) ---------------------------------------
    std::map<std::uint32_t, std::shared_ptr<const twAutomationCurve> > paramCurves_;
    std::atomic<std::uint64_t> automationEpoch_{ 0 };
    // Per-chunk scratch; members so the render path allocates nothing.
    std::vector<twEvent> autoEvents_;
    twEventOut           autoSink_;

    // Position continuity: the plugin is stateful, so a page that does not
    // start exactly where the last one ended is a discontinuity and resets it.
    offset_t lastEnd_        = 0;
    bool     haveLastEnd_    = false;

    // "Unsupported" is logged once per slot, not once per page.
    bool     loggedUnsupported_ = false;

    // --- the instrument slot (proposal 37 P3b) ------------------------------

    bool     isGenerator_ = false;

    std::shared_ptr<const twEventSource> events_;
    twTempoMap                           tempo_;
    bool                                 tempoValid_ = false;

    // "An instrument is freeze-path only" is logged once per slot: the legacy
    // pull is positionless, so it cannot place an event and has to answer
    // silence (design 4.3). SMARAGD_REVAL_WORKERS=0 therefore makes an
    // instrument track silent BY DESIGN.
    bool     loggedNoPull_ = false;

    // Per-call scratch. Members, not locals: the render path allocates nothing
    // once it is warm (CONTRACT invariant 2).
    twEventBlock          pageBlock_, preBlock_, probeBlock_;
    std::vector<twEvent>  chunkEvents_;
    std::vector<uint8_t>  chunkArena_;
    std::vector<twEvent>  chaseEvents_;
    // The plugin's OWN output when it does not land straight in the page:
    // MonoSpread's single voice, GenFold's pair, WideGen's surplus channels,
    // and the whole of a bypassed or pre-rolling call. One chunk per channel.
    std::vector<std::vector<sample_t>> genOut_;
    std::vector<sample_t *>            genPtrs_;
    std::vector<float *const *>        busPtrs_;
    // Storage for the plugin -> host event sink. Nothing consumes it in P3b
    // (an arpeggiator feeding the next slot is 5.4 / P9); it exists so a
    // plugin that pushes has somewhere to push and the drop count is real.
    std::vector<twEvent>  outEvents_;
    std::vector<uint8_t>  outArena_;
};

}  // namespace audio

#endif
