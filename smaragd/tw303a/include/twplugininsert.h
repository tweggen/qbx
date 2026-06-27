#ifndef _TWPLUGININSERT_H_
#define _TWPLUGININSERT_H_

#include "twcomponent.h"
#include "plugins/twplugin.h"
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
    length_t calcOutputTo( sample_t *dst, length_t len, idx_t port ) override;
    void createOutputLatches() override;
    const char *getInputName( idx_t ) const override { return nullptr; }
    const char *getOutputName( idx_t ) const override { return nullptr; }
    int seekTo( offset_t offset ) override;

    void resetBlock() { producedThisBlock_ = false; }

    // Plugin control.
    void setBypass( bool bypass ) { bypass_ = bypass; }
    bool getBypass() const { return bypass_; }
    twPlugin *getPlugin() const { return plugin_.get(); }

    virtual void reset() override;
private:
    std::unique_ptr<twPlugin> plugin_;
    bool bypass_ = false;
    bool producedThisBlock_ = false;

    // De-interleaved scratch buffers for processing.
    std::vector<std::vector<sample_t>> inScratch_, outScratch_;

    // Copy channels in->out with matching channel count.
    void copyChannels( const std::vector<std::vector<sample_t>> &in,
                       std::vector<std::vector<sample_t>> &out,
                       idx_t nChan, length_t len );

    // Pull one input channel into dst buffer.
    length_t pullInput( idx_t port, sample_t *dst, length_t len );
};

}  // namespace audio

#endif
