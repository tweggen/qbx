#ifndef _STAKESTACK_H_
#define _STAKESTACK_H_

#include "app/model/sobject.h"
#include "app/model/sobjectrenderer.h"
#include "tw/core/twfraction.h"

class SProject;
class SLink;
class SCut;
class SProjectLoader;
class STakeStack;
class QDomElement;

/**
 * Inline renderer for a take stack: compact mode draws the ACTIVE take
 * exactly like a plain cut ("my link but his object" — the cut renderer
 * reads its window from its own SCut, the passed link only supplies the
 * timeline start). No active take renders a dimmed hatch. A small
 * "active/count" badge marks multi-take clips.
 */
class STakeStackRendererInline : public SObjectRenderer
{
    Q_OBJECT
public:
    explicit STakeStackRendererInline( STakeStack & );
    void draw( SLink &, SRenderContext & ) override;
private:
    STakeStack &stack() const;
};

/**
 * STakeStack — a COLUMN of parallel takes (proposal 17).
 *
 * Placed on a track like any clip (SLink carries the timeline start), it
 * holds one child SLink per take, each wrapping an SCut over that take's
 * media. Exactly one take is audible at a time (activeTake_, -1 = none);
 * the stack delegates getRootComponent()/mapTimelineToComponentPos()/
 * preview to the active take's cut, so to twTrackMix a stack is ONE clip
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
    SCut *takeCutAt( int index ) const;
    SCut *activeCut() const { return takeCutAt( activeTake_ ); }

    /**
     * Add a take (an SCut, already windowed to the stack duration) at
     * atIndex (-1 = append). Maintains the activeTake_ index. Does NOT
     * activate the new take — callers decide (the actions do).
     * Returns the new take link.
     */
    SLink *insertTake( SCut &cut, int atIndex = -1 );

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
    // Inv-1: resolve component + mapped position via the active take's SCut in
    // ONE call, so a take switch or lazy reader build can't split them.
    twResolvedClip resolveClip( offset_t off ) override;
    // Range-scoped invalidation: only the ACTIVE take is audible, so
    // dirty ranges inside an inactive take's content map to NOTHING.
    QList<SDirtyRange> mapChildRangesToSelf(
        SLink *childLink, const QList<SDirtyRange> &childRanges ) override;
    int seekTo( offset_t ) override;
    bool hasDuration() const override { return true; }
    length_t getDuration() const override;
    // Edit/signal-path duration (proposal 19 Phase 2b): blocking snapshot,
    // never the stale try-lock fallback. Used by durationChanged emitters.
    length_t getDurationBlocking() const override;
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
