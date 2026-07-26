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

// How the slot maps N parallel mono track buses onto the plugin's own channel
// count. This is the channel-mismatch table of proposal 08 §Layer 3, and it is
// derived ONCE per (plugin I/O, bus count) pair — never per page.
enum class twPluginSlotMode {
    Transparent,    // no plugin, or Unsupported: input copied to output per bus
    Direct,         // nIn == nOut == nBuses: one instance, all buses coherently
    DualMono,       // a 1->1 plugin on N buses: N independent instances
    MonoFold        // a 2->2 plugin on ONE bus: feed both inputs, average outputs
};

// The shared processing core of one plugin slot: it owns the twPlugin
// instance(s), the bypass flag, the prepare() state, the block chunking and a
// small (startPos, len, stamp) -> all-bus output cache.
//
// It is deliberately NOT a twComponent. The frozen-page model is one MONO page
// per component (twComponent::freezePage_nolock renders idx = 0 only), so N
// parallel mono wires are N parallel component instances — the per-bus
// twPluginInsert "taps". A stereo-linked plugin, however, must see all its
// channels in ONE process() call, which no single mono component can express.
// Hence the split: N taps, one processor. The first tap to ask for a page
// renders every bus; the others are served from the cache.
//
// Threading: every public method is safe from any non-realtime thread.
// setBusCount() (and therefore the initial construction path) calls
// twPlugin::prepare(), which for CLAP is activate() — annotated [main-thread] —
// so it must be reached from the UI thread. Nothing here may be called from the
// realtime audio callback; the RT path reads ready pages (twRtThreadGuard).
class twPluginSlotProcessor {
public:
    // The block size the host promises never to exceed. Pages are
    // twOutputPage::FRAME_CAPACITY (65536) frames, which no real plugin accepts
    // in one call, so prepare() declares this and process() never sees more.
    static constexpr length_t kChunkFrames = 4096;

    // How many (startPos, len, stamp) results are kept. Two is enough for the
    // case this exists for — every tap of the slot asking for the SAME page —
    // plus one slot of slack when two demands for different pages interleave.
    static constexpr std::size_t kCacheEntries = 2;

    // Produces a FRESH plugin instance per call. A factory rather than one
    // instance because the dual-mono mapping needs N of them, and the mapping
    // is only known once the bus count is.
    using Factory = std::function<std::unique_ptr<twPlugin>()>;

    // One rendered page, all buses, de-interleaved (one mono vector per bus).
    struct Output {
        offset_t      startPos = 0;
        length_t      frames   = 0;
        std::uint64_t stamp    = 0;
        std::vector<std::vector<sample_t>> bus;
    };

    twPluginSlotProcessor( tw303aEnvironment &env, Factory factory,
                           const twPluginIoLayout &declaredIo );
    ~twPluginSlotProcessor();

    twPluginSlotProcessor( const twPluginSlotProcessor & )            = delete;
    twPluginSlotProcessor &operator=( const twPluginSlotProcessor & ) = delete;

    // --- configuration (UI thread) -----------------------------------------

    // Declare how many parallel mono wires this slot serves. Re-derives the
    // channel-mismatch policy, re-instantiates, and prepare()s. Idempotent.
    void setBusCount( idx_t nBuses );
    idx_t busCount() const;

    // Replace the instantiation factory and re-derive everything (proposal 08
    // M4). This is how a slot whose plugin was MISSING becomes Active after a
    // rescan found it: the taps and the twPluginChains they sit in are left
    // completely alone, because they only ever reference this processor. The
    // caller re-applies its stored state chunk afterwards.
    void setFactory( Factory factory );

    // Register the per-bus tap. Held WEAKLY: taps own the processor, so a
    // strong ref here would be a cycle. The taps are also how the processor
    // gathers upstream audio (see pageFor()).
    void attachTap( idx_t bus, const std::shared_ptr<twPluginInsert> &tap );

