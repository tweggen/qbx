#ifndef _TW_COMPCOLUMN_H_
#define _TW_COMPCOLUMN_H_

#include "tw/graph/twcomponent.h"
#include "tw/events/twcompmap.h"
#include "tw/events/twautomationcurve.h"
#include "tw/events/twfade.h"
#include "tw/mix/twpanlaw.h"
#include <functional>
#include <memory>
#include <vector>

class tw303aEnvironment;
class twView;

/**
 * twCompColumn — A TAKE COLUMN THAT RENDERS ITS COMP MAP (proposal 43 N2).
 *
 * A comp boundary lands on a WORD, ~200 ms. The mix resolves a clip's
 * component ONCE PER PAGE and a page is 65536 frames — 1.37 s at 48 kHz — so
 * per-page resolution is two to seven times too coarse and the map cannot be
 * honoured at the clip. It is honoured HERE, one level below: the column is a
 * component, holds its takes as inputs, and renders each range from whichever
 * the map selects.
 *
 * Nothing above it changes. `twTrackMix` keeps resolving one component per
 * clip per page, and `resolveClip`'s contract is untouched — which is the whole
 * reason proposal 43 chose this over segmenting the clip inside the mix, where
 * `clip.previousPage` (the per-clip DSP-state predecessor) would have had to
 * become per-(clip, take) and every `twPagePlan` consumer would grow a case.
 *
 * IT EXISTS ONLY WHEN A MAP DOES. `STakeStack` hands out the ACTIVE TAKE's
 * component, exactly as before, whenever its map is empty — so every project
 * written before proposal 43, and every golden, renders through the same code
 * as ever. That is not only compatibility theatre: an extra page copy is
 * bit-exact, but routing a stretched take's DSP state through a second
 * component's chaining is not, and no golden should have to prove that.
 *
 * THE MAP IS READ BY POSITION at freeze time, never precomputed and stashed —
 * the mistake this codebase has made three times (level meters, MIDI-out, the
 * metronome), each time by computing at freeze time what had to be read at
 * play time. Here it is the mirror image and the same rule: a page is frozen
 * far from the playhead and for positions nobody is listening to, so the only
 * honest answer is "what does the map say HERE".
 */
class twCompColumn : public twComponent
{
public:
    /** One take: how to resolve its component at a position (proposal 19 Inv-1). */
    using ResolveFn = std::function<twResolvedClip(offset_t)>;

    /**
     * ONE TAKE'S OWN GAIN (QBX-185) — the four factors `twTrackMix::ClipEntry`
     * carries for a clip, carried here PER TAKE instead.
     *
     * WHY THEY CANNOT STAY ONE LEVEL UP. A take's volume, pan, fade and gain
     * envelope are properties of the take's own WINDOW, and the mix composed
     * them for a CLIP ENTRY — one entry per placement, read from
     * `SClipWindow::parametersOf`, i.e. from the ACTIVE take. This column
     * renders each region from whichever take the map selects, so the gain had
     * already been decided above it, per clip and not per region: an inactive
     * take's own volume and fade were carried, serialized, editable and NEVER
     * HEARD, and comping two takes recorded at different levels gave one level
     * to the whole clip.
     *
     * THE SAME FOUR FACTORS, THE SAME PRODUCT, THE SAME FUNCTIONS. Per frame:
     * `scalar * curve->valueAt(f) * fade.gainAt(f, fadeLen) * pan`, with `f`
     * in the TAKE's own domain. It is deliberately the identical expression
     * `twTrackMix::freezePage_nolock` applies to a clip, evaluated through the
     * identical `twAutomationCurve::valueAt` and `twClipFade::gainAt` — two
     * places that could disagree about what a fade means is one more than
     * there should be.
     *
     * `fadeLen` is the TAKE's own length, not its extent in this column's
     * frames: a fade-out is anchored to the end of the window it belongs to.
     * The two coincide for a bounded take (invariant 1', where a take's extent
     * length IS its window length) and differ for an unbounded one.
     *
     * PAN APPLIES AT A WIDTH OF EXACTLY 2 and nowhere else, which is the rule
     * `ClipEntry::pan` already follows (proposal 49 D1): at any other width the
     * take mixes as if centred.
     *
     * `neutral()` is the whole fast path. A take whose gain is neutral is
     * mixed by the straight copy this column has always done, so a column
     * nobody has set a per-take parameter on renders byte for byte as before —
     * and since the DEGENERATE case (no map, every take full) never builds a
     * column at all, so does every project written before this change.
     */
    struct TakeGain {
        double     scalar  = 1.0;   // linear; the take's dB trim, converted
        double     pan     = 0.0;   // [-1, 1]; 0 = centre
        twClipFade fade;            // none() by default
        length_t   fadeLen = 0;     // the TAKE's own length, for gainAt
        std::shared_ptr<const twAutomationCurve> curve;   // null = untouched

