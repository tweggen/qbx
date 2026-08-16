#ifndef _TWPLUGININSERT_H_
#define _TWPLUGININSERT_H_

#include "tw/graph/twcomponent.h"
#include "tw/plugins/twplugin.h"
#include "tw/plugins/twpluginslotproc.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace audio {

class twPlugin;
class twPluginSlotProcessor;

// ONE plugin slot in the audio path: a WIDE component, 1 port in / 1 port out,
// N CHANNELS (proposal 36 B4).
//
// WHAT THIS REPLACED, and why the old shape existed. Until proposal 36 the
// frozen page was one MONO page per component, so N channels had to be N
// parallel component instances — and a stereo-linked plugin, which must see all
// its channels in ONE process() call, could not be expressed by any single one
// of them. ("A component that wrote interleaved stereo into one page produced
// garbage the engine then read as mono.") The slot was therefore split into N
// per-bus TAPS around one out-of-band twPluginSlotProcessor with a private
// all-bus page cache: the first tap to ask rendered every bus by reaching
// SIDEWAYS through its siblings, and the others hit the cache. That sideways
// gather is what plugins/CONTRACT.md invariant 13's deadlock rule was about.
//
// B4 made the page N channels wide, so all of that is gone: one insert, one
// upstream page, one process() call per chunk over every channel, one output
// page. No sibling gather, so no lock-order hazard between taps; no all-bus
// cache, because the component page cache above is now the only cache there is.
// The processor stays as the plugin LIFETIME and STATE holder — see its header.
class twPluginInsert : public twComponent {
public:
    // Kept as the host's declared block size for source compatibility; the
    // authority is twPluginSlotProcessor::kChunkFrames.
    static constexpr length_t kChunkFrames = twPluginSlotProcessor::kChunkFrames;

    // The slot form: share the processor that owns the plugin instance(s).
    twPluginInsert( tw303aEnvironment &env,
                    std::shared_ptr<twPluginSlotProcessor> processor );

    // Convenience/standalone form (tests, and any caller that just wants one
    // plugin on one wire): wraps the plugin in a private processor whose
    // channel count is the plugin's own input count, so a 2->2 plugin gets a
    // coherent stereo pair.
    twPluginInsert( tw303aEnvironment &env, std::unique_ptr<twPlugin> plugin );

    ~twPluginInsert();

    // twComponent interface. ONE PORT each way — the channel dimension lives in
    // the page, not in the patch bay (§4.2 / §7 trap 8: getNOutputs() is the
    // port count and must never be merged with getOutputChannels()).
    idx_t getNInputs() const override  { return 1; }
    idx_t getNOutputs() const override { return 1; }

    // §4.2. The width the slot was configured for. This is what makes
    // twComponent::freezePage allocate a page of that width and freezePage_nolock
    // take the wide fork.
    idx_t getOutputChannels() const override;

    length_t calcOutputTo( IOVector& dest, idx_t port ) override;
    void init() override;
    void createOutputLatches() override;
    const char *getInputName( idx_t ) const override { return nullptr; }
    const char *getOutputName( idx_t ) const override { return nullptr; }
    int seekTo( offset_t offset ) override;

    // The authoritative freeze. Only two things happen here that the base class
    // cannot do: the PREVIEW bypass (CONTRACT invariant 6 — a freeze whose
    // sampleRate differs from env.getSRate() is a waveform-envelope render and
    // must not touch the plugin), and then delegation to twComponent::freezePage
    // so the insert inherits the page cache, the epoch stamping, the RT-thread
    // guard, the stale-predecessor fallback and the readiness gate. The actual
    // render lands in renderPageWide() / renderFrames().
    std::shared_ptr<twOutputPage> freezePage(
        offset_t startPos,
        const sample_t *inputData,
        uint64_t inputOffset,
        length_t inputLength,
        int sampleRate,
        std::shared_ptr<twOutputPage> previousPage = nullptr
    ) override;

    // §4.3: fill every channel in one pass, from one upstream page, with one
    // process() sweep. This is the authoritative render at width > 1.
    length_t renderPageWide( twOutputPage &page, length_t frames,
                             const sample_t *input, length_t inputLength ) override;

    // THE NARROW DEGRADATION (§7 trap 18). The base renderFrames() calls
    // calcOutputTo() and the base calcOutputTo() calls renderFrames(), so a
    // component that overrides NEITHER recurses until the stack ends — and a
    // wide component is handed a width-1 page by the mono-scratch and preview
    // paths, which no width fork can widen. At a project width of 1 this IS the
    // authoritative path, and it is the same core as renderPageWide with one
    // channel.
    length_t renderFrames( sample_t *output, length_t length,
                           const sample_t *input, length_t inputLength,
                           idx_t idx ) override;

    // The legacy streaming-pull equivalent: read len frames from the input plug.
    // The plug is snapshotted under a brief lock and the lock RELEASED before
    // the pull — a component's own mutex must never be held across a call into
    // a producer.
    length_t pullInputStreaming( sample_t *dst, length_t len );

    // Plugin control. Both forward to the shared processor.
    void setBypass( bool bypass );
    bool getBypass() const;
    twPlugin *getPlugin() const;

    const std::shared_ptr<twPluginSlotProcessor> &getProcessor() const { return proc_; }

    // The insert carries no plugin state of its own: the processor owns it and
    // resets it on a position discontinuity. Deliberately NOT plugin_->reset():
    // twComponent::freezePage_nolock calls reset() on every discontinuity, and
    // the processor's own lastEnd_ bookkeeping is what decides whether the DSP
    // may continue.
    virtual void reset() override;

    virtual void teardown() override;

private:
    // The shared render core, used by both renderPageWide (nCh = page width)
    // and renderFrames (nCh = 1). `outCh` are nCh writable planar buffers of
    // `frames` frames.
    length_t renderCore_( sample_t **outCh, idx_t nCh, offset_t startPos,
                          length_t frames, bool positional );

    std::shared_ptr<twPluginSlotProcessor> proc_;

    // The page position renderPageWide()/renderFrames() must render. Their
    // signatures do not carry one for the narrow case, but freezePage_nolock()
    // calls seekTo(page->startPosition) immediately before, INSIDE the
    // component's cursorMutex_ — which is what makes capturing it there safe
    // against a concurrent freeze. Atomic because seekTo() is also reachable
    // from the chain's own seek.
    std::atomic<offset_t> renderPos_{ 0 };

    // The per-channel input gather, zero-padded past the upstream page's valid
    // frames. Sized on demand; touched only from the render, which
    // freezePage_nolock has already serialized on cursorMutex_ (this component
    // has a streaming input, so usesSerialCursor()'s sibling rule applies).
    std::vector<sample_t>         gather_;
    std::size_t                   gatherStride_ = 0;
    std::vector<const sample_t *> inPtrs_;
};

}  // namespace audio

#endif
