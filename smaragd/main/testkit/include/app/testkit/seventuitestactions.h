#ifndef _SEVENTUITESTACTIONS_H_
#define _SEVENTUITESTACTIONS_H_

#include <QString>

#include "app/actions/saction.h"

/**
 * The event-editor test verbs (proposal 37 P4, design 6.4).
 *
 * All four go through the SHELL, exactly as `drag-clip-edge` and `assert-meter`
 * do: testkit may not include app/eventui or app/timeline
 * (tools/check_layering.py; testkit CONTRACT inv. 5), and the point of these
 * verbs is that the REAL widget runs - a script that poked the model instead
 * would test the script.
 */

/**
 * `virtual-key` - one press on the virtual keyboard.
 *
 * TWO MODES, because the keyboard does two things and a case needs to address
 * them separately (proposal 21 L2, design D9):
 *
 *   (default)              WRITE a note at the LOCATOR through `add-note`.
 *                          The target is the first event clip in the SELECTION,
 *                          else the first one on the SELECTED TRACK. With
 *                          neither, the verb FAILS rather than doing nothing
 *                          quietly - a case asserts that with `expectReject`.
 *                          The note lands as a nested `add-note` action, so
 *                          `<undo count="1"/>` after this verb removes it.
 *
 *   hold="1"               PLAY it: a note-on on the computer keyboard's
 *                          in-process MIDI PORT, which a live-armed instrument
 *                          track hears exactly as it hears hardware. Nothing is
 *                          written and nothing lands on the undo stack.
 *   release="1"            the matching note-off. `key="-1"` releases every key
 *                          the dock is holding.
 *   durationMs="N"         with hold: send the note-off N ms later, pacing on
 *                          the ONE steady clock (MidiOutScheduler::hostNowNs)
 *                          and PUMPING the event loop while it waits - every
 *                          clock a case cares about only advances when events
 *                          are processed.
 *
 * The two modes are deliberately not folded into one "do both": a case that is
 * measuring what an instrument SOUNDS must not also be editing the project
 * under the measurement, while the real mouse handler does both precisely
 * because a user pressing a key means both.
 */
class SVirtualKeyAction : public SAction
{
public:
    SVirtualKeyAction() = default;

    QString name() const override { return QStringLiteral( "virtual-key" ); }
    QStringList knownAttributes() const override
    { return { QStringLiteral( "key" ), QStringLiteral( "velocity" ),
               QStringLiteral( "durationTicks" ), QStringLiteral( "hold" ),
               QStringLiteral( "release" ), QStringLiteral( "durationMs" ) }; }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    int    key_ = 60;
    double velocity_ = 100.0;
    qint64 durationTicks_ = 960;
    bool   hold_ = false;
    bool   release_ = false;
    int    durationMs_ = 0;
};

/**
 * `drag-note` - the `drag-clip-edge` twin for the piano roll. It works out
 * where the addressed note is ON SCREEN and sends real press / move / release
 * events to the view, so the gesture arithmetic under test is the one a
 * pointer drives.
 *
 *   lane="notes" (default)  : move the note to (toTick, toKey)
 *   lane="notes" edge="end" : resize it so it ENDS at toTick
 *   lane="velocity"         : drag its velocity-lane bar to toValue (1..127)
 *
 * The drop is PIXEL-QUANTISED and then snapped to the editor's grid, so a case
 * must assert on a range or on a grid position, never on an exact arbitrary
 * tick - the same caveat drag-clip-edge carries.
 *
 * The gesture submits its own `set-notes` on release; `<undo count="1"/>` after
 * it reverses the drag.
 */
class SDragNoteAction : public SAction
{
public:
    SDragNoteAction() = default;

    QString name() const override { return QStringLiteral( "drag-note" ); }
    QStringList knownAttributes() const override;
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString clip_;
    qint64  tick_ = 0;
    int     key_ = 60;
    int     channel_ = -1;
    qint64  toTick_ = 0;
    int     toKey_ = -1;
    QString edge_;                              // "" | "end"
    QString lane_ = QStringLiteral( "notes" );  // "notes" | "velocity"
    double  toValue_ = -1.0;
};

