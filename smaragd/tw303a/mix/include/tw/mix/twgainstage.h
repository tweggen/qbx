#ifndef _TWGAINSTAGE_H
#define _TWGAINSTAGE_H

#include "tw/graph/twcomponent.h"
#include "tw/events/twautomationcurve.h"

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

    // --- automation (proposal 37 P5, design D5 / §4.5) ---------------------
    //
    // THE CURVE IS A SNAPSHOT, and it is swapped, never edited: the render
    // reads the shared_ptr ONCE per page into a local (THREADING rule 2), so a
    // page already being frozen finishes against the table it started with.
    // A NULL curve is the SCALAR path — the same arithmetic, and at 0 dB the
    // same pure copy, that every render without a lane has always taken. That
    // is what keeps the golden corpus byte-identical across this phase.

    /// The `self:Volume` lane, in dB (the fader's own space,
    /// app/timeline/sfadercurve.h). `absolute` false is TRIM: the curve is
    /// SUMMED with gainDb() — dB sum == gain product, which is exactly what
    /// "static value x curve" means. `absolute` true is READ: the curve alone,
    /// and the fader's stored value is not consumed.
    void setVolumeCurve( std::shared_ptr<const twAutomationCurve> curve,
                         bool absolute );
    std::shared_ptr<const twAutomationCurve> volumeCurve() const;

    /// The `self:Muted` lane: >= 0.5 is muted. A STEP table, and each
    /// transition gets the SAME ~1.5 ms ramp setMuted() uses — an automated
    /// mute must not click any more than a button-driven one. Before the first
    /// breakpoint the track is AUDIBLE (the lane's own convention; "muted from
    /// frame 0" is what the structural mute says).
    void setMuteCurve( std::shared_ptr<const twAutomationCurve> curve );
    std::shared_ptr<const twAutomationCurve> muteCurve() const;

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

public:
    // --- the pure fader arithmetic, PUBLIC since proposal 21 L1a -----------
    //
    // The live pump (design D1) applies a track's fader to a BLOCK it rendered
    // itself, outside the graph: it never freezes a page, so it cannot reach
    // renderPageWide(). It needs exactly what a page render needs — one
    // Envelope snapshot taken on the main thread when the plan is built, and
    // the same position-pure applyGain over it — and it must be THE SAME
    // arithmetic, or an armed track's fader would differ from the frozen one it
    // hands back to at disarm. So the snapshot type and the three pure
    // functions below are public; nothing else about this class is.

    // One snapshot of everything the render needs, taken once per page under
    // mutex() (THREADING rule 2) and then read lock-free.
    struct Envelope {
        double   base       = 1.0;            // the fader, linear
        double   baseDb     = 0.0;            // ...and in dB, for the Trim sum
        bool     muted      = false;
        offset_t muteAnchor = kRampImmediate;
        length_t ramp       = 1;
        std::shared_ptr<const twAutomationCurve> vol;    // dB
        bool     volAbsolute = false;                    // Read (vs Trim)
        std::shared_ptr<const twAutomationCurve> mute;   // >= 0.5 == muted
    };
    /// A snapshot of the fader as it stands right now. Main thread.
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

private:
    // The multiplier a mute CURVE gives at `pos`, ramp included.
    static double muteFactorFromCurve( const twAutomationCurve &c, offset_t pos,
                                       length_t ramp );
    // True when `c` yields ONE value for every frame of [start, start + n).
    static bool curveIsFlatOver( const twAutomationCurve &c, offset_t start,
                                 length_t n );

    mutable std::mutex   paramMutex_;      // guards the fields below
    double               gainDb_{ 0.0 };
    bool                 muted_{ false };
    offset_t             muteAnchor_{ kRampImmediate };
    std::shared_ptr<const twAutomationCurve> volCurve_;
    bool                 volAbsolute_{ false };
    std::shared_ptr<const twAutomationCurve> muteCurve_;

    std::atomic<int>     channels_{ 1 };

    // The page position the narrow render must produce. Set by seekTo(), which
    // freezePage_nolock() calls immediately before rendering, inside
    // cursorMutex_ — the same trick twPluginInsert uses, for the same reason:
    // renderFrames()/calcOutputTo() carry no position in their signature.
    std::atomic<offset_t> renderPos_{ 0 };
};

#endif
