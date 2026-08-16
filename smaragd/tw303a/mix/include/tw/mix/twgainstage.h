#ifndef _TWGAINSTAGE_H
#define _TWGAINSTAGE_H

#include "tw/graph/twcomponent.h"

#include <atomic>
#include <cstdint>
#include <mutex>

/**
 * THE TRACK FADER (proposal 37 D5 / §4.5, executed by P3a).
 *
 * One WIDE component per track, wired between the track's plugin chain and its
 * rewire:
 *
 *     twTrackMix(N) -> twPluginChain(N) -> twGainStage(N) -> twRewire(N)
 *
 * WHAT MOVED, AND WHY. Until P3a the fader was a per-page scalar applied INSIDE
 * twTrackMix::freezePage_nolock, i.e. PRE-FX (design F6). That is the wrong
 * order for two reasons the proposal settles once (D5): an instrument's output
 * must be under the fader, and post-insert faders are what every reference DAW
 * does. A linear insert cannot tell the two orders apart — which is exactly why
 * the ordering is gated by a CLIPPER (tw.test.clap.gain's "Clip Threshold",
 * param id 2) in qxa.fader_post_fx rather than by a byte compare.
 *
 * WHY IT IS ITS OWN COMPONENT rather than a multiply bolted onto twRewire: the
 * rewire is the track's ROOT, and the root is the metering tap and the mixer's
 * input. Keeping the fader one hop upstream leaves both of those reading a
 * post-fader signal (which they already did) while the fader itself stays a
 * thing with its own content epoch — and THAT is what retires the "the legacy
 * pull does not observe a gain change" caveat: the rewire's producer is now the
 * gain stage, whose epoch the latch gates its cached page on, so a fader change
 * made after a position was first frozen is picked up on every path.
 *
 * CLASS INFINITY, PURE. The output for a frame depends on that frame's input,
 * the scalar, and the frame's POSITION (the mute ramp) — never on how the
 * component got there. So it carries no DSP state, reset() is a no-op, and
 * range invalidation over it is EXACT.
 *
 * BYTE EXACTNESS. At 0 dB, unmuted, the factor is exactly 1.0 and the render is
 * a pure copy with no multiply at all — the same samples the chain produced,
 * bit for bit. That is what makes proposal 36's committed golden corpus (which
 * has no non-unity fader anywhere) byte-identical across this move by
 * construction rather than by luck.
 *
 * MUTE. The 1-2 ms ramp lives HERE, and it is the AUDIO mute — the `self:Muted`
 * automation lane of P5 is what will drive it. The mute BUTTON
 * (`set-track-mute` / solo) stays STRUCTURAL: the parent nulls the muted
 * child's input plug (twMixer) or skips its clip entry (twTrackMix), because
 * mute is a property of the summing CHANNEL, not of a track's own output — a
 * capture of a muted track must still contain its material. P3a implements the
 * ramp and leaves it unwired; nothing in the app calls setMuted() yet.
 */
class twGainStage
    : public twComponent
{
public:
    explicit twGainStage( tw303aEnvironment &env );
    virtual ~twGainStage();

    // --- the fader -------------------------------------------------------

    /// Volume in dB, in the app's fader space (app/timeline/sfadercurve.h).
    /// 0 dB is unity and renders as a pure copy. Bumps the content epoch: the
    /// scalar is baked into every page this component has already published.
    void setGainDb( double gainDb );
    double gainDb() const;

    // --- mute (P5 wires it; see the class note) ---------------------------

    /// "The state has always been what it is now" — no ramp, the whole page is
    /// at the flat value. This is the default, and the only value P3a uses.
    static constexpr offset_t kRampImmediate = INT64_MIN;

    /// Mute/unmute, ramping over ~1.5 ms starting at `atFrame` (in the track's
    /// timeline frames). The anchor is what keeps this POSITION-DETERMINISTIC:
    /// a page rendered out of order, twice, or on another thread produces the
    /// same samples, so the component stays class infinity.
    void setMuted( bool muted, offset_t atFrame = kRampImmediate );
    bool muted() const;

    /// Ramp length in frames at the environment's current rate (~1.5 ms).
    length_t muteRampFrames() const;

    // --- width (proposal 36 §4.2) ----------------------------------------

    void setChannels( idx_t n );
    virtual idx_t getOutputChannels() const override
    {
        return (idx_t) channels_.load( std::memory_order_acquire );
    }

    // --- twComponent ------------------------------------------------------

    // ONE port each way: the channel dimension lives in the page, not in the
    // patch bay (proposal 36 §4.2 — getNOutputs() is the port count and must
    // never be merged with getOutputChannels()).
    virtual idx_t getNInputs() const override  { return 1; }
    virtual idx_t getNOutputs() const override { return 1; }
    virtual const char *getInputName( idx_t ) const override;
    virtual const char *getOutputName( idx_t ) const override;

    virtual void createOutputLatches() override;
    virtual int seekTo( offset_t offset ) override;
    virtual void setBufferSize( length_t ) override {}

    /// The authoritative render at width > 1: one upstream page, one pass, every
    /// channel scaled (proposal 36 §4.3).
    virtual length_t renderPageWide( twOutputPage &page, length_t frames,
                                     const sample_t *input,
                                     length_t inputLength ) override;

    /// THE LEGACY PULL, and the narrow (width 1) render, which reaches this
    /// through the base renderFrames(). `page x gain` over the mono plug seam —
    /// deliberately the same shape twRewire has always had, so a width-1 track
    /// gains exactly one transparent copy and nothing else.
    virtual length_t calcOutputTo( IOVector& dest, idx_t idx ) override;

protected:
    virtual void reset() override;
    virtual void teardown() override;

private:
    // One snapshot of everything the render needs, taken once per page under
    // mutex() (THREADING rule 2) and then read lock-free.
    struct Envelope {
        double   base       = 1.0;            // the fader, linear
        bool     muted      = false;
        offset_t muteAnchor = kRampImmediate;
        length_t ramp       = 1;
    };
    Envelope envelope() const;

    // The multiplier for the frame at absolute position `pos`.
    static double factorAt( const Envelope &e, offset_t pos );

    // True when the envelope is the same value for every frame of
    // [start, start + n) — the overwhelmingly common case, and the one that
    // degenerates to a plain scalar (or, at unity, to no arithmetic at all).
    static bool isFlat( const Envelope &e, offset_t start, length_t n );

    // Scale `n` frames of `src` into `dst` at absolute position `start`.
    // dst == src is allowed.
    static void applyGain( const sample_t *src, sample_t *dst, length_t n,
                           offset_t start, const Envelope &e );

    mutable std::mutex   paramMutex_;      // guards the three fields below
    double               gainDb_{ 0.0 };
    bool                 muted_{ false };
    offset_t             muteAnchor_{ kRampImmediate };

    std::atomic<int>     channels_{ 1 };

    // The page position the narrow render must produce. Set by seekTo(), which
    // freezePage_nolock() calls immediately before rendering, inside
    // cursorMutex_ — the same trick twPluginInsert uses, for the same reason:
    // renderFrames()/calcOutputTo() carry no position in their signature.
    std::atomic<offset_t> renderPos_{ 0 };
};

#endif
