#ifndef _SMIDIOUTTESTACTIONS_H_
#define _SMIDIOUTTESTACTIONS_H_

#include <QString>

#include "app/actions/saction.h"

/**
 * Headless coverage for MIDI OUT (proposal 37 D6 / P7b).
 *
 * THE MEASUREMENT IS INDEPENDENT OF THE THING MEASURED, and that is the whole
 * point of the arrangement (design review #12). Two recorders, written by two
 * threads that know nothing of each other:
 *
 *   - the CAPTURE MIDI port records `{hostTimeNs, port, bytes}` and nothing
 *     else - specifically NOT the due time it was asked for, because the
 *     difference between the two IS the measurement;
 *   - the AUDIO capture backend records `{hostTimeNs, firstFrame}` for every
 *     block it hands to the "device" (CaptureBackend::frameAtHostTime,
 *     piecewise linear over that log).
 *
 * `assert-midi-out` maps a message's host time through the AUDIO log, so what
 * it asserts is "the pump agreed with the audio clock", never "the pump agreed
 * with the pump". Both logs are stamped with MidiOutScheduler::hostNowNs() -
 * one steady clock - which is what makes the two comparable at all.
 *
 * `at` is therefore in PROJECT FRAMES SINCE PLAYBACK STARTED, and it is
 * SIGNED: the audio recording begins at the transport's start position, so an
 * event with a positive send offset legitimately lands BEFORE frame 0. Two
 * corrections are folded in, both of them properties of the DEVICE rather than
 * of the pump:
 *
 *   at = frameAtHostTime(sentAt) - deviceOutputLatencyFrames
 *
 * i.e. "the project frame whose audio was being HEARD at the instant this
 * message left for the wire", which is exactly the alignment D6 asks the pump
 * to produce.
 *
 * `SMARAGD_CAPTURE_SPEED` must be 1 for any of this: the audio log stays
 * empirically correct at other speeds, but a project frame then means
 * something different in wall-clock terms while the MIDI due times do not, so
 * the two clocks stop describing the same world.
 */

// assert-midi-out - what actually reached the wire, and when.
//
//   port      = ""     only messages sent to this port id
//   kind      = ""     noteon | noteoff | cc | pitchbend | programchange |
//                      channelpressure | polypressure | any ("" = any)
//   channel   = "-1"   0-BASED status nibble, matching add-note channel= and
//                      set-track-midi-output channel=
//   key       = "-1"   note number (noteon/noteoff/polypressure)
//   cc        = "-1"   controller number (kind=cc)
//   value     = "-1"   the message's LAST data byte: velocity for a note, the
//                      controller value for a CC, the program for a PC
//   at        = ""     project frames since playback started; when given, at
//                      least one matching message must land within tolerance
//   tolerance = "4096" frames of slack around `at` (~85 ms at 48 kHz)
//   count / minCount / maxCount   how many messages matched (-1 = not checked)
//   index     = "-1"   if >= 0, the FIRST match must be message number `index`
//                      in the recording - the only way to assert ORDER across
//                      kinds ("the chase CC came before the note-on")
//
// REJECTS when the MIDI backend is not `capture` (AC5): there is nothing to
// read, and silently passing would make every MIDI-out case vacuous the moment
// someone set SMARAGD_MIDI_BACKEND by hand.
class SAssertMidiOutAction : public SAction
{
public:
    SAssertMidiOutAction() = default;

    QString name() const override { return QStringLiteral( "assert-midi-out" ); }
    QStringList knownAttributes() const override;
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString port_;
    QString kind_;
    int     channel_ = -1;
    int     key_     = -1;
    int     cc_      = -1;
    int     value_   = -1;
    bool    hasAt_   = false;
    qint64  at_      = 0;
    qint64  tolerance_ = 4096;
    int     count_    = -1;
    int     minCount_ = -1;
    int     maxCount_ = -1;
    int     index_    = -1;
    // MIDI-THRU (proposal 21 L2 AC5). `maxLagMs` bounds how long after the
    // LAST `midi-in-event` injection the matched messages reached the wire, in
    // MILLISECONDS, host time to host time - no frame mapping at all, because
    // thru has no position: it is a key the performer is pressing right now.
    // The measured worst lag is printed on success as well as on failure.
    double  maxLagMs_ = -1.0;
};

