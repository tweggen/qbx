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

// One BUS TAP of a plugin slot: strictly 1 in / 1 out (proposal 08 M3).
//
// Why not "the component that wraps a plugin": the frozen-page model is one
// MONO page per component (twComponent::freezePage_nolock renders idx = 0
// only), so N parallel mono wires must be N parallel component instances — a
// component that wrote interleaved stereo into one page produced garbage the
// engine then read as mono. A stereo-linked plugin, though, has to see all its
// channels in ONE process() call. So the slot is split: N taps like this one,
// all sharing one twPluginSlotProcessor which owns the plugin instance(s), the
// chunking and a small all-bus page cache. The first tap to ask for a page
// renders every bus; the others are served from that cache.
class twPluginInsert : public twComponent {
public:
    // Kept as the host's declared block size for source compatibility; the
    // authority is twPluginSlotProcessor::kChunkFrames.
    static constexpr length_t kChunkFrames = twPluginSlotProcessor::kChunkFrames;

    // The tap form: share a processor, own one bus.
    twPluginInsert( tw303aEnvironment &env,
                    std::shared_ptr<twPluginSlotProcessor> processor,
                    idx_t busIndex );

    // Convenience/standalone form (tests, and any caller that just wants one
    // plugin on one wire): wraps the plugin in a private single-bus processor.
    // The bus count is the plugin's own input count, so a 2->2 plugin still
    // gets a coherent stereo pair — the tap then serves bus 0.
    twPluginInsert( tw303aEnvironment &env, std::unique_ptr<twPlugin> plugin );

    ~twPluginInsert();

    // twComponent interface. ALWAYS 1/1: the tap is one mono wire.
    idx_t getNInputs() const override  { return 1; }
    idx_t getNOutputs() const override { return 1; }

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
    // so the tap inherits the page cache, the epoch stamping, the RT-thread
    // guard, the stale-predecessor fallback and the readiness gate. The actual
    // render lands in renderFrames().
    std::shared_ptr<twOutputPage> freezePage(
        offset_t startPos,
        const sample_t *inputData,
        uint64_t inputOffset,
        length_t inputLength,
        int sampleRate,
        std::shared_ptr<twOutputPage> previousPage = nullptr
    ) override;

    length_t renderFrames( sample_t *output, length_t length,
                           const sample_t *input, length_t inputLength,
                           idx_t idx ) override;

    // Pull this bus's upstream page for [startPos, startPos+len) into dst.
    //
    // HARD INVARIANT (CONTRACT 13): this must NOT hold this tap's own component
    // mutex while it calls into the producer. It is invoked BY THE PROCESSOR,
    // with the processor mutex held, on behalf of whichever sibling tap asked
    // for the page first; a tap that held its own mutex here (or across
    // pageFor()) would deadlock bus 0's gather against bus 1's own freeze. The
    // producer shared_ptr is therefore snapshotted under a brief lock, the lock
    // released, and only then is requestPage() called.
    length_t pullUpstreamPage( offset_t startPos, length_t len, int sampleRate,
                               sample_t *dst );

    // The legacy streaming-pull equivalent (twComponent::calcOutputTo path):
    // read len frames from this bus's input plug. Same locking rule.
    length_t pullInputStreaming( sample_t *dst, length_t len );

    void resetBlock();

    // Plugin control. Both forward to the shared processor, so an edit reaches
    // every bus of the slot at once.
    void setBypass( bool bypass );
    bool getBypass() const;
    twPlugin *getPlugin() const;

    const std::shared_ptr<twPluginSlotProcessor> &getProcessor() const { return proc_; }
    idx_t getBusIndex() const { return busIndex_; }

    // The tap carries no plugin state of its own: the processor owns it and
    // resets it on a position discontinuity. Deliberately NOT plugin_->reset():
    // twComponent::freezePage_nolock calls reset() on every discontinuity for
    // EVERY tap, so resetting the shared plugin here would wipe the state the
    // sibling tap's render just produced.
    virtual void reset() override;

    virtual void teardown() override;

private:
    std::shared_ptr<twPluginSlotProcessor> proc_;
    idx_t                                  busIndex_ = 0;

    // The page position renderFrames() must render. Its signature does not
    // carry one, but freezePage_nolock() calls seekTo(page->startPosition)
    // immediately before renderFrames(), INSIDE the component's cursorMutex_ —
    // which is what makes capturing it there safe against a concurrent freeze.
    // Atomic because seekTo() is also reachable from the chain's own seek.
    std::atomic<offset_t> renderPos_{ 0 };

    // This reader's page-continuity hint, exactly like twStreamingLatch's
    // per-reader previousPage_: only the immediately preceding page may chain
    // DSP state. Touched solely from pullUpstreamPage(), i.e. always under the
    // PROCESSOR's mutex.
    std::shared_ptr<twOutputPage> upstreamHint_;
};

}  // namespace audio

#endif
