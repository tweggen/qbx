#ifndef _TWPLUGINCHAIN_H_
#define _TWPLUGINCHAIN_H_

#include "tw/graph/twcomponent.h"
#include <atomic>
#include <memory>
#include <vector>
#include <mutex>

namespace audio {
class twPluginInsert;
}

// Internal DSP component that threads audio through a sequence of plugin
// inserts, wired in series: insert0 output -> insert1 input, etc.
//
// ONE CHAIN PER TRACK, N CHANNELS WIDE (proposal 36 B4). It used to be one
// chain per BUS, each 1 port wide, because a page was mono and N channels were
// N parallel component instances; the chain's `nBusses_` was that port count
// and its rebuildWiring() looped over it. Now the channel dimension lives in
// the page, so a chain is a single wire: one input port, one output port, N
// channels, and one insert per slot instead of one per (slot, bus).
class twPluginChain : public twComponent {
public:
    twPluginChain( tw303aEnvironment &env, idx_t channels );
    ~twPluginChain();

    // twComponent interface. ONE PORT each way — see the class note, and §7
    // trap 8: the port count and the channel count must never be merged.
    idx_t getNInputs() const override { return 1; }
    idx_t getNOutputs() const override { return 1; }

    // §4.2. A chain has no page cache of its own — it FORWARDS its last
    // insert's page, or its producer's when it has no inserts — so this is a
    // promise about what those forwarded pages will be, and the silence pages
    // it builds itself have to honour it.
    idx_t getOutputChannels() const override
    {
        return (idx_t) channels_.load( std::memory_order_acquire );
    }

    // Runtime width change. Forwards to nothing: the inserts learn their width
    // from their processors (SPluginSlot::setChannelCount) and the trackmix
    // from its own setChannels(). What this does is stale the pages of the old
    // width, ours and every insert's.
    void setChannels( idx_t n );

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

    std::atomic<int> channels_;
    mutable std::mutex pluginsMutex_;  // protects plugins_ vector from concurrent access
    std::vector<std::shared_ptr<twComponent> > plugins_;  // not owned; managed by SPluginSlot
};

#endif
