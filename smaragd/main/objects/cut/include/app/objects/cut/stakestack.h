#ifndef _STAKESTACK_H_
#define _STAKESTACK_H_

#include "app/model/sobject.h"
#include "tw/events/twcompmap.h"
#include "tw/mix/twcompcolumn.h"
#include "app/model/sobjectrenderer.h"
#include "tw/core/twfraction.h"

class SProject;
class SLink;
class SClipWindow;
class SProjectLoader;
class STakeStack;
class QDomElement;

/**
 * Inline renderer for a take stack: compact mode draws the ACTIVE take
 * exactly like a plain cut ("my link but his object" — the cut renderer
 * reads its window from its own window object, the passed link only supplies the
 * timeline start). No active take renders a dimmed hatch. A small
 * "active/count" badge marks multi-take clips.
 */
class STakeStackRendererInline : public SObjectRenderer
{
    Q_OBJECT
public:
    explicit STakeStackRendererInline( STakeStack & );
    void draw( SLink &, SRenderContext & ) override;
    // Delegates to the ACTIVE take's renderer, identically to draw() (proposal
    // 39 M1): a stack is one clip whose identity is whichever take is audible,
    // so the envelope a caller reads is the envelope on screen.
    bool collectEnvelope( SLink &, const SEnvelopeWindow &, preview_t * ) override;
    // The TAKE-LANE terminals: the same delegation, to the take the caller
    // NAMES rather than to the audible one. takeIndex < 0 keeps meaning "the
    // audible one", so both are exactly draw()/collectEnvelope() then.
    void drawTake( SLink &, SRenderContext &, int takeIndex ) override;
    bool collectTakeEnvelope( SLink &, const SEnvelopeWindow &, preview_t *,
                              int takeIndex ) override;
private:
    STakeStack &stack() const;
    SObject *takeFor( int takeIndex ) const;
};

/**
 * STakeStack — a COLUMN of parallel takes (proposal 17).
 *
 * Placed on a track like any clip (SLink carries the timeline start), it
 * holds one child SLink per take, each wrapping an SClipWindow over that
 * take's media (an SCut for audio; proposal 37 D8b made the stack window-
 * typed rather than SCut-typed, so an event take needs no change here).
 * Exactly one take is audible at a time (activeTake_, -1 = none);
 * the stack delegates getRootComponent()/mapTimelineToComponentPos()/
 * preview to the active take, so to twTrackMix a stack is ONE clip
 * whose component identity changes on take selection (twView resolves it
 * lazily). Switching takes emits durationChanged, which drives the
 * standard STrack sync (updateClip + invalidateRenderPath) — no engine
 * changes needed.
 *
 * Invariants:
 *  1′. EVERY TAKE HAS AN EXTENT [tStart, tEnd) ⊆ [0, duration_) (plan 53
 *     §4.1). A take covers only the span its material actually occupies, and
 *     THERE IS NO SILENT TAKE BODY ANYWHERE: a position no take covers plays
 *     silence, and there is no fall-back to another take (§3.1 step 5).
 *     Per-take startOffset (slip), pitch and grain params are free, as before.
 *     Length edits go through setDurationAll()/applyWindowAll(), the ONE place
 *     "length ops affect all lanes" lives — and under 1′ those CLAMP extents
 *     rather than overwrite them (see setDurationAll).
 *
 *     The extent is STORED WHERE IT ALREADY WAS WRITTEN: tStart is the take
 *     SLink's startTime (unconditionally serialized since always; 0 for every
 *     file written before FORMAT_VERSION 3) and tEnd - tStart is the take
 *     window's own duration. No new per-take field, and no new element.
 *
 *     THE FULL-EXTENT COLUMN IS THE DEGENERATE CASE and is bit-for-bit the
 *     old one: every take at tStart = 0 with the column's duration is exactly
 *     invariant 1, which is why no golden and no existing column gate moves.
 *  2. activeTake_ ∈ [-1, nTakes()).
 *  3′. A stack never has zero takes, and a single-take stack is collapsed
 *     back to a plain cut by the take actions (see stakehelpers in
 *     saddtakeaction.cpp / sremovetakeaction.cpp) ONLY when that one take is
 *     FULL-EXTENT (plan 53 §4.2). A one-take column with a partial take is
 *     legal: it is the union placeholder Q5 produces on an un-armed member,
 *     and it is what a column becomes when the user removes all but one
 *     partial take. Its length is duration_, so it is representable.
 *
 * THE COLUMN HAS A LENGTH OF ITS OWN: duration_ (plan 53 §4.1). Before 1′ it
 * did not — getDuration() answered the ACTIVE take's duration, else take 0's
 * — and that is exactly what 1′ makes untenable: a column whose active take
 * covers only its first half would report half its length and grow and shrink
 * as the user clicked between takes.
 */
