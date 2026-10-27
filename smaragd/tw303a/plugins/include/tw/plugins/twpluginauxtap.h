#ifndef _TWPLUGINAUXTAP_H_
#define _TWPLUGINAUXTAP_H_

#include "tw/graph/twcomponent.h"

#include <atomic>
#include <cstdint>
#include <string>

namespace audio {

// ONE AUX OUTPUT BUS OF ONE PLUGIN SLOT, as a component (proposal 55 D2, M1).
//
// A plugin renders all its buses in one process() call, so the aux buses of a
// routed slot are rendered by the slot's twPluginInsert and cached on the
// insert's own page as a SIDE page (twOutputPage::aux). This component reads
// one bus out of it. It has one input plug, wired to the insert's output, and
// one output of the width it was built for (a return track's: the project's).
//
// WHY THIS SHAPE, and not the per-bus tap of proposal 37 §5.4. Before proposal
// 36 B4 a slot was one processor plus N per-bus taps sharing a private page
// cache, and the first tap to ask rendered every bus by reaching SIDEWAYS
// through its siblings -- the gather that deadlocked (plugins CONTRACT inv.
// 13). This tap reaches the insert through an ORDINARY input plug, so the
// scheduler plans it like any dependency, the lock order stays downstream ->
// upstream, and there is no second cache: it caches its own mapped pages like
// every component, and those are stamped against the insert's.
//
// THE MAPPING IS DECIDED ONCE, here, never per page (proposal 55 D4, plugins
// inv. 16's discipline):
//
//   bus width A == output width C    channel for channel, a byte copy
//   A == 1, C > 1                    the one channel on every channel
//   A == 2, C == 1                   the mean of the pair
//   anything else                    REFUSED: silent, logged once, described
//
// A side page that does not carry this bus at the width this tap was built
// for -- a render in flight across a mask change (T10), a bus no longer
// routed, an insert whose slot cannot route -- renders SILENCE and is
// counted. It never reads past the side page.
class twPluginAuxTap : public twComponent {
public:
    enum class Mapping { Copy, Spread, Fold, Refused };

    twPluginAuxTap( tw303aEnvironment &env, idx_t outChannels,
                    std::uint16_t bus, std::uint16_t busChannels );

    idx_t getNInputs() const override  { return 1; }
    idx_t getNOutputs() const override { return 1; }
    idx_t getOutputChannels() const override { return outChannels_; }
    const char *getInputName( idx_t ) const override  { return nullptr; }
    const char *getOutputName( idx_t ) const override { return nullptr; }

    void createOutputLatches() override;
    void reset() override {}
    int  seekTo( offset_t offset ) override;

    length_t renderPageWide( twOutputPage &page, length_t frames,
                             const sample_t *input, length_t inputLength ) override;
    length_t renderPageNarrow( twOutputPage &page, length_t frames,
                               const sample_t *input, length_t inputLength ) override;
    // The narrow degradation (proposal 36 §7 trap 18): a component overriding
    // neither renderFrames nor calcOutputTo recurses until the stack ends.
    length_t renderFrames( sample_t *output, length_t length,
                           const sample_t *input, length_t inputLength,
                           idx_t idx ) override;

    std::uint16_t bus()         const { return bus_; }
    std::uint16_t busChannels() const { return busChannels_; }
    Mapping       mapping()     const { return mapping_; }
    static const char *mappingName( Mapping m );

    /// "bus=1|busChannels=2|width=2|mapping=copy" -- what a testkit verb and
    /// a describe surface report.
    std::string describe() const;

    /// Renders answered with silence because the side page did not carry
    /// this bus at this width (T10), since construction.
    std::uint64_t silentRenders() const
    {
        return silentRenders_.load( std::memory_order_relaxed );
    }

private:
    length_t renderCore_( sample_t **outCh, idx_t nCh, offset_t startPos,
                          length_t frames );

    const idx_t         outChannels_;
    const std::uint16_t bus_;
    const std::uint16_t busChannels_;
    const Mapping       mapping_;

    std::atomic<offset_t>      renderPos_{ 0 };
    std::atomic<std::uint64_t> silentRenders_{ 0 };
};

}  // namespace audio

#endif
