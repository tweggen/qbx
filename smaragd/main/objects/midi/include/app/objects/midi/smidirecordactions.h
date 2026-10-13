#ifndef _SMIDIRECORDACTIONS_H_
#define _SMIDIRECORDACTIONS_H_

#include <QList>
#include <QString>
#include <vector>

#include "app/actions/saction.h"
#include "app/objects/midi/smidisequence.h"

class SCompositeAction;

/**
 * The MIDI RECORDING verbs (proposal 21 L4 = 37 P8b, design D8).
 *
 * Two of them, and the split mirrors the audio side exactly:
 *
 *   `add-midi-take`          the take primitive - `add-take` for events.
 *                            `add-take` is AUDIO-only by construction (it
 *                            addresses a FILE and seeds grain params), so this
 *                            is a NEW verb rather than a widened one.
 *   `place-midi-recording`   the PLANNER - `place-recording` for events. One
 *                            call per track per loop pass; it decides by
 *                            itself whether the pass lands on an existing
 *                            column (a take / an overdub / a replace) or
 *                            becomes a new clip.
 *
 * WHY THEY LIVE IN `objects/midi` AND NOT IN `objects/cut`. A take stack is
 * window-typed since proposal 37 D8b, so nothing about a column of takes is
 * audio-specific - but `STakeStack` itself lives in `objects/cut`, and
 * `objects/midi` sits at the SAME RANK and must not depend on it (the same
 * rule that keeps `objects/track` free of `objects/midi`). Both verbs
 * therefore reach the column through the generic seam on `SObject`
 * (`windowTakeCount` / `insertWindowTake` / `setActiveWindowTake` / …) and
 * through `SClipWindow`'s registered wrap factory. Nothing here names a stack.
 *
 * TICKS ARE THE DOMAIN, as everywhere in the event model (D2,
 * POSITION_DOMAINS rule 7). The `<n .../>` children of both verbs are in the
 * TARGET SEQUENCE's ticks for `add-midi-take` and in the PASS WINDOW's ticks
 * (zero = `timePos`) for `place-midi-recording`, which is what lets a recorder
 * emit one identical event table per loop pass and let the verb re-base it.
 */

/**
 * `add-midi-take` - a new EVENT take on a clip column.
 *
 *     <add-midi-take clip="0,0" index="-1" activate="1" name="Take 2"
 *                    lengthTicks="3840">
 *       <n tick="0" dur="480" key="60" velocity="100" channel="0"/>
 *     </add-midi-take>
 *
 * `clip` may address a plain `SMidiCut` placement - it is wrapped into a
 * column first, becoming take 0, exactly as `add-take` wraps a plain `SCut` -
 * or an existing column, in which case the take is inserted at `index`
 * (-1 appends). `lengthTicks` is the new SEQUENCE's musical length, and 0
 * derives it from the take's extent (or, with no extent, from the column's
 * duration) through the tempo map.
 *
 * ## `atTicks` / `extentTicks` - THE TAKE'S EXTENT (plan 53 §4.1, P3)
 *
 *     <add-midi-take clip="0,0" atTicks="3840" extentTicks="3840">…</add-midi-take>
 *
 * The event counterpart of `add-take at=`/`length=`, and DELIBERATELY NOT the
 * existing `lengthTicks=`, which is a different number: `lengthTicks` is how
 * long the SEQUENCE is, `extentTicks` is how much of the COLUMN this take
 * answers for. Before extents those two were forced to agree, because a take
 * had to cover the column (invariant 1); under 1' a take covers
 * `[atTicks, atTicks + extentTicks)` and the rest of the column is answered by
 * the other takes - which is the whole point of a recording that lands on a
 * lane that already has material.
 *
 * The defaults are "the whole column", so every call written before extents
 * means exactly what it meant: `atTicks` 0 and `extentTicks` absent give a
 * take whose window is the column's duration.
 *
 * TICKS, NOT FRAMES, AND THAT IS DECISION D6 (a). The conversion happens once
 * here, through THE tempo map (POSITION_DOMAINS rule 7), and the STORAGE is
 * musical from then on without anyone maintaining it: a take's link inside a
 * column is born `timebase=beats` for event content
 * (`SLink::defaultTimebaseFor`), so `set-tempo`'s `rederiveBeatsLinks` moves
 * the extent start with the bar, and the window's length is `lengthTicks_` on
 * the `SMidiCut` itself. That is why the extent is set through
 * `setWindowTicks` rather than through the frame-valued
 * `setDurationFromTimeline`: a round trip ticks -> frames -> ticks is exact
 * only at tempi where a tick divides a frame, and 3840 ticks must stay 3840
 * ticks at 143 BPM too.
 *
 * A column of AUDIO takes REFUSES an event take (`STakeStack::insertTake`,
 * homogeneity), and this verb turns that into a rejected action rather than a
 * column that plays notes or audio depending on which lane is active.
 *
 * Inverse: `remove-midi-take` at the inserted index, which captures the take's
 * events AND ITS EXTENT when it runs so ITS inverse restores them.
 * `SRemoveTakeAction` cannot serve here - it builds an `add-take` from an
 * `SExternFile` path, and an event take has no file, so undoing one would be a
 * non-undoable removal.
 */