class STakeStack : public SObject
{
    Q_OBJECT
public:
    explicit STakeStack( SProject *project );
    virtual ~STakeStack();

    static SLink *instantiateFromDomElement( SProjectLoader &projectLoader,
                                             QDomElement &element,
                                             SObject *parent );

    // --- takes ----------------------------------------------------------
    int nTakes() const { return childCount(); }
    int activeTakeIndex() const { return activeTake_; }

    // --- the COMP MAP (proposal 43 N1) -----------------------------------
    //
    // WHICH TAKE SOUNDS WHERE, in the column's own frame domain. An EMPTY map
    // -- the state every column has until something writes one, and the state
    // every project written before proposal 43 loads in -- means "activeTake_
    // everywhere", so no map means no behaviour change anywhere.
    //
    // N1 puts the map in the MODEL only: nothing reads it yet, so nothing
    // sounds different. `twCompColumn` (N2) is what makes it audible, and it
    // will read it BY POSITION at freeze time -- never precomputed and stashed,
    // which is the mistake this codebase has made three times (the level
    // meters, MIDI-out, the metronome).
    const twCompMap &compMap() const { return compMap_; }
    void setCompMap( const twCompMap &map );

    /**
     * The take sounding at a column position: the map's answer, or
     * `activeTake_` where the map does not say, and -1 where the take so
     * chosen HAS NO MATERIAL THERE. THE one place the degenerate case is
     * spelled, so no consumer has to remember it.
     *
     * THE -1 IS §3.1 STEP 5 AND IS NOT A FALL-BACK (plan 53, review 2 M2).
     * A position whose selected take is outside its extent plays SILENCE;
     * it does not quietly sound some other take that happens to cover it.
     * For a full-extent column every take covers every position, so this
     * reduces to exactly the old answer.
     */
    int takeIndexAt( int64_t columnPos ) const
    {
        const int t = compMap_.takeAt( columnPos );
        const int chosen = ( t >= 0 && t < childCount() ) ? t : activeTake_;
        if( chosen < 0 || chosen >= childCount() ) return -1;
        return takeCoversPos( chosen, (offset_t) columnPos ) ? chosen : -1;
    }
    /** The take at index as a WINDOW (null when out of range). */
    SClipWindow *takeAt( int index ) const;

    int serializeInlineChildren( QTextStream &o ) override;
    SClipWindow *activeTake() const { return takeAt( activeTake_ ); }
    /** SObject: index < 0 means the ACTIVE take (the generic take seam). */
    SClipWindow *windowTakeAt( int index ) const override
    { return takeAt( index < 0 ? activeTake_ : index ); }
    /**
     * The rest of the generic take-column seam (proposal 21 L4). One-line
     * forwarders: the stack has been window-typed since proposal 37 D8b, so
     * there is nothing audio-specific left to translate. They exist so
     * `add-midi-take` can build a column of EVENT takes from `objects/midi`,
     * which has no edge to this slice.
     */
    int windowTakeCount() const override { return nTakes(); }
    int activeWindowTakeIndex() const override { return activeTake_; }
    SLink *insertWindowTake( SClipWindow &window, int atIndex ) override
    { return insertTake( window, atIndex ); }
    void removeWindowTake( int index ) override { removeTake( index ); }
    void setActiveWindowTake( int index ) override { setActiveTake( index ); }
    /** The take at index as the model OBJECT it also is (delegation target). */
    SObject *takeObjectAt( int index ) const;
    SObject *activeTakeObject() const { return takeObjectAt( activeTake_ ); }

