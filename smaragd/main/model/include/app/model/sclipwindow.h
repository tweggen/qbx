#ifndef _SCLIPWINDOW_H_
#define _SCLIPWINDOW_H_

#include "app/model/sobject.h"
#include "tw/core/twfraction.h"

class SLink;
class SProject;

/**
 * SClipWindow — the WINDOW layer of the clip model, as an interface.
 *
 * CLIP_MODEL.md's three layers are placement (`SLink`), window (`SCut` today,
 * `SMidiCut` from proposal 37 P1) and content. Every windowed verb —
 * split-clip, resize-clip, duplicate-clip, unsplit-clip, set-clip-name,
 * add/remove/select-take — needs exactly the arithmetic below and nothing else
 * that is specific to audio. Before this interface each of them reached for
 * `dynamic_cast<SCut*>` (`ssplitclipaction.cpp` even compared the class NAME),
 * which is why a second window type could not exist without editing all of
 * them. They dispatch here instead.
 *
 * Two rules make this interface safe to implement for a window whose content
 * is measured in something other than frames (ticks, for an event clip):
 *
 *  1. THE READ API IS FRAME-FACING. `duration()`, `loopLength()` and
 *     `startOffset()` are TIMELINE frames at the project rate, because that is
 *     the domain the track, the mixer and every gesture speak
 *     (POSITION_DOMAINS.md). A window stores whatever it likes internally.
 *  2. THE SETTERS TAKE TIMELINE FRAMES AND CONVERT EXACTLY ONCE, inside the
 *     implementation. A caller must never do the conversion itself: two
 *     callers converting independently is how a rounding difference becomes an
 *     off-by-one clip edge. The one exception is the pair that speaks the
 *     CONTENT's own units explicitly (`contentAnchorExact` /
 *     `setContentAnchorExact` / `setWindowExact`), which exist because the
 *     anchor is stored content-authoritative (proposal 18 Phase 3) and must
 *     survive a stretch edit without drifting.
 *
 * What is deliberately NOT here: pitch, formant preservation, warp anchors,
 * the grain params and the slip THROTTLE. Those are audio-specific and stay on
 * `SCut`; a verb that edits them is an audio verb and may cast.
 *
 * Lifetime/ownership is unchanged: an implementation IS an `SObject` (see
 * `asObject()`), owned exactly as before, and this interface never allocates
 * except in `cloneWindowOver()` / `wrapContent()`, whose results are plain new
 * SObjects the caller places with an `SLink` (model invariant 6).
 */
class SClipWindow
{
public:
    virtual ~SClipWindow() {}

    // --- identity ---------------------------------------------------------

    /** This window as the model object it also is (for links, names, signals). */
    virtual SObject &asObject() = 0;
    const SObject &asObject() const
    { return const_cast<SClipWindow *>( this )->asObject(); }

    /** The content this window looks at (the cut's sample, the sequence, …). */
    virtual SObject &windowContent() const = 0;

    /** Audio or Event — the window's kind IS its content's kind. */
    SContentKind contentKind() const { return asObject().contentKind(); }

    // --- reads, in TIMELINE frames ----------------------------------------

    /** Timeline length of the window (stretch/rate already applied). */
    virtual length_t duration() const = 0;
    /**
     * Edit-path duration: never the stale try-lock fallback (SObject's
     * getDurationBlocking contract). Anything that BAKES a duration into a new
     * window or an inverse action must use this one.
     */
    virtual length_t durationBlocking() const = 0;
    /** Length of the repeating segment; 0 = not looping. */
    virtual length_t loopLength() const = 0;
    /** Slip: where in the content the window starts, in timeline frames. */
    virtual offset_t startOffset() const = 0;
    /**
     * The window's time scaling: an audio cut's stretch, an event clip's rate.
     * Exact, never a double — a clip edge computed from a rounded ratio drifts.
     */
    virtual Fraction stretchOrRate() const = 0;
    /** The slip anchor in the CONTENT's own units (frames, ticks, …), exact. */
    virtual Fraction contentAnchorExact() const = 0;

