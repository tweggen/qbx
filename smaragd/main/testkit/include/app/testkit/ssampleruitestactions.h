#ifndef SSAMPLERUITESTACTIONS_H
#define SSAMPLERUITESTACTIONS_H

#include "app/actions/saction.h"

#include <QString>
#include <QStringList>

// The NassauDigital editor's test verbs (proposal 56 Q7, QBX-279). The editor is
// widgets, and no headless case can look at a widget, so these drive the REAL
// window off screen and read back what it shows (the arranger-menu precedent:
// a trigger verb and a describe verb over the same object). Nothing here
// asserts a rectangle or a grab: sizes are asserted as CONSTRAINTS (minimum ==
// maximum), which need no mapped window.
//
// sampler-editor — perform one gesture on the editor of `trackPath`/`slotIndex`:
//   action = open           through pluginui's app-editor registry (the FX
//                           strip's Edit goes through plugin-row-command)
//          | close
//          | view           click the view button `view` (key|pad|map|zone)
//          | bank           click bank button `bank` (0..5)
//          | pad            press pad cell `pad` (0..15) of the Pad view
//          | key            press `key` on the Key view
//          | map            press (`key`, `vel`) on the Map view
//          | knob           press knob `knob` ("zone.cutoff", "layer1.level"),
//                           move it through `values` ("a;b;c", parameter
//                           units, C locale), release (`press`="0" skips the
//                           press and release: a wheel or a keyboard step)
//          | xml-type       put `text` in the XML pane, cursor at `cursor`
//                           (-1 = the end), run the completion the typing runs
//          | xml-accept     accept the offered completion `choice`
//          | xml-apply      press Apply
//          | drop           drop `payload` on `target` ("key:<k>", "pad:<p>",
//                           "map:<k>:<v>", "zone")
//   expect = "1"            what the gesture must return ("0": a refusal)
// REJECTED when there is no such slot or (for every action but open) no open
// editor. Not undoable itself: what the gesture submits is.
class SSamplerEditorAction : public SAction {
public:
    QString name() const override { return QStringLiteral( "sampler-editor" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override;

private:
    QString trackPath_ = QStringLiteral( "0" );
    int     slotIndex_ = 0;
    QString action_;
    QString view_;
    int     bank_ = -1;
    int     pad_  = -1;
    int     key_  = -1;
    int     vel_  = -1;
    QString knob_;
    QString values_;
    int     press_  = 1;
    QString text_;
    int     cursor_ = -1;
    QString choice_;
    QString payload_;
    QString target_;
    int     expect_ = 1;
};

// assert-sampler-editor — read the editor of `trackPath`/`slotIndex`:
//   open = "1" | "0"     an editor is (not) registered for the slot
//   flag = "1" | "0"     the slot's saved editorOpen flag
//   contains, absent     substrings of SSamplerEditor::describe() (needs an
//                        open editor)
// An element that checks nothing is REJECTED.
class SAssertSamplerEditorAction : public SAction {
public:
    QString name() const override { return QStringLiteral( "assert-sampler-editor" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override;

private:
    QString trackPath_ = QStringLiteral( "0" );
    int     slotIndex_ = 0;
    int     open_ = -1;
    int     flag_ = -1;
    QString contains_;
    QString absent_;
};

// sampler-hold-keys — Q7.2's injected MIDI. Injects a note-on for every key of
// `keys` ("40;52"), then a note-off for every key of `release`, into the
// CAPTURE MIDI INPUT (the device input a --test-case run opens; midi-in-event's
// port), so the message reaches EVERY sink of that port's fan-out — the
// editor's own tap among them, the engine's live source untouched — and then
// drains the editor's tap at once (SSamplerEditor::pollTap), so the case needs
// no wait. `velocity` = "100", `channel` = "0" (0-based). REJECTED unless an
// editor is open on the slot (the tap exists only while it is) and the capture
// backend is the MIDI backend.
class SSamplerHoldKeysAction : public SAction {
public:
    QString name() const override { return QStringLiteral( "sampler-hold-keys" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override;

private:
    QString trackPath_ = QStringLiteral( "0" );
    int     slotIndex_ = 0;
    QString keys_;
    QString release_;
    int     velocity_ = 100;
    int     channel_  = 0;
};

#endif