/**
 * `scroll-event-editor-keys` - drives the piano roll's REAL vertical key
 * scrollbar (SPianoRollView::tkScrollKeys -> QScrollBar::setValue), so a case
 * can prove the full 0-127 MIDI pitch range is actually reachable through the
 * widget under test, not merely through the clamp arithmetic in isolation.
 *
 * `topKey` is the key requested at the TOP of the grid; it is clamped into
 * the currently reachable range exactly as dragging the scrollbar handle to
 * either end would clamp. Follow with `assert-event-editor` (no `clip=`, so
 * it re-binds to whatever this call left selected/bound) and check its
 * `topKey=`/`botKey=` fields - `botKey=0` after `topKey="0"` and
 * `topKey=127` after `topKey="200"` (out of range, clamped at the top) is
 * what proves the full range.
 *
 * Not undoable: a scroll position is view state, exactly like the arranger's
 * own zoom/scroll.
 *
 * XML format:
 * <scroll-event-editor-keys clip="0,0" topKey="0"/>
 */
class SScrollEventEditorKeysAction : public SAction
{
public:
    SScrollEventEditorKeysAction() = default;

    QString name() const override
    { return QStringLiteral( "scroll-event-editor-keys" ); }
    QStringList knownAttributes() const override
    { return { QStringLiteral( "clip" ), QStringLiteral( "topKey" ) }; }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString clip_;
    int     topKey_ = 0;
};

/**
 * `assert-event-editor` - what the REAL dock, built in the main window and
 * never shown, reports for a clip.
 *
 * `describe()` reads
 *
 *   kind=…|notes=N|grid=…|linked=0/1|empty=0/1|selection=N|<view fields>
 *
 * and `contains=` matches a CONTIGUOUS substring of it, which is why the field
 * order is fixed. `grabPng` paints the dock into the test output directory:
 * coverage of the paint paths, never an oracle.
 */
class SAssertEventEditorAction : public SAction
{
public:
    SAssertEventEditorAction() = default;

    QString name() const override
    { return QStringLiteral( "assert-event-editor" ); }
    QStringList knownAttributes() const override;
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString clip_;
    QString kind_ = QStringLiteral( "pianoroll" );
    QString contains_;
    QString absent_;
    QString grabPng_;
    int     grabWidth_ = 720;
    int     grabHeight_ = 360;
};

/**
 * `assert-track-head` - the track head's DENSITY rules, through the real
 * widget built off screen (SMainWindow::describeTrackHead).
 *
 * `describe()` reads
 *
 *   density=…|w=…|h=…|btns=M,S,R,T,G,A|I=0/1|A=0/1|fitW=…|fitH=…|name=…
 *
 * `fitW`/`fitH` are the "hiding beats clipping" rule made assertable: the
 * VISIBLE strip must fit the lane height it was given.
 *
 * Since proposal 37 P6 the string ends `|Amode=<off|trim|read|touch|latch|
 * write>` - the track's automation MODE, reported at EVERY density, because
 * the "A" button hides on a short lane while the mode does not stop existing.
 * `grabPng` paints that same off-screen head into the test output directory:
 * coverage of the paint paths (the mode colour), never an oracle.
 */
class SAssertTrackHeadAction : public SAction
{
public:
    SAssertTrackHeadAction() = default;

    QString name() const override
    { return QStringLiteral( "assert-track-head" ); }
    QStringList knownAttributes() const override
    { return { QStringLiteral( "trackPath" ), QStringLiteral( "headHeight" ),
               QStringLiteral( "contains" ), QStringLiteral( "absent" ),
               QStringLiteral( "grabPng" ), QStringLiteral( "grabWidth" ),
               QStringLiteral( "grabHeight" ) }; }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString trackPath_ = QStringLiteral( "0" );
    int     headHeight_ = 160;
    QString contains_;
    QString absent_;
    QString grabPng_;
    int     grabWidth_ = 0;
    int     grabHeight_ = 0;
};

#endif // _SEVENTUITESTACTIONS_H_
