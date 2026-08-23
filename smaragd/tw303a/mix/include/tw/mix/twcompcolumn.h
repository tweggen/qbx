#ifndef _TW_COMPCOLUMN_H_
#define _TW_COMPCOLUMN_H_

#include "tw/graph/twcomponent.h"
#include "tw/events/twcompmap.h"
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
     */
    int64_t clampedXfade_nolock( size_t segIndex ) const;

    struct TakeEntry {
        std::shared_ptr<twView>       view;
        std::shared_ptr<twOutputPage> previousPage;   // per-take state chain
    };

    tw303aEnvironment       &env_;
    idx_t                    channels_;
    std::vector<TakeEntry>   takes_;
    twCompMap                map_;
    int                      activeTake_ = -1;
};

#endif // _TW_COMPCOLUMN_H_