    /**
     * Add a take at atIndex (-1 = append), covering the column span
     * [tStart, tStart + tLen). Maintains the activeTake_ index. Does NOT
     * activate the new take — callers decide (the actions do).
     *
     * THE DEFAULTS ARE "THE WHOLE COLUMN", which is what makes every caller
     * written before plan 53 §4.1 correct unchanged: tStart = 0, and
     * tLen < 0 means "the window's own duration", which is the duration those
     * callers had already sized the window to. The column's duration_ grows
     * to cover the new take, so the FIRST take of a fresh column still
     * establishes its length exactly as take 0 used to answer for it.
     *
     * HOMOGENEITY (proposal 37 D8b): a stack is a column of ALTERNATIVES for
     * one region, so every take must carry the same kind of material. A take
     * whose contentKind() differs from the takes already here is REFUSED and
     * null is returned — the caller (add-take) turns that into a rejected
     * action rather than a column that plays audio or notes depending on which
     * lane is armed.
     *
     * Returns the new take link, or null if refused.
     */
    SLink *insertTake( SClipWindow &window, int atIndex = -1,
                       offset_t tStart = 0, length_t tLen = -1 );

    // --- TAKE EXTENTS (invariant 1′, plan 53 §4.1) ------------------------
    //
    // Read straight off the take's link and window — there is no cached copy
    // to go stale, and the one derived fact (is ANY take partial) is cached
    // because the resolution rule asks it on every resolve.

    /** The take's extent start in COLUMN frames (its link's startTime). */
    offset_t takeExtentStart( int index ) const;
    /** The take's extent length in column frames (its window's duration). */
    length_t takeExtentLength( int index ) const;
    /** One past the take's last covered column frame. */
    offset_t takeExtentEnd( int index ) const
    { return takeExtentStart( index ) + (offset_t) takeExtentLength( index ); }
    /** Does this take cover the WHOLE column? */
    bool takeIsFullExtent( int index ) const;
    /** Does this take have material at `columnPos`? */
    bool takeCoversPos( int index, offset_t columnPos ) const
    { return columnPos >= takeExtentStart( index )
          && columnPos <  takeExtentEnd( index ); }

    /**
     * Move a take's extent. The ONE writer, so the partial-take cache and the
     * column's own length cannot drift from the links they are derived from.
     * `len < 0` keeps the take's current length.
     */
    void setTakeExtent( int index, offset_t tStart, length_t len = -1 );

    /**
     * DOES ANY TAKE COVER LESS THAN THE COLUMN? Cached: the resolution rule
     * below asks on every resolve, and the answer changes only through
     * insertTake / removeTake / setTakeExtent / the write-through / a take's
     * own durationChanged.
     */
    bool hasPartialTake() const { return hasPartial_; }

    /**
     * THE RESOLUTION RULE — ONE PLACE HONOURS EXTENTS (plan 53 §4.1).
     *
     * A column with ANY partial take resolves through `twCompColumn` even when
     * its map is EMPTY. The original path hands the column position straight
     * to the active take, which is correct only while that take is
     * full-extent; `twCompColumn` is the one object that knows how to shift a
     * take's position by its tStart and contribute nothing outside its extent.
     *
     * So the comp column exists when there is a map OR a partial take, and
     * the degenerate column — no map, every take full — still takes the
     * ORIGINAL path, byte for byte (proposal 43 N2's reason, unchanged: an
     * extra page copy is bit-exact, but routing a stretched take's DSP state
     * through a second component's chaining is not).
     */
    bool resolvesThroughComp() const
    { return nTakes() > 0 && ( !compMap_.empty() || hasPartial_ ); }

    /**
     * The column's length as its TAKES imply it: max_i( tStart_i + len_i ).
     * What a file with no `duration=` attribute loads as, and the value that
     * makes `duration=` informative-or-absent on save.
     */
    length_t derivedDuration() const;

    /**
     * Set the column's own length WITHOUT touching a single take (plan 53
     * §4.1). The loader and `split-clip`'s per-half rebuild are the callers:
     * both already know every take's extent and must not have them clamped
     * underneath. Every OTHER length edit goes through setDurationAll /
     * applyWindowAll, which clamp.
     */
    void setColumnDuration( length_t duration );

