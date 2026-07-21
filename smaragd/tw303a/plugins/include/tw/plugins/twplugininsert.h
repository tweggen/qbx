#ifndef _TWPLUGININSERT_H_
#define _TWPLUGININSERT_H_

#include "tw/graph/twcomponent.h"
#include "tw/plugins/twplugin.h"
#include <memory>
#include <vector>
#include <mutex>

namespace audio {

class twPlugin;

// Component wrapper around a twPlugin. Integrates a plugin into the DSP graph.
// Each input/output port corresponds to one channel (de-interleaved).
// The component processes all channels coherently on first port pull per block,
// then serves results from cache.
class twPluginInsert : public twComponent {
public:
    twPluginInsert( tw303aEnvironment &env, std::unique_ptr<twPlugin> plugin );
    ~twPluginInsert();

    // twComponent interface.
    idx_t getNInputs() const override;
    idx_t getNOutputs() const override;

    // Phase 3: IOVector-based interface (type-safe, page-backed)
    length_t calcOutputTo( IOVector& dest, idx_t port ) override;
    void createOutputLatches() override;
    const char *getInputName( idx_t ) const override { return nullptr; }
    const char *getOutputName( idx_t ) const override { return nullptr; }
    int seekTo( offset_t offset ) override;

    // Phase 6c: Freeze-page interface for non-blocking plugin processing
    // Assumes pages arrive sequentially; detects and warns on non-sequential pages (seek case)
    std::shared_ptr<twOutputPage> freezePage(
        offset_t startPos,
        const sample_t *inputData,
        uint64_t inputOffset,
        length_t inputLength,
        int sampleRate,
        std::shared_ptr<twOutputPage> previousPage = nullptr
    ) override;

    void resetBlock() { producedThisBlock_ = false; }

    // Plugin control.
    // Thread-safe: acquires lock to prevent race with calcOutputTo()
    void setBypass( bool bypass );
    bool getBypass() const { return bypass_; }
    twPlugin *getPlugin() const { return plugin_.get(); }

    virtual void reset() override;

    // Teardown protocol
    virtual void teardown() override;

private:
    // Private _nolock() helper (caller must hold mutex())
    // CRITICAL: Used to protect bypass changes from calcOutputTo()
    void setBypass_nolock( bool bypass );
    void reset_nolock();

    std::unique_ptr<twPlugin> plugin_;
    bool bypass_ = false;
    bool producedThisBlock_ = false;

    // Phase 6c: Track frozen page position for sequential page detection
    // If pages arrive non-sequentially (seek case), reset plugin to avoid state corruption
    uint64_t lastFrozenPos_ = 0;
    size_t pageSize_ = 0;  // Cached page size for sequential check

    // De-interleaved scratch buffers for processing.
    std::vector<std::vector<sample_t>> inScratch_, outScratch_;

    // Grow scratch buffers to at least len frames (caller must hold mutex()).
    // freezePage renders full pages (65536 frames), far above the 4096 default.
    void ensureScratchCapacity( length_t len );

    // Copy channels in->out with matching channel count.
    void copyChannels( const std::vector<std::vector<sample_t>> &in,
                       std::vector<std::vector<sample_t>> &out,
                       idx_t nChan, length_t len );

    // Pull one input channel into dst buffer.
    length_t pullInput( idx_t port, sample_t *dst, length_t len );
};

}  // namespace audio

#endif
