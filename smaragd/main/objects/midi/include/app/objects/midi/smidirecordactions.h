#ifndef _SMIDIRECORDACTIONS_H_
#define _SMIDIRECORDACTIONS_H_

#include <QList>
#include <QString>
#include <vector>

#include "app/actions/saction.h"
#include "app/objects/midi/smidisequence.h"

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
 * (-1 appends). The new take's window is the COLUMN's duration (take-stack
 * invariant 1); `lengthTicks` is the new SEQUENCE's musical length, and 0
 * derives it from the column's duration through the tempo map.
 *
 * A column of AUDIO takes REFUSES an event take (`STakeStack::insertTake`,
 * homogeneity), and this verb turns that into a rejected action rather than a
 * column that plays notes or audio depending on which lane is active.
 *
 * Inverse: `remove-midi-take` at the inserted index, which captures the take's
 * events when it runs so ITS inverse restores them. `SRemoveTakeAction` cannot
 * serve here - it builds an `add-take` from an `SExternFile` path, and an
 * event take has no file, so undoing one would be a non-undoable removal.
 */
class SAddMidiTakeAction : public SAction
{
public:
    SAddMidiTakeAction() = default;
    SAddMidiTakeAction( const QList<int> &clipPath, std::vector<SEvent> events,
                        qint64 lengthTicks, const QString &name,
                        int index = -1, bool activate = true );

    QString name() const override { return QStringLiteral( "add-midi-take" ); }
    QStringList knownAttributes() const override
    { return { QStringLiteral( "clip" ), QStringLiteral( "index" ),
               QStringLiteral( "activate" ), QStringLiteral( "name" ),
               QStringLiteral( "lengthTicks" ) }; }
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
 * `mode` is the app-wide input-record mode, passed in rather than read from
 * the settings HERE so a scripted call is reproducible:
 *
 *   new-take   (default) the pass becomes a NEW take on the column, activated.
 *              What "record over a part again" means in every reference DAW.
 *   overdub    the pass is MERGED into the column's active take. The events
 *              already there survive; this is how a hi-hat is added to a
 *              kick-and-snare pattern.
 *   replace    the notes already in the PASS WINDOW are dropped and the pass
 *              replaces them; events outside the window - and every non-note
 *              event anywhere - survive untouched.
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

    /** "new-take" | "overdub" | "replace"; anything else falls back to the first. */
    static bool isKnownMode( const QString &mode );

private:
    QList<int>          trackPath_;
    offset_t            timePos_ = 0;
    qint64              durationTicks_ = 0;
    std::vector<SEvent> events_;
    QString             mode_ = QStringLiteral( "new-take" );
    QString             quantize_ = QStringLiteral( "off" );
    QString             clipName_;
};

#endif // _SMIDIRECORDACTIONS_H_