    /**
     * Remove the take at index (the cut is released via its link refcount).
     * The removed take being active leaves activeTake_ = -1; indices above
     * shift down. Emits durationChanged so the track resyncs.
     */
    void removeTake( int index );

    /** Set the audible take (-1 = none). Emits activeTakeChanged and
     *  durationChanged (the track-sync signal: updateClip resets the clip's
     *  state chain, invalidateRenderPath re-freezes only this path). */
    void setActiveTake( int index );

    // --- window write-through (invariant 1′) -----------------------------
    //
    // A COLUMN EDGE MOVE CLAMPS EXTENTS, it does not overwrite them (plan 53
    // §4.1). A right-edge trim shortens every take that reaches past the new
    // end; a right-edge extend grows only the takes whose extent ENDED at the
    // old end — they were clamped BY the column, while a take that ended
    // earlier was not and must stay where the user put it.
    //
    // FOR A FULL-EXTENT COLUMN THIS IS EXACTLY THE OLD BEHAVIOUR: every take
    // reaches the old end, so a trim shortens all of them and an extend grows
    // all of them. The clamp is a generalisation that reduces to "set every
    // take's duration", which is why no existing column gate moves.
    //
    // A trim that would make a take EMPTY is NOT refused here: refusal needs a
    // verb that can reject, so it belongs to `resize-clip` (plan 53 A0b). The
    // model clamps to a minimum of one frame and says so in the log.

    /** Set the column's duration, clamping every take's extent into it. */
    void setDurationAll( length_t duration );
    /** Set duration/loop/stretch on every take. Slip offsets are rescaled
     *  by newStretch/oldStretch (offsets live in the stretched OUTPUT
     *  domain — see CLIP_MODEL.md invariant 4). Extents clamp as above. */
    void applyWindowAll( length_t duration, length_t loopLength,
                         const Fraction &stretch );

    // --- SObject ---------------------------------------------------------
    std::shared_ptr<twComponent> getRootComponent() override;
    offset_t mapTimelineToComponentPos( offset_t off ) override;
    /**
     * A stack of EVENT takes resolves to its active take, exactly as the audio
     * delegation does (proposal 37 P1). Without this the track would route the
     * stack into its event clip set - contentKind() says Event - and get an
     * empty record back, i.e. a silently mute column.
     */
    twEventClipResolved resolveEventClip( offset_t clipPos ) override;
    // Inv-1: resolve component + mapped position via the active take in
    // ONE call, so a take switch or lazy reader build can't split them.
    twResolvedClip resolveClip( offset_t off ) override;
    // Range-scoped invalidation. UNDER 1′ EVERY TAKE MAPS, not only the
    // active one: a comped or partial column makes an inactive take audible
    // over its own span, so an edit inside it must bump an epoch. The child's
    // ranges are shifted by its tStart (the base's ShiftMap already does that,
    // because tStart IS the take link's startTime) and INTERSECTED with its
    // extent — an edit outside the extent is windowed away and bumps nothing.
    QList<SDirtyRange> mapChildRangesToSelf(
        SLink *childLink, const QList<SDirtyRange> &childRanges ) override;
    // NO bumpRenderChainEpochRange OVERRIDE, AND THAT IS MEASURED rather than
    // assumed. A take column DOES own an engine component once it resolves
    // through `twCompColumn`, whose per-take `twView`s carry their own page
    // caches, so an override pushing the range into it looks obviously right
    // — and `STrack::bumpRenderChainEpochRange` already invalidates
    // `cpTrackMix_` over the same range, which reaches the column through the
    // clip entry it resolved. Added on a wrong diagnosis and then SABOTAGED:
    // with the push removed, all five of A0a's gates still pass, including
    // `take_extents_invalidate`, whose whole subject is this walk. A guard a
    // sabotage cannot bite is a guard with no reason to exist, and a second
    // invalidation of the same range is not free.
    //
    // (What actually made that diagnosis look right was the slip THROTTLE —
    // `SCut::SLIP_INVALIDATE_MS`, 333 ms, one immediate shot per process which
    // `add-take startOffset=` had already spent. See
    // `take_extents_invalidate.qxa`'s header.)
    // Position walk (proposal 09 §15): a column is a window whose content is
    // its ACTIVE take — so the step is into that take, which is itself a
    // window and continues the chain. The base's childLinks() fallback would
    // descend into EVERY take and report an inactive one's arrangement as
    // sounding.
    //
    // UNDER 1′ THE STEP SUBTRACTS tStart and returns FALSE outside the extent:
    // the identity it used to be is the full-extent case of exactly that, and
    // a position the active take has no material for is not a position in it.
    bool windowStep( offset_t clipRel, SWindowStep &out ) const override;
    int seekTo( offset_t ) override;
    bool hasDuration() const override { return true; }
    length_t getDuration() const override;
    // Edit/signal-path duration (proposal 19 Phase 2b): blocking snapshot,
    // never the stale try-lock fallback. Used by durationChanged emitters.
    length_t getDurationBlocking() const override;
    /** The column's kind is its takes' kind (Audio while it is empty). */
    SContentKind contentKind() const override;
    // STILL THE ACTIVE TAKE'S, UNWINDOWED, and that is A0b's row rather than
    // an oversight (plan 53 §4.1 lists it beside collectEnvelope/drawTake).
    // A preview is what the PAINTER reads, so windowing it here while both
    // painters still draw a take across the whole column would produce a
    // half-windowed picture: an envelope that stops at the extent inside a
    // waveform that does not. The pair moves together, in A0b.
    bool hasPreview() const override;
    int getPreview( preview_t *dest, offset_t start, length_t length,
                    offset_t nProbes ) override;
    QWidget *getDetailEditWidget( QWidget *parent ) override;
    QWidget *getInlineEditWidget( QWidget *parent ) override;
    SObjectRenderer *getInlineRenderer() override;

