#ifndef _SMIDIINTESTACTIONS_H_
#define _SMIDIINTESTACTIONS_H_

#include <QString>

#include "app/actions/saction.h"

/**
 * Headless MIDI INPUT (proposal 21 L0, design D9).
 *
 * The counterpart of the MIDI-out verbs, and deliberately much thinner: there
 * is no new device layer here, because the one that was needed already exists.
 * `CaptureMidiInput` (proposal 37 P7a) is a real `MidiInput` whose `inject()`
 * delivers bytes to the port's callback exactly as a driver would, on the
 * caller's thread, with the caller's host-time stamp — and
 * `SMARAGD_MIDI_BACKEND=capture` already selects it, already by default under
 * `--test-case`. These two verbs are the script's hand on that port.
 *
 * WHERE THE EVENTS GO. Nothing in the app consumes a `MidiInput` yet (proposal
 * 37 recorded that as known debt; proposal 21 L1a/L3a are what attach the live
 * lane and the MIDI recorder to it). So today an injected event reaches the
 * port and stops there — the verbs are gated on their own behaviour (order,
 * stamps, pacing), not on anything sounding. They exist NOW because the L1
 * phases need a way to feed input that does not involve a human with a
 * keyboard, and because a verb added later would have no way to prove it did
 * not change the timing of the cases written against it.
 *
 * THE PORT THE VERBS USE is `CaptureMidiInput::active()` — the live instance,
 * whichever component constructed it. When nothing has (which is the case
 * today), the first verb call mints a process-lifetime one and opens it, so a
 * script never depends on a consumer existing yet; once the app opens its own,
 * that one is `active()` and the verbs use it instead.
 */

// midi-in-event — inject ONE message.
//
//   kind      noteon | noteoff | cc | pitchbend | programchange |
//             channelpressure | polypressure          (default noteon)
//   key       note number / poly-pressure key / CC number when `cc` is absent
//   velocity  the message's last data byte: velocity, CC value, program …
//   channel   0-BASED, matching add-note channel= and the MIDI-out verbs
//   cc        controller number for kind=cc (overrides key)
//   bytes     "90 3C 64" — raw hex, INSTEAD of the fields above. Whatever the
//             script needs to send that the field form cannot spell.
//   atFrame   optional. The project frame to inject AT; valid ONLY while the
//             transport is PLAYING, because a frame is a position on a moving
//             playhead and there is no such thing while it is stopped (design
//             D2). Rejected — not ignored — when stopped.
//   timeoutMs how long atFrame may wait for the playhead (default 10000).
//
// atFrame is resolved by WAITING for the playhead to reach it and injecting
// then. The design's "map through the engine's delivered-frame clock" needs
// that clock, and it is L1a's deliverable (the engine-owned {seq,
// deliveredFrame, hostNs} atomic stamped in twSpeaker's callback); until it
// exists the app's published locator is the only frame clock there is, and it
// is the same one wait-playhead and the meters read. Accuracy today is
// therefore the locator's publication granularity, not a sample — a case that
// needs better than that should assert on the CAPTURED result, not on the
// injection.
class SMidiInEventAction : public SAction
{
public:
    SMidiInEventAction() = default;

    QString name() const override { return QStringLiteral( "midi-in-event" ); }
    QStringList knownAttributes() const override;
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString kind_ = QStringLiteral( "noteon" );
    int     key_      = 60;
    int     velocity_ = 100;
    int     channel_  = 0;
    int     cc_       = -1;
    QString bytes_;
    bool    hasAtFrame_ = false;
    qint64  atFrame_    = 0;
    int     timeoutMs_  = 10000;
};

// midi-in-replay — inject a whole performance, REAL-TIME PACED.
//
//   filePath   a .mid (read with twSmf) or a text log. The extension decides:
//              ".mid" / ".midi" is a Standard MIDI File, anything else is the
//              text form
//                  # comment
//                  0.0    90 3C 64      <- offset in MILLISECONDS, then bytes
//                  250.0  80 3C 00
//              which is what a captured log turns into, and what a case that
//              wants one awkward message writes by hand.
//   track      SMF track index (default -1 = every track merged)
//   channel    0-based override for every message (default -1 = as authored)
//   startFrame optional. Hold until the playhead reaches this project frame,
//              then start the performance. Requires the transport to be
//              PLAYING, for the same reason midi-in-event's atFrame does.
//   timeoutMs  how long startFrame may wait (default 10000).
//
// Pacing is on MidiOutScheduler::hostNowNs() — the ONE steady clock the audio
// capture backend stamps its blocks with — so a case can map what it hears
// back onto what it sent through CaptureBackend::frameAtHostTime, the same
// independent-measurement arrangement the MIDI-out cases use. The event loop
// is pumped while waiting, because everything a case is testing (the locator,
// the meters, the pumps) only runs when events are processed.
//
// A .mid's own timing is used: its PPQ and its tempo metas (500000 µs/quarter
// until one says otherwise), NOT the project tempo. A performance file
// describes a performance; re-timing it against the project would make the
// same file mean different things in two cases.
class SMidiInReplayAction : public SAction
{
public:
    SMidiInReplayAction() = default;

    QString name() const override { return QStringLiteral( "midi-in-replay" ); }
    QStringList knownAttributes() const override;
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString filePath_;
    int     track_   = -1;
    int     channel_ = -1;
    bool    hasStartFrame_ = false;
    qint64  startFrame_    = 0;
    int     timeoutMs_     = 10000;
};

#endif // _SMIDIINTESTACTIONS_H_
