#ifndef _SMIDICUT_H_
#define _SMIDICUT_H_

#include <memory>

#include "app/model/sclipwindow.h"
#include "app/model/sobject.h"
#include "tw/core/twdomains.h"
#include "tw/core/twfraction.h"
#include "tw/events/tweventclipset.h"
#include "tw/events/tweventseq.h"

class QDomElement;
class SLink;
class SMidiSequence;
class SObjectRenderer;
class SMidiCutRendererInline;
class SProject;
class SProjectLoader;

/**
 * The immutable window snapshot an event consumer reads (the SCutSnapshot
 * pattern). Built under mutex() and swapped whole, so a collect() that already
 * holds one keeps a coherent view for its whole duration.
 */
struct SMidiCutSnapshot {
    Fraction srcStartTicks{ 0 };   // anchor in the CONTENT's ticks, exact
    Fraction lengthTicks{ 0 };     // window length in TIMELINE ticks, exact
    Fraction loopTicks{ 0 };       // repeating segment, TIMELINE ticks; 0 = none
    Fraction rate{ 1 };            // content ticks -> timeline ticks scaling

    // Derived, in TIMELINE FRAMES - the domain everything above the clip
    // speaks. The tick -> frame conversion happens HERE and nowhere else
    // (POSITION_DOMAINS rule 7).
    length_t durationFrames = 0;
    length_t loopFrames = 0;
    offset_t startOffsetFrames = 0;

    // The content, re-expressed in FRAMES with this clip's transpose /
    // velocity scale / channel override already applied. Zero of this sequence
    // is the CONTENT's zero, not the window's, so a slip edit changes only the
    // map and never costs a rebuild.
    std::shared_ptr<const twEventSeq> framesSeq;
};

/**
 * SMidiCut - the EVENT clip window (the SCut analogue; deliberately NOT an
 * SCut mode, D8b: an "event mode" inside SCut would push `if (isEvent)` into
 * every reader, capture and preview path of the most-tested class in the repo).
 *
 * THE WINDOW IS TICK-NATIVE (D2). `srcStart`, `lengthTicks` and `loopTicks`
 * are exact-rational MUSICAL ticks and `rate` is an exact Fraction, so a tempo
 * change keeps the same notes inside the clip - a musical window over musical
 * content. Every side the track sees speaks FRAMES, derived through the
 * project's twTempoMap, and that conversion happens EXACTLY ONCE PER VALUE,
 * inside this class.
 *
 * SPLIT IS NON-DESTRUCTIVE (D3). The generic `split-clip` narrows this window
 * through `SClipWindow::setWindowExact`; the shared content is never edited.
 * A note straddling the split keeps its full duration in the head, and the
 * head's window end synthesises its note-off (twEventClipSet, events/CONTRACT
 * inv. 8); the tail never re-issues the note-on, because a note-on before the
 * window reaches a consumer only through the chase set.
 */