    int readPostChildrenAttributes( QDomElement &element ) override;

public slots:
    /** Generic duration setter (SObject slot): forwards to all takes. */
    void setDuration( length_t ) override;

signals:
    void activeTakeChanged( int index );

private slots:
    // A take cut's window changed (slip, duration, …). Forward as our own
    // durationChanged when it is the audible take, so the track resyncs.
    void onTakeCutChanged( length_t );

protected:
    int serializeSelfAttributes( QTextStream &o ) override;

private:
    std::shared_ptr<twComponent> ensureSilence();

    int activeTake_ = -1;
    twCompMap compMap_;

    /**
     * THE COLUMN'S OWN LENGTH (invariant 1′). Not derivable from the takes
     * once one may be partial: W1's column, whose only full-extent take is an
     * inactive one, would otherwise report the active take's partial length.
     *
     * Serialized as `duration=` ONLY when it differs from derivedDuration(),
     * and derived as that maximum when the attribute is absent — which is
     * every file written before FORMAT_VERSION 3, so they all re-serialize
     * byte-identically.
     */
    length_t duration_ = 0;

    /** Cache behind hasPartialTake(); recomputed by refreshPartial_(). */
    bool hasPartial_ = false;
    void refreshPartial_();
    /**
     * The ONE body behind setDurationAll and applyWindowAll: move the column
     * to `duration` and CLAMP every take's extent into it (the rule spelled
     * above the two public setters). `wholeWindow` false preserves each take's
     * own loop and stretch, which is what setDurationAll promises.
     */
    void applyColumnLength_( length_t duration, bool wholeWindow,
                             length_t loopLength, const Fraction &stretch );

    // THE COMP COLUMN (proposal 43 N2), built ONLY when a map exists.
    //
    // With an EMPTY map this object hands out the ACTIVE TAKE's component
    // exactly as it always has, so every project written before proposal 43 --
    // and every golden -- renders through the same code as ever. That is not
    // compatibility theatre: an extra page copy is bit-exact, but routing a
    // stretched take's DSP state through a second component's chaining is not,
    // and no golden should have to prove that.
    std::shared_ptr<twCompColumn> cpComp_;
    std::shared_ptr<twCompColumn> ensureCompColumn();
    // Guards against per-take forwarding storms while setDurationAll/
    // applyWindowAll mutate every take; they emit ONE durationChanged after.
    bool forwardSuppressed_ = false;
    std::shared_ptr<twComponent> cpSilence_ = nullptr;   // lazily-built silent component
    STakeStackRendererInline *inlineRenderer_ = nullptr;
};

#endif // _STAKESTACK_H_