class SAddMidiTakeAction : public SAction
{
public:
    SAddMidiTakeAction() = default;
    SAddMidiTakeAction( const QList<int> &clipPath, std::vector<SEvent> events,
                        qint64 lengthTicks, const QString &name,
                        int index = -1, bool activate = true,
                        qint64 atTicks = 0, qint64 extentTicks = -1 );

    QString name() const override { return QStringLiteral( "add-midi-take" ); }
    QStringList knownAttributes() const override
    { return { QStringLiteral( "clip" ), QStringLiteral( "index" ),
               QStringLiteral( "activate" ), QStringLiteral( "name" ),
               QStringLiteral( "lengthTicks" ), QStringLiteral( "atTicks" ),
               QStringLiteral( "extentTicks" ) }; }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QList<int>          clipPath_;
    std::vector<SEvent> events_;
    qint64              lengthTicks_ = 0;
    QString             takeName_;
    int                 index_ = -1;
    bool                activate_ = true;
    // The take's extent, in TICKS. The defaults are "the whole column", which
    // is what every caller written before plan 53 §4.1 means.
    qint64              atTicks_ = 0;
    qint64              extentTicks_ = -1;
};

/**
 * `add-midi-take`'s inverse. Not registered as a verb - created live, like
 * `remove-midi-clip`, because its own inverse carries the take's whole event
 * table and an XML-constructed instance could not have one.
 */
class SRemoveMidiTakeAction : public SAction
{
public:
    SRemoveMidiTakeAction( const QList<int> &columnPath, int takeIndex,
                           int thenActivate );