        bool neutral() const
        {
            return scalar == 1.0 && pan == 0.0 && fade.none() && !curve;
        }
    };

    twCompColumn( tw303aEnvironment &env, idx_t channels );
    ~twCompColumn() override;

    /**
     * Rebuild the take list. Called from the main thread when the column's
     * takes change; each entry gets its own `twView`, which is what makes a
     * take's lazy reader build and its position mapping ONE resolution.
     */
    void setTakes( std::vector<ResolveFn> takes );

    /** The map, and the fallback the empty parts of it mean. */
    void setCompMap( const twCompMap &map, int activeTake );

    /**
     * ONE TAKE'S EXTENT: it occupies column positions [tStart, tStart + len)
     * and its OWN domain starts at 0 there (plan 53 §4.1, invariant 1′).
     *
     * So a position `p` inside the extent reads the take at `p - tStart`, and
     * a position OUTSIDE it reads NOTHING: the take contributes silence, and
     * — the load-bearing half — the page plan DECLARES NO DEPENDENCY on it
     * there. A plan that declared one would have the scheduler build a lazy
     * reader for a window the take does not have, which is both a wasted
     * freeze and, for a stretched take, a DSP state chain advanced over
     * material nobody is listening to.
     *
     * `len < 0` means UNBOUNDED — the default, and what every caller written
     * before extents gets: the take covers wherever the map sends it, which is
     * exactly the old behaviour. `setTakes` resets every entry to it, so the
     * extents are set AFTER the take list (STakeStack::ensureCompColumn).
     *
     * A take index outside the current list is ignored rather than stored: the
     * list is authoritative and is rebuilt whenever the column's takes change.
     */
    void setInputExtent( int takeIndex, offset_t tStart, length_t len );

    /**
     * THE PROJECT'S AUTO-FADE, in frames, as the length a boundary WITH NO
     * CROSSFADE OF ITS OWN gets (plan 53 P5, decision D3a).
     *
     * A comp boundary is a butt join between two different performances, which
     * is the one join that is NEVER continuous: the two takes were played at
     * different moments, so the waveform steps and the step clicks. That is
     * why the auto-fade reaches here at all, and why nothing in the seamless
     * exemption (D3b, which is about one source read continuously) applies to
     * a boundary.
     *
     * IT IS A FALLBACK AND NOTHING ELSE. A boundary with `xfade > 0` keeps its
     * own length — `set-comp-xfade` is the override and an explicit crossfade
     * always wins — and the value set here is then not read at all. It is
     * applied in `clampedXfade_nolock`, the ONE place both `planPage` and
     * `freezePage` ask what a boundary's crossfade is, so an auto-fade can
     * never make the plan and the render disagree (proposal 19 Inv-1).
     *
     * EVERY CLAMP STILL APPLIES to it: half the distance to each neighbouring
     * boundary, and both takes' extents. So an auto-fade at a boundary where
     * one take has no material to fade across shortens, or vanishes, exactly
     * as an explicit crossfade there would.
     *
     * 0 (the default) is "no auto-fade", which is what every project written
     * before plan 53 reads and therefore what keeps them rendering byte for
     * byte as before.
     */
    void setAutoXfade( int64_t frames );

    /**
     * ONE TAKE'S OWN GAIN (QBX-185). See `TakeGain`.
     *
     * Set AFTER `setTakes`, which rebuilds the entry list and resets every
     * entry to neutral — the same ordering `setInputExtent` needs and for the
     * same reason. A take index outside the current list is ignored rather
     * than stored: the list is authoritative.
     */
    void setInputGain( int takeIndex, const TakeGain &gain );

    idx_t getOutputChannels() const override { return channels_; }

    twPagePlan planPage( offset_t pageStart ) override;