    twPluginSlotMode  mode()  const;
    twPluginSlotState state() const;

    void setBypass( bool bypass );
    bool bypass() const { return bypass_.load( std::memory_order_acquire ); }

    // Host-side access for parameters and state chunks. plugins() returns every
    // instance, which is what the dual-mono mapping needs (a parameter edit has
    // to reach all N). Both return the live pointers; the caller must not
    // outlive the processor.
    twPlugin               *plugin()  const;   // bus 0's instance, or nullptr
    std::vector<twPlugin *> plugins() const;

    // Anything that changes what process() would produce (bypass, a parameter,
    // a state chunk) must land here, or the cached page is served unchanged and
    // the edit is inaudible. It moves the cache key AND stales every tap's
    // frozen pages; the caller still owns the DOWNSTREAM path
    // (SObject::invalidateRenderPath()).
    void bumpParamEpoch();

    // --- the freeze path ---------------------------------------------------

    // Resolve the all-bus output for [startPos, startPos + len). The first tap
    // to ask renders (gathering every bus's upstream page through the sibling
    // taps' pullUpstreamPage()); the rest hit the cache. Never null.
    std::shared_ptr<const Output> pageFor( idx_t bus, offset_t startPos,
                                           length_t len, int sampleRate );

    // --- the legacy pull path (twComponent::calcOutputTo) ------------------

    // Positionless block render for the streaming pull path. The first tap of a
    // block renders every bus through readStreamingData(); resetBlock() opens
    // the next block. No cache: the pull path has no page identity.
    length_t blockFor( idx_t bus, sample_t *dst, length_t len );
    void     resetBlock();

private:
    // All _nolock helpers require mutex_.
    void  bumpParamEpoch_nolock();
    void  rebuild_nolock();
    void  ensureScratch_nolock( length_t len );
    void  resetInstances_nolock();
    void  runChunked_nolock( length_t len );
    bool  sampleRateChangedForPull_nolock();
    std::uint64_t stampNow_nolock() const;
    std::shared_ptr<const Output> findCached_nolock( offset_t startPos,
                                                     length_t len,
                                                     std::uint64_t stamp ) const;
    void  publish_nolock( const std::shared_ptr<const Output> &out );

    tw303aEnvironment &env_;
    Factory            factory_;
    twPluginIoLayout   declaredIo_;

    mutable std::mutex mutex_;

    idx_t              nBuses_ = 0;
    twPluginSlotMode   mode_   = twPluginSlotMode::Transparent;
    twPluginSlotState  state_  = twPluginSlotState::Active;
    int                preparedRate_ = 0;

    std::vector<std::unique_ptr<twPlugin>>      instances_;
    std::vector<std::weak_ptr<twPluginInsert>>  taps_;

    // Per-bus de-interleaved gather/result buffers, and the two extra output
    // buffers the MonoFold mapping folds down from.
    std::vector<std::vector<sample_t>> busIn_, busOut_, foldOut_;

    // Pointer arrays handed to twPlugin::process(). Members, not locals, so the
    // per-chunk loop allocates nothing (CONTRACT invariant 2).
    std::vector<const sample_t *> inPtrs_;
    std::vector<sample_t *>       outPtrs_;

    std::atomic<bool>          bypass_{ false };
    std::atomic<std::uint64_t> paramEpoch_{ 1 };

    // Position continuity: the plugin is stateful, so a page that does not
    // start exactly where the last one ended is a discontinuity and resets it.
    offset_t lastEnd_        = 0;
    bool     haveLastEnd_    = false;

    // Legacy pull path only.
    bool     blockProduced_  = false;

    std::vector<std::shared_ptr<const Output>> cache_;
    std::size_t                               cacheNext_ = 0;

    // "Unsupported" is logged once per slot, not once per page.
    bool     loggedUnsupported_ = false;
};

}  // namespace audio

#endif