// dump-midi-capture - write the capture MIDI recording out as text.
//
// The MIDI twin of dump-playback-capture, and used the same way: it is a
// DIAGNOSTIC, not an oracle. One line per message with its host time, the
// project frame it maps to through the audio clock, the port, the raw bytes
// and a decoded form, so a failing assertion can be read rather than guessed
// at. Written into the test output directory and nowhere else.
class SDumpMidiCaptureAction : public SAction
{
public:
    SDumpMidiCaptureAction() = default;

    QString name() const override { return QStringLiteral( "dump-midi-capture" ); }
    QStringList knownAttributes() const override;
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString filename_;
    int     minCount_ = 0;
};

// assert-midi-options - the Options -> MIDI page, built off screen.
//
// The house pattern for a page made of widgets: construct the REAL
// SOptionsDialog (never shown - a qxa run on Windows uses the real platform
// plugin, so a shown dialog would appear on the developer's screen), and match
// against SOptionsDialog::describeMidiPage(). PNG grabs would be coverage;
// this is the oracle.
//
//   contains / absent   substrings of the describe() dump
//   outputPorts / inputPorts   exact counts (-1 = not checked)
class SAssertMidiOptionsAction : public SAction
{
public:
    SAssertMidiOptionsAction() = default;

    QString name() const override
    { return QStringLiteral( "assert-midi-options" ); }
    QStringList knownAttributes() const override;
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString contains_;
    QString absent_;
    int     outputPorts_ = -1;
    int     inputPorts_  = -1;
};

// set-option - write one per-user SSettings key.
//
// The SSettings twin of `set-property` (which writes a per-PROJECT property).
// It exists because two of the MIDI-out behaviours D6 specifies are per-user
// options rather than project state - the note-on chase and the global send
// offset - and a case that cannot set them cannot gate them.
//
// NOTE it writes the developer's real smaragd.ini, exactly as the Options
// dialog would. A case that changes a value must set it back, and only one
// case in the suite may own any given key.
class SSetOptionAction : public SAction
{
public:
    SSetOptionAction() = default;

    QString name() const override { return QStringLiteral( "set-option" ); }
    QStringList knownAttributes() const override
    { return { QStringLiteral( "key" ), QStringLiteral( "value" ) }; }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString key_;
    QString value_;
};

// close-options-dialog - drive the REAL SOptionsDialog's close path (AC-b1),
// off screen, the house pattern once again.
//
// There is no verb anywhere in this repo that can click a QTreeWidget item
// or drive a modal QDialog's own event loop, so this is a deliberate stand-in
// for "the user navigated to `page`, then closed the dialog": it constructs
// SOptionsDialog(nullptr, page) -- the ctor argument standing in for the
// click -- then closes it through `result` ("ok" default, or "cancel"),
// which is what actually exercises SOptionsDialog::done() and persists
// `SOpt::OptionsLastPage`. Read the persisted value back with
// `assert-settings-file contains="optionsLastPage=<name>"` (the key's own
// section is "ui", per SSettings' "first segment is the [section]" rule).
//
// Closed through a QDialog* -- SOptionsDialog::done() is a PRIVATE override,
// but QDialog::done() is public, and C++ access control for a virtual call
// is checked against the STATIC type used to make it: through a QDialog*
// the call is well-formed and still dispatches to the override at runtime.
class SCloseOptionsDialogAction : public SAction
{
public:
    SCloseOptionsDialogAction() = default;

    QString name() const override
    { return QStringLiteral( "close-options-dialog" ); }
    QStringList knownAttributes() const override
    { return { QStringLiteral( "page" ), QStringLiteral( "result" ) }; }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    // -1 = omit the ctor argument entirely (opens on whatever is currently
    // remembered, or page 0 if nothing is).
    int     page_   = -1;
    QString result_ = QStringLiteral( "ok" );   // "ok" | "cancel"
};

// wait-ms - pump the event loop for a wall-clock duration.
//
// `wait-playhead` covers "playback got somewhere", which is the right verb
// almost everywhere. It cannot express "let the transport run for N seconds"
// under a CYCLE, though: the locator wraps, so neither the relative nor the
// absolute form has a target that means "two passes". MIDI-out cases also need
// the transport to keep running after the last note so the pump's stop
// behaviour has something to be asserted about.
class SWaitMsAction : public SAction
{
public:
    SWaitMsAction() = default;

    QString name() const override { return QStringLiteral( "wait-ms" ); }
    QStringList knownAttributes() const override
    { return { QStringLiteral( "ms" ) }; }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    int ms_ = 0;
};

#endif // _SMIDIOUTTESTACTIONS_H_