    std::shared_ptr<twOutputPage> freezePage(
        offset_t startPos, const sample_t *inputData, uint64_t inputOffset,
        length_t inputLength, int sampleRate,
        std::shared_ptr<twOutputPage> previousPage = nullptr ) override;

    void invalidatePagesInRange( offset_t start, offset_t end ) override;

    idx_t getNInputs() const override { return 0; }
    idx_t getNOutputs() const override { return 1; }
    const char *getInputName( idx_t ) const override { return "in"; }
    const char *getOutputName( idx_t ) const override { return "out"; }
    bool isSeekable() const override { return true; }
    int seekTo( offset_t ) override;
    length_t calcOutputTo( IOVector &dest, idx_t outChannel ) override;
    void teardown() override;
    void createOutputLatches() override;

protected:
    void reset() override;

public:

private:
    /**
     * The take sounding at a column position, with the degenerate case folded
     * in — an empty map, or a position before the first segment, means the
     * ACTIVE take. One place, so no caller has to remember it (the same reason
     * `STakeStack::takeIndexAt` exists on the model side).
     */
    int takeAt_nolock( offset_t pos ) const;

    /**
     * ONE RUN of the output: a span over which the set of contributing takes
     * does not change. Outside a crossfade that is one take; inside one it is
     * TWO, and their gains vary per frame.
     *
     * What turns "read the map per frame" into "one freeze per region", which
     * is what the page plan has to declare anyway — and the one walk both
     * `planPage` and `freezePage` make, so plan and render cannot disagree
     * (proposal 19 Inv-1 extended to the plan).
     */
    struct CompRun {
        length_t len   = 0;
        int      takeA = -1;    // the sole take, or the OUTGOING one
        int      takeB = -1;    // the INCOMING take; -1 outside a crossfade
        offset_t xStart = 0;    // crossfade start, absolute column position
        int64_t  xLen   = 0;    // crossfade length; 0 outside one
    };
    CompRun runAt_nolock( offset_t pos, length_t limit ) const;

    /**
     * A boundary's crossfade, CLAMPED so two of them can touch but never
     * overlap: each is limited to half the distance to its neighbouring
     * boundaries. Overlapping crossfades would need three takes live at once,
     * and a rule that silently produced that is a rule that produces audio
     * nobody asked for.
     *
     * AND CLAMPED TO BOTH TAKES' EXTENTS (plan 53 §4.1): a crossfade is
     * centred on the boundary, so it needs the OUTGOING take to have material
     * after it and the INCOMING one to have material before it. W1's boundary
     * at T is the case — the outgoing take ends exactly there — and a fade
     * that ignored the extent would fade out of silence, i.e. fade DOWN the
     * incoming take over material that is all there is.
     */
    int64_t clampedXfade_nolock( size_t segIndex ) const;

    /** Does take `t` have material at column position `pos`? */
    bool takeLiveAt_nolock( int t, offset_t pos ) const;
    /**
     * The first extent edge of take `t` strictly after `pos`, or -1 when none.
     * What keeps a CompRun from straddling an extent boundary, so that every
     * run is wholly inside or wholly outside each contributing take — which
     * is what "a span over which the set of contributing takes does not
     * change" has to mean once takes have extents.
     */
    offset_t nextTakeEdgeAfter_nolock( int t, offset_t pos ) const;

    struct TakeEntry {
        std::shared_ptr<twView>       view;
        std::shared_ptr<twOutputPage> previousPage;   // per-take state chain
        offset_t                      exStart = 0;    // extent start, column frames
        length_t                      exLen   = -1;   // < 0 = unbounded
        TakeGain                      gain;           // QBX-185; neutral by default
    };

    /**
     * Take `t`'s own gain at COLUMN position `pos`, for output channel `ch`.
     *
     * The position is converted to the take's own domain here, in one place,
     * because that is the whole content of the bug this fixes: every other
     * per-take quantity in this class is already read in the take's domain and
     * the gain was not read there at all.
     */
    double takeGainAt_nolock( int t, offset_t pos, idx_t ch ) const;

    tw303aEnvironment       &env_;
    idx_t                    channels_;
    std::vector<TakeEntry>   takes_;
    twCompMap                map_;
    int                      activeTake_ = -1;
    int64_t                  autoXfade_ = 0;   // plan 53 P5; 0 = none
};

#endif // _TW_COMPCOLUMN_H_