    /**
     * Window-relative TIMELINE position -> position in the CONTENT's own
     * units, exact. This is the map split-clip needs: the tail's anchor is
     * `timelineToSourceExact(splitOffset)` with no rounding anywhere.
     */
    virtual Fraction timelineToSourceExact( const Fraction &relTimeline ) const = 0;

    // --- writes -----------------------------------------------------------

    /** Set the timeline length, publishing the change (durationChanged). */
    virtual void setDurationFromTimeline( length_t duration ) = 0;
    /** Set the slip from a TIMELINE offset; converted exactly once, inside. */
    virtual void setStartOffsetFromTimeline( offset_t startOffset ) = 0;
    /** The whole window at once, from timeline values; one publish. */
    virtual void setWindowFromTimeline( offset_t startOffset, length_t duration,
                                        length_t loopLength,
                                        const Fraction &stretchOrRate ) = 0;
    /** The whole window at once, anchor in the CONTENT's units; one publish. */
    virtual void setWindowExact( const Fraction &contentAnchor, length_t duration,
                                 length_t loopLength,
                                 const Fraction &stretchOrRate ) = 0;
    /**
     * Move the anchor only, WITHOUT publishing — the "raw" form used while a
     * freshly created window is still unplaced. A placed window must be
     * slipped through its own (audio) setter so the render path is invalidated.
     */
    virtual void setContentAnchorExact( const Fraction &contentAnchor ) = 0;

    // --- copying ----------------------------------------------------------

    /**
     * A NEW window object over the SAME content, with this window's parameters
     * copied faithfully (duplicate-clip semantics). The result is unplaced:
     * the caller gives it an `SLink`.
     */
    virtual SClipWindow *cloneWindowOver( SProject *project ) const = 0;

    // --- the wrap factory -------------------------------------------------

    /**
     * Wrap raw content in the window type that fits its `contentKind()`
     * (`SCut` for Audio, `SMidiCut` for Event from P1 on). Returns null when
     * no window type is registered for that kind.
     *
     * Registration is by static initializer from the slice that owns the
     * window type — the same pattern (and the same OBJECT-library requirement)
     * as SProjectLoader::registerSObjectClass, so app/model still names no
     * concrete object type.
     */
    typedef SClipWindow *( *WrapFn )( SProject *, SObject &content );
    static void registerWrapFactory( SContentKind kind, WrapFn fn );
    static SClipWindow *wrapContent( SProject *project, SObject &content );

    // --- the take-COLUMN factory (proposal 21 L4) -------------------------

    /**
     * Wrap a plain windowed placement into a take COLUMN in place, and
     * collapse a one-take column back into a plain placement — the pair
     * `stakes::wrapCutLinkIntoStack` / `stakes::collapseSingleTakeStack`
     * already implements generically (both take an `SClipWindow`, neither
     * names `SCut`).
     *
     * They are registered here for exactly the reason `windowTakeAt` exists on
     * `SObject`: `objects/midi` sits at the RANK of `objects/cut` and must not
     * depend on it, but `add-midi-take` has to be able to turn a plain MIDI
     * clip into a column of MIDI takes. Registration is by static initializer
     * from the slice that OWNS `STakeStack`, like the wrap factory above.
     * Null (unregistered) yields null, never a crash.
     */
    typedef SLink *( *ColumnWrapFn )( SProject *, SObject *lane, SLink *clipLink );
    typedef SLink *( *ColumnCollapseFn )( SObject *lane, SLink *columnLink );
    static void registerTakeColumnFactory( ColumnWrapFn wrap,
                                           ColumnCollapseFn collapse );
    static SLink *wrapIntoTakeColumn( SProject *project, SObject *lane,
                                      SLink *clipLink );
    static SLink *collapseTakeColumn( SObject *lane, SLink *columnLink );

    /** The window interface of an object, or null when it is not a window. */
    static SClipWindow *of( SObject *obj );
    static SClipWindow *of( SObject &obj ) { return of( &obj ); }
};

#endif // _SCLIPWINDOW_H_
