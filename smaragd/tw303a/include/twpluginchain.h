#ifndef _TWPLUGINCHAIN_H_
#define _TWPLUGINCHAIN_H_

#include "twcomponent.h"
#include <vector>
#include <mutex>

namespace audio {
class twPluginInsert;
}

// Internal DSP component that threads audio through a sequence of plugin inserts.
// Each insert is wired in series: plugin0 output → plugin1 input, etc.
// Channels are preserved (each input channel threads independently).
class twPluginChain : public twComponent {
public:
    twPluginChain( tw303aEnvironment &env, idx_t nBusses );
    ~twPluginChain();

    // twComponent interface
    idx_t getNInputs() const override { return nBusses_; }
    idx_t getNOutputs() const override { return nBusses_; }

    // Phase 3: IOVector-based interface (type-safe, page-backed)
    length_t calcOutputTo( IOVector& dest, idx_t port ) override;
    void createOutputLatches() override;
    const char *getInputName( idx_t ) const override { return nullptr; }
    const char *getOutputName( idx_t ) const override { return nullptr; }
    int seekTo( offset_t offset ) override;

    // Add a plugin insert to the chain
    void addPlugin( audio::twPluginInsert *insert );

    // Remove a plugin insert by index
    void removePlugin( int index );

    // Reorder plugins in the chain
    void reorderPlugin( int fromIndex, int toIndex );

    // Rebuild the wiring (call after modifications)
    void rebuildWiring();

    virtual void reset() override;
private:
    idx_t nBusses_;
    mutable std::mutex pluginsMutex_;  // protects plugins_ vector from concurrent access
    std::vector<audio::twPluginInsert *> plugins_;  // not owned; managed by SPluginSlot
};

#endif
