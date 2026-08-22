#ifndef _STAKESTACK_H_
#define _STAKESTACK_H_

#include "app/model/sobject.h"
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
 *  1. All take cuts share the stack's timeline duration; per-take
 *     startOffset (slip), pitch and grain params are free. Length edits go
 *     through setDurationAll()/applyWindowAll(), the ONE place "length ops
 *     affect all lanes" lives.
 *  2. activeTake_ ∈ [-1, nTakes()).
 *  3. A stack never has zero takes, and a single-take stack is collapsed
 *     back to a plain cut by the take actions (see stakehelpers in
 *     saddtakeaction.cpp / sremovetakeaction.cpp).
 *
 * Take link startTime is always 0 (takes are column-relative; the OUTER
 * link owns the timeline placement).
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
    /** The take at index as a WINDOW (null when out of range). */
    SClipWindow *takeAt( int index ) const;
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
     * Add a take (a window, already sized to the stack duration) at atIndex
     * (-1 = append). Maintains the activeTake_ index. Does NOT activate the
     * new take — callers decide (the actions do).
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
    SLink *insertTake( SClipWindow &window, int atIndex = -1 );

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

    // --- window write-through (invariant 1) ------------------------------
    /** Set every take's timeline duration, preserving slip/loop/stretch. */
    void setDurationAll( length_t duration );
    /** Set duration/loop/stretch on every take. Slip offsets are rescaled
     *  by newStretch/oldStretch (offsets live in the stretched OUTPUT
     *  domain — see CLIP_MODEL.md invariant 4). */
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
    // Range-scoped invalidation: only the ACTIVE take is audible, so
    // dirty ranges inside an inactive take's content map to NOTHING.
    QList<SDirtyRange> mapChildRangesToSelf(
        SLink *childLink, const QList<SDirtyRange> &childRanges ) override;
    // Position walk (proposal 09 §15): a column is a window whose content is
    // its ACTIVE take, aligned with the column — so the step is the identity
    // into that take, which is itself a window and continues the chain. The
    // base's childLinks() fallback would descend into EVERY take and report an
    // inactive one's arrangement as sounding.
    bool windowStep( offset_t clipRel, SWindowStep &out ) const override
    {
        SObject *take = activeTakeObject();
        if( !take ) return false;               // no active take: nothing audible
        out.content = take;
        out.pos     = clipRel;
        return true;
    }
    int seekTo( offset_t ) override;
    bool hasDuration() const override { return true; }
    length_t getDuration() const override;
    // Edit/signal-path duration (proposal 19 Phase 2b): blocking snapshot,
    // never the stale try-lock fallback. Used by durationChanged emitters.
    length_t getDurationBlocking() const override;
    /** The column's kind is its takes' kind (Audio while it is empty). */
    SContentKind contentKind() const override;
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
    // Guards against per-take forwarding storms while setDurationAll/
    // applyWindowAll mutate every take; they emit ONE durationChanged after.
    bool forwardSuppressed_ = false;
    std::shared_ptr<twComponent> cpSilence_ = nullptr;   // lazily-built silent component
    STakeStackRendererInline *inlineRenderer_ = nullptr;
};

#endif // _STAKESTACK_H_