class SMidiCut
    : public SObject,
      public SClipWindow
{
    Q_OBJECT
    Q_PROPERTY( double Rate READ getRate WRITE setRate )
    Q_PROPERTY( int Transpose READ getTranspose WRITE setTranspose )
    Q_PROPERTY( double VelocityScale READ getVelocityScale WRITE setVelocityScale )
public:
    /** Like SCut: the cut always creates its OWN content link (+1 ref). */
    SMidiCut( SProject *project, SObject &content );
    virtual ~SMidiCut();

    static SLink *instantiateFromDomElement( SProjectLoader &projectLoader,
                                             QDomElement &element,
                                             SObject *parent );

    SObject &getContent() const;
    /** The content as a sequence, or null when it is something else. */
    SMidiSequence *sequence() const;

    SMidiCutSnapshot getSnapshot() const;

    // --- window, in the window's OWN units ---------------------------------
    Fraction getSrcStartTicks() const;
    Fraction getLengthTicks() const;
    Fraction getLoopTicks() const;
    Fraction getRateExact() const;
    double   getRate() const { return getRateExact().toDouble(); }

    /** The whole window at once, in ticks. One publish. */
    void setWindowTicks( const Fraction &srcStartTicks,
                         const Fraction &lengthTicks,
                         const Fraction &loopTicks, const Fraction &rate );

    // --- per-clip modifiers -------------------------------------------------
    int    getTranspose() const;
    double getVelocityScale() const;
    int    getChannelOverride() const;
    void setTranspose( int semitones );
    void setVelocityScale( double scale );
    void setChannelOverride( int channel );
    void setRate( double rate );

    /** ±4 octaves, clamped at every entry point (mirrors SCut::PITCH_CENTS_LIMIT). */
    static constexpr int TRANSPOSE_LIMIT = 48;
    static int clampTranspose( int t )
    { return t > TRANSPOSE_LIMIT ? TRANSPOSE_LIMIT
           : t < -TRANSPOSE_LIMIT ? -TRANSPOSE_LIMIT : t; }

    // --- SObject ------------------------------------------------------------
    std::shared_ptr<twComponent> getRootComponent() override;
    QWidget *getDetailEditWidget( QWidget *parent ) override;
    QWidget *getInlineEditWidget( QWidget *parent ) override;
    SObjectRenderer *getInlineRenderer() override;

    SContentKind contentKind() const override { return SContentKind::Event; }

    bool hasDuration() const override { return true; }
    length_t getDuration() const override;
    void setDuration( length_t ) override;

    twEventClipResolved resolveEventClip( offset_t clipPos ) override;

    int readPostChildrenAttributes( QDomElement &element ) override;

    // --- SClipWindow --------------------------------------------------------
    //
    // Frames in, frames out (SClipWindow rule 1); the two exact forms carry
    // the CONTENT-domain anchor in TICKS, which is authoritative.
    SObject &asObject() override { return *this; }
    SObject &windowContent() const override { return getContent(); }

    // --- the take-COLUMN seam, FORWARDED ONE LEVEL (proposal 42 M2) --------
    //
    // A window over a take column IS a placement of that column, so it answers
    // the column questions on the column's behalf. Without this the seam was
    // NOT TOTAL: `SObject`'s base returns 0 / null / nothing, so every consumer
    // that reached a take through it silently got the WRAPPER instead — the
    // MIDI event verbs (rejected outright, because the wrapper's `sequence()`
    // is null over a stack), the event editor and virtual keyboard (bound an
    // empty ref), the clip-properties panel (no take navigation), the
    // automation `cut:` owner (a per-take envelope landed on the wrapper) and
    // four testkit asserts (`take=` silently ignored).
    //
    // THE SEAM MEANS "TAKE k OF THIS PLACEMENT'S COLUMN". It does NOT mean
    // "the window whose parameters this placement carries" — that question is
    // `SClipWindow::parametersOf()`, and on a wrapped column the two give
    // different answers on purpose.
    SClipWindow *windowTakeAt( int index ) const override
    { return windowContent().windowTakeAt( index ); }
    int windowTakeCount() const override
    { return windowContent().windowTakeCount(); }
    int activeWindowTakeIndex() const override
    { return windowContent().activeWindowTakeIndex(); }
    SLink *insertWindowTake( SClipWindow &window, int atIndex ) override
    { return windowContent().insertWindowTake( window, atIndex ); }
    void removeWindowTake( int index ) override
    { windowContent().removeWindowTake( index ); }
    void setActiveWindowTake( int index ) override
    { windowContent().setActiveWindowTake( index ); }

    length_t duration() const override { return getDuration(); }
    length_t durationBlocking() const override { return getDuration(); }
    length_t loopLength() const override;
    offset_t startOffset() const override;
    Fraction stretchOrRate() const override { return getRateExact(); }
    Fraction contentAnchorExact() const override { return getSrcStartTicks(); }
    Fraction timelineToSourceExact( const Fraction &relTimeline ) const override;

    void setDurationFromTimeline( length_t d ) override { setDuration( d ); }
    void setStartOffsetFromTimeline( offset_t startOffset ) override;
    void setWindowFromTimeline( offset_t startOffset, length_t duration,
                                length_t loopLength,
                                const Fraction &stretchOrRate ) override;
    void setWindowExact( const Fraction &contentAnchor, length_t duration,
                         length_t loopLength,
                         const Fraction &stretchOrRate ) override;
    void setContentAnchorExact( const Fraction &contentAnchor ) override;
    SClipWindow *cloneWindowOverContent( SProject *project,
                                         SObject &content ) const override;

protected:
    int serializeSelfAttributes( QTextStream &o ) override;

private slots:
    /**
     * The tempo map or the project rate moved: every frame-facing value of
     * this clip is derived from ticks through them, so the snapshot is rebuilt
     * and the duration republished. This is why an event clip needs no
     * `durationSec` migration - ticks are rate-free (D2).
     */
    void onTempoOrRateChanged();
    /** Our content's event table changed. */
    void onContentEventsChanged( offset_t fromClipPos );

private:
    // Callers hold mutex(). Recomputes the frame-domain values AND the
    // frame-domain sequence from the ticks.
    void rebuild_nolock();

public:
    // --- automation (proposal 37 P5) ---------------------------------------
    //
    // `cut:VelocityScale` and `cut:Transpose` are applied when the SNAPSHOT is
    // built (design D5), not at freeze time: they change the EVENTS, and the
    // events are the thing every consumer - the instrument slot, the MIDI-out
    // pump, the piano roll - reads. `cut:Gain` is inherited from SObject and
    // consumed by the track's mix; it means nothing on an event clip.
    virtual void onAutomationChanged( SAutomationLane &lane,
                                      offset_t start, offset_t end ) override;
    virtual void applyAutomationToEngine() override;

private:
    // Publish: durationChanged when the length moved, eventsChanged always.
    void publish_( length_t oldDuration );

    SLink *content_ = nullptr;

    Fraction srcStartTicks_{ 0 };
    Fraction lengthTicks_{ 0 };
    Fraction loopTicks_{ 0 };
    Fraction rate_{ 1 };
    int      transpose_ = 0;
    double   velocityScale_ = 1.0;
    int      channelOverride_ = -1;

    SMidiCutSnapshot snapshot_;
    SMidiCutRendererInline *inlineRenderer_ = nullptr;
};

#endif // _SMIDICUT_H_