    QString name() const override
    { return QStringLiteral( "remove-midi-take" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QList<int> columnPath_;
    int        takeIndex_ = 0;
    int        thenActivate_ = -2;   // -2 = leave the selection alone
};

/**
 * `place-midi-recording` - place one recorded PASS on a track (design D8).
 *
 *     <place-midi-recording trackPath="0" timePos="48000" durationTicks="3840"
 *                           mode="new-take" quantize="1/16" name="MIDI">
 *       <n tick="0" dur="480" key="60" velocity="100" channel="0"/>
 *     </place-midi-recording>
 *
 * It is the planner shape of `place-recording`: the caller says WHERE the pass
 * was and WHAT was played, and the verb decides what that means against the
 * lane it lands on.
 *
 *   - an EVENT column overlapping the pass window gets the pass, in the way
 *     `mode` says (below);
 *   - nothing overlapping ⇒ `insert-midi-clip` at `timePos`, `durationTicks`
 *     long, carrying the events. The link is born `timebase=beats` like every
 *     event clip, so a later tempo edit moves the take with the bar.
 *
 * `mode` is the app-wide record mode, resolved per armed lane by the RECORDER
 * (`srecmode::resolveMode`) and passed in rather than read from the settings
 * HERE, so a scripted call is reproducible.
 *
 * ## THE FOUR MODES, AND WHAT EACH MEANS FOR EVENTS (plan 53 §3.3, P3)
 *
 * The vocabulary is plan 53's, which is what `SProjectProps::RecordMode`
 * stores and `srecplan::Mode` parses. `new-take` is accepted as a synonym for
 * `takes`: it is proposal 21 L4's spelling for the same intention, and a
 * `.qxa` or a saved action that uses it keeps working.
 *
 *   takes        (default) ONE UNION COLUMN per pass: every event clip or
 *   `new-take`   column the pass touches, and the pass itself, become takes of
 *                one column spanning all of them, with the pass active over
 *                its own bars and what was there before answering everywhere
 *                else. This goes through `objects/recplan`'s planner -- the
 *                SAME planner the audio side uses -- so the geometry, the
 *                operand order and the index bookkeeping have one
 *                implementation for both kinds.
 *   takes-split  the pass is an ALTERNATIVE FOR EXACTLY THE BARS IT COVERS:
 *                each touched clip is cut at the pass's edges, the inside
 *                piece gets the pass as a second take, and the bars no clip
 *                covered become clips of their own. Also the shared planner.
 *   overdub      the pass is MERGED into the active take of the first touched
 *                event column. The events already there survive; this is how a
 *                hi-hat is added to a kick-and-snare pattern.
 *   replace      the notes already in the PASS WINDOW are dropped and the pass
 *                replaces them; events outside the window - and every non-note
 *                event anywhere - survive untouched.
 *
 * `overdub` and `replace` KEEP THEIR OWN EVENT MEANINGS and do not go through
 * the planner, which §3.3 states for both in as many words. The audio
 * planners mean something else by the same two words -- `overdub` lays a plain
 * clip on top and lets both sound, `replace` is tape and destroys what the
 * pass covers -- and for notes those are not the useful operations: notes
 * SUM by nature, so "both sound" needs no second clip, and a note-level
 * replace that keeps the CCs is finer than deleting the clip. The audio
 * meanings are in `srecordplanner.h` beside each planner.
 *
 * `quantize` ("off" | "1/4" | "1/8" | "1/16" | "1/8t" | "1/16t" | …) appends a
 * `quantize-notes` INSIDE this verb's own composite, so input quantise is one
 * step of the same undo entry as the placement rather than a second one the
 * user has to undo separately. It quantises the CLIP, which after a merge is
 * exactly the notes that are now there.
 *
 * The whole verb is ONE composite, so its inverse undoes the placement, the
 * merge and the quantise together.
 */
class SPlaceMidiRecordingAction : public SAction
{
public:
    SPlaceMidiRecordingAction() = default;
    ~SPlaceMidiRecordingAction() override;
    SPlaceMidiRecordingAction( const QList<int> &trackPath, offset_t timePos,
                               qint64 durationTicks, std::vector<SEvent> events,
                               const QString &mode, const QString &quantize,
                               const QString &name );

    QString name() const override
    { return QStringLiteral( "place-midi-recording" ); }
    QStringList knownAttributes() const override
    { return { QStringLiteral( "trackPath" ), QStringLiteral( "timePos" ),
               QStringLiteral( "durationTicks" ), QStringLiteral( "mode" ),
               QStringLiteral( "quantize" ), QStringLiteral( "name" ) }; }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

    /**
     * `takes` | `takes-split` | `overdub` | `replace`, plus `new-take` as the
     * historical synonym of `takes`.
     */
    static bool isKnownMode( const QString &mode );
    /**
     * The plan-53 spelling of `mode`: `new-take` becomes `takes`, a known mode
     * is itself, and anything unknown becomes `takes` -- the same answer
     * `srecplan::modeFromString` gives, so a typo cannot reach a planner
     * nobody named.
     */
    static QString canonicalMode( const QString &mode );

private:
    QList<int>          trackPath_;
    offset_t            timePos_ = 0;
    qint64              durationTicks_ = 0;
    std::vector<SEvent> events_;
    QString             mode_ = QStringLiteral( "new-take" );
    QString             quantize_ = QStringLiteral( "off" );
    QString             clipName_;
    /**
     * THE PLAN, HELD, for the reason `SPlaceRecordingAction::plan_` is held: a
     * child's inverse may BORROW the child. `merge-columns`' inverse is an
     * `unmerge-columns` that reads its own operand recipe back out of the
     * merge action, so the children have to outlive the inverse this returns,
     * and `SActionUndoCommand` keeps this action for exactly that long (it
     * owns the forward and the inverse together and destroys them together).
     *
     * A STACK-LOCAL COMPOSITE WAS CORRECT UNTIL P3 AND IS NOT ANY MORE. Every
     * child this verb used to emit built a self-contained inverse, so the
     * composite could die with `apply()`. Routing `takes` through the planner
     * put `merge-columns` in the plan, and the first `undo` of a MIDI
     * recording then read a destroyed action: measured as a SEGFAULT in
     * `SUnmergeColumnsAction::apply` -> `containerAt` -> `resolveByPath`,
     * reading `owner_->lanePath()` of a freed `SMergeColumnsAction`.
     *
     * Not a second source of truth: nothing reads it back, it is rebuilt on
     * every apply (a redo re-plans against the lane as the undo left it), and
     * the previous one is deleted there.
     */
    SCompositeAction   *plan_ = nullptr;
};

#endif // _SMIDIRECORDACTIONS_H_
