#ifndef _TWPLUGINCHAIN_H_
#define _TWPLUGINCHAIN_H_

#include "tw/graph/twcomponent.h"
#include <memory>
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

    // Phase 6c: Freeze-page interface for non-blocking plugin chain processing
    // Threads frozen pages sequentially through plugin chain
    std::shared_ptr<twOutputPage> freezePage(
        offset_t startPos,
        const sample_t *inputData,
        uint64_t inputOffset,
        length_t inputLength,
        int sampleRate,
        std::shared_ptr<twOutputPage> previousPage = nullptr
    ) override;

    // Add a plugin insert to the chain
    void addPlugin( std::shared_ptr<audio::twPluginInsert> insert );

    // Remove a plugin insert by index. Correct ONLY while the caller's index
    // means the same thing as a position in plugins_ — prefer the identity
    // overload below, which cannot target the wrong insert.
    void removePlugin( int index );

    // Remove a plugin insert by identity. This is what the model layer uses:
    // a model slot index and a plugins_ position are two different numbers the
    // moment anything reorders, and erasing by the wrong one silently drops a
    // DIFFERENT plugin from the audio path while the model drops the right one.
    // A no-op if the insert is not in this chain.
    void removePlugin( const std::shared_ptr<audio::twPluginInsert> &insert );

    // Reorder plugins in the chain
    void reorderPlugin( int fromIndex, int toIndex );

    // Rebuild the wiring (call after modifications)
    void rebuildWiring();

    // Snapshot the ordered insert list. Public so callers can inspect the chain
    // without holding pluginsMutex_ across their own work.
    std::vector<std::shared_ptr<twComponent> > snapshotPlugins() const;

    // Scoped invalidation (proposal 15): a chain has no page cache of its own,
    // but its inserts' pages bake in upstream audio — forward the bump to them.
    void bumpContentEpoch() override;
    void invalidatePagesInRange(offset_t start, offset_t end) override;

    virtual void reset() override;

    // Teardown protocol
    virtual void teardown() override;
    virtual void onDependencyTeardown(std::shared_ptr<twComponent> dep) override;
private:
    // Caller must hold pluginsMutex_.
    void rebuildWiring_nolock();

    idx_t nBusses_;
    mutable std::mutex pluginsMutex_;  // protects plugins_ vector from concurrent access
    std::vector<std::shared_ptr<twComponent> > plugins_;  // not owned; managed by SPluginSlot
};

#endif
