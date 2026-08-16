
#ifndef _TWREWIRE_H
#define _TWREWIRE_H

#include "tw/graph/twcomponent.h"

#include <atomic>
#include <mutex>
#include <vector>

/**
 * Patch-bay component with N paired inputs and outputs (output[i] = input[i]).
 *
 * Owns one twStreamingLatch per output index. calcOutputTo() pulls from the
 * matching input plug, or fills with silence if no input is wired. This
 * lets downstream consumers (e.g. the speaker) wire to a stable latch once
 * and have the rewire transparently swap its source over time, instead of
 * holding a snapshot pointer to whichever component happened to be wired
 * in at link time.
 *
 * CHANNEL MAPPING (proposal 36 B4). This is the component `SStdMixer::
 * getRootComponent()` has carried a "FIXME: Generate a channel reassignment."
 * over since the beginning, and B4 is where it becomes one. A rewire of width
 * N reads the PAGE of input plug 0 (§4.4 rule 2) and publishes output channel
 * c from that page's channel `map[c]`, clamped by §4.4. The default map is the
 * identity, so a wide rewire is a pass-through until somebody asks for
 * something else.
 *
 * A WIDE REWIRE IS SINGLE-PLUG, and that is the whole point of B4's collapse:
 * a track used to be N parallel mono wires ending in N rewire plugs; it is now
 * ONE wire N channels wide, so there is one plug and the channel dimension
 * lives inside the page. The N-plug/N-latch patch bay is unchanged at width 1,
 * which is what keeps a mono project byte-exact — and the plugs beyond 0 are
 * simply not read when the width is > 1 (setNPlugs() refuses to leave them
 * wired, so this cannot silently drop audio).
 */
class twRewire
    : public twComponent
{
public:
    twRewire( tw303aEnvironment &env );
    virtual ~twRewire();

    // --- Page width and channel mapping (proposal 36 §4.2 / B4) -----------
    void setChannels( idx_t n );
    virtual idx_t getOutputChannels() const override
    {
        return (idx_t) channels_.load( std::memory_order_acquire );
    }

    /// Output channel c is input channel map[c]. An empty map (the default) is
    /// the identity. Entries are clamped against the page in hand at read time,
    /// so a map naming a channel the producer does not have degrades to that
    /// producer's last channel rather than reading out of bounds.
    void setChannelMap( const std::vector<idx_t> &map );
    std::vector<idx_t> channelMap() const;

    virtual length_t renderPageWide( twOutputPage &page, length_t frames,
                                     const sample_t *input,
                                     length_t inputLength ) override;

    virtual twLatchOutput *linkOutput( idx_t idx ) override;
    virtual void allocPlugs() override;
    virtual void init() override;
    virtual void createOutputLatches() override;

    virtual int setNPlugs( idx_t );

    virtual idx_t getNInputs() const override;
    virtual idx_t getNOutputs() const override;
    virtual const char *getInputName( idx_t ) const override;
    virtual const char *getOutputName( idx_t ) const override;

    virtual int seekTo( offset_t offset ) override;

    virtual void setBufferSize( length_t ) override {};

protected:
    // Phase 3: IOVector-based interface (type-safe, page-backed)
    virtual length_t calcOutputTo( IOVector& dest, idx_t idx ) override;
    virtual void reset() override;

    // Teardown protocol
    virtual void teardown() override;

private:
    // Helpers: do work outside lock (caller must hold mutex)
    int setNPlugs_nolock(idx_t n);
    int seekTo_nolock(offset_t offset);
    twLatchOutput *linkOutput_nolock(idx_t idx);

    int nInputs_;
    std::atomic<int>   channels_{ 1 };   // see setChannels()
    mutable std::mutex mapMutex_;        // guards channelMap_ only
    std::vector<idx_t> channelMap_;      // empty = identity
};

#endif
