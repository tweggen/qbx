#ifndef _SMIDIRECORDER_H_
#define _SMIDIRECORDER_H_

#include <cstdint>
#include <vector>

#include <QList>
#include <QObject>
#include <QString>

#include "app/shell/splayheadclock.h"
#include "tw/core/twtypes.h"
#include "tw/devices/midi_in_fanout.h"

class QTimer;
class SApplication;
class STrack;

/**
 * THE MIDI RECORDER (proposal 21 L4 = 37 P8b, design D6/D8/D9).
 *
 * A sibling of `SAudioRecorder`, `SAutomationRecorder`, `SMidiOutPump` and
 * `SLiveMonitor`: one per `SApplication`, MAIN THREAD ONLY, owning a `QTimer`.
 * It is what a record start MEANS for a MIDI-armed track, and it is a
 * consumer of the input fan-out exactly the way `SAutomationRecorder` is a
 * consumer of a control's value stream - bounded by the TRANSPORT, committing
 * ONE undo entry per pass at the stop.
 *
 * THE SPLIT WITH `SAudioRecorder` IS BY TRACK INPUT, not by two record
 * buttons: a record start collects the armed tracks and gives the ones whose
 * `trackInput` is `midi:`/`keyboard` to this class and the rest to the audio
 * recorder. So a project with an armed guitar and an armed synth part records
 * both in one pass, and neither recorder ever sees a track it cannot handle.
 * Exactly one of them starts the transport (`SApplication::startRecording`).
 *
 * WHAT HAPPENS, in order.
 *
 * START:
 *   1. collect the armed MIDI tracks (false, and nothing changed, when there
 *      are none - which is how the audio recorder gets a plain audio take);
 *   2. acquire the RECORDER SINK of each distinct port from `SMidiInputHub` -
 *      one SPSC ring per consumer at the device callback (design D8), never
 *      the live lane's - and DRAIN it, so a key pressed before the button does
 *      not appear inside the take;
 *   3. read the mode and the input quantise from `SOpt` ONCE, so a settings
 *      change mid-take cannot make the commit inconsistent with what was
 *      captured;
 *   4. begin the run on `SPlayheadClock` - the SAME clock `SMidiOutPump`
 *      schedules through, read backwards.
 *
 * TICK (20 ms, `SMidiOutPump`'s cadence and for the same reason - a 250 ms
 * lookahead there, a 1024-message ring here, both sized so a GUI hiccup costs
 * nothing): offer the playhead to the clock, then pop every port's ring into a
 * per-port buffer of `{hostTimeNs, bytes}`. NOTHING IS MAPPED HERE. The model
 * is not touched until the stop, which is what makes the recorder safe to run
 * beside the arranger and what makes the mapping RETROSPECTIVE:
 *
 *     projectFrame(msg) = clock.frameAtHostNs(msg.hostTimeNs)
 *                         - inputOffsetProj
 *
 * `frameAtHostNs` answers "which project frame was being HEARD at that
 * instant" - the anchor already carries the publish-lag correction and the
 * device output latency (`SPlayheadClock`) - which is design D6's derivation
 * verbatim: the performer plays to what they hear. `inputOffsetProj` is the
 * port's `midi/inputOffsetMs` (an `SSettings` key, L0), and its sign is the
 * app-wide one: POSITIVE means EARLIER, i.e. "this controller arrives late,
 * compensate more". A take begun from a stopped transport has NO anchor while
 * its first messages arrive (the readahead primes before the RT publishes),
 * and needs no special case: buffering the host times and mapping them once at
 * the stop IS the retrospective mapping, and backward extrapolation on a clock
 * that is linear in host time is exact.
 *
 * STOP: pair note-ons with note-offs into notes WITH LENGTHS (a note still
 * held is closed at the stop frame), convert each frame to TICKS through the
 * project's `twTempoMap` - once per value, POSITION_DOMAINS rule 7 - split the
 * take at the cycle boundaries by ARITHMETIC (`floor((f - loopIn)/loopLen)`,
 * never wrap detection: a 20 ms tick cannot see a wrap), and submit ONE undo
 * macro of `place-midi-recording` calls, one per track per pass.
 *
 * ALL-NOTES-OFF ON STOP IS DELIBERATELY NOT SENT FROM HERE. Closing the held
 * notes in the RECORDING is this class's half of it. The sounding half already
 * has two owners: `SMidiOutPump::stop()` panics every MIDI-out port it used,
 * and L2's `detachLiveEvents` flushes the live source's held-note table at
 * disarm. A third flush would be a duplicate all-notes-off on the user's
 * hardware, and the recorder is not the thing holding those notes.
 */
class SMidiRecorder : public QObject
{
    Q_OBJECT
public:

    // The tick. Same 20 ms as SMidiOutPump: fast enough that a 1024-slot ring
    // cannot fill from a human performance, cheap enough to run beside the
    // arranger.
    static constexpr int kTickMs = 20;

    explicit SMidiRecorder( SApplication *app, QObject *parent = nullptr );
    ~SMidiRecorder() override;

    /**
     * THE RECORD GESTURE HAS BEGUN, but the take has not: a count-in or a
     * pre-roll is in front of it (proposal 21 L5), and `start()` will not run
     * until the click has finished.
     *
     * CAPTURE ANYWAY, FROM HERE (QBX-213). A note played to a count-in click
     * is a note played AFTER the record button, and before this existed it
     * was lost in one of two ways, both measured:
     *
     *   - on the FIRST take of a session there was no recorder sink at all
     *     while the click ran - `SMidiInputHub::recorderSink()` is minted on
     *     first ask and the first ask was `start()` - so the fan-out had
     *     nowhere to put it and NOTHING logged the loss;
     *   - on every later take the sink existed, filled during the click, and
     *     `start()`'s drain threw the messages away (at TW_LOGD, so an
     *     ordinary run never saw that either).
     *
     * So this acquires the sinks and drains them ONCE, right here - which
     * keeps the drain's original purpose exactly: whatever is in the ring at
     * the instant the button goes down really is from before it. What arrives
     * during the preamble is then kept, and `start()` harvests it instead of
     * draining.
     *
     * IT DOES NOT MAKE THE RECORDER ACTIVE. `isActive()` stays false, so
     * `isRecordingActive()`, the monitor state (design D9) and the transport
     * edge are untouched: this is a buffer filling, not a take running.
     *
     * Nothing happens when no MIDI track is armed, when there is no project,
     * or when a take is already running.
     */
    void beginPreCapture();
    /// The preamble was cancelled: drop what it collected, so it cannot leak
    /// into the next take. Safe to call when no pre-capture is running.
    void cancelPreCapture();
    /// Is the pre-capture buffer filling? (`describe()` / diagnostics)
    bool isPreCapturing() const { return preCapture_; }

    /// Begin a take on every armed MIDI track. False (and nothing changed)
    /// when there are none, when there is no project, or when no port opens.
    bool start();
    /// End the take and commit the placement. Safe to call when not recording.
    void stop();

    bool isActive() const { return active_; }
    QString errorMessage() const { return error_; }

    /// Where the take was told to begin, in project frames.
    offset_t recordStartFrame() const { return recordStart_; }
    /// Loop passes committed by the last stop (1 when not cycling, 0 when
    /// nothing was captured).
    int lastPassCount() const { return lastPassCount_; }
    /// `place-midi-recording` calls the last stop submitted.
    int lastPlacementCount() const { return lastPlacements_; }
    /// Notes committed by the last stop, across every track and pass.
    int lastNoteCount() const { return lastNotes_; }
    /// Non-note events (CC, bend, program, pressure) committed by the last stop.
    int lastEventCount() const { return lastEvents_; }
    /// Messages the current (or last) take took off the rings.
    int capturedMessages() const { return captured_; }
    /// Armed MIDI tracks the last take covered.
    int lastTrackCount() const { return lastTracks_; }
    /// Notes whose mapped start fell before their pass and were CLAMPED to it.
    int clampedNotes() const { return clamped_; }
    /// Messages the PREAMBLE collected that this take admitted (QBX-213).
    int preCapturedMessages() const { return preMsgs_; }
    /// Notes played before the take's start and DROPPED as further away than
    /// the catch range (QBX-213).
    int uncaughtNotes() const { return uncaught_; }
    /**
     * THE PROJECT-LEVEL record mode this take started under, as plan 53 spells
     * it (`takes` | `takes-split` | `overdub` | `replace`).
     *
     * IT IS NOT NECESSARILY WHAT EVERY LANE GOT. The mode is resolved PER
     * ARMED LANE at the commit (`srecmode::resolveMode`: the lane's own
     * attribute, else the nearest ancestor folder's, else the project's), so a
     * track pinned to `overdub` inside a project on `takes` records
     * `overdub`. This is the project's answer, which is what `describe()` and
     * `assert-midi-recorded mode=` are about.
     *
     * WHAT CHANGED IN P3 (QBX-181). This used to be an enum of proposal 21
     * L4's own three values, read straight from `SOpt::MidiRecordMode` with no
     * notion of a per-track mode at all -- so the MIDI recorder and the AUDIO
     * recorder could disagree about the mode of the same press, and no MIDI
     * recording could ever be `takes-split`. There is one authority now, the
     * same one the audio recorder asks, and the INI key keeps its old job:
     * seeding a BRAND-NEW project (`srecmodeseed.cpp`).
     */
    QString mode() const { return mode_; }
    QString quantizeGrid() const { return quantize_; }

    QString describe() const;


private slots:
    void poll();

private:
    // One message as it left the device. POD, and copied out of the ring on
    // the main thread - the ring's own storage is the producer's.
    struct Msg {
        qint64       hostNs = 0;
        std::uint8_t size   = 0;
        std::uint8_t bytes[16] = {};
        // Did this arrive during the PREAMBLE, before the take began
        // (QBX-213)? The catch range applies to these and to no others: a
        // clamp of a pre-start message is about a performer anticipating the
        // downbeat, while a clamp INSIDE a running take is latency
        // compensation and is deliberately unbounded (a punch-in lead-in
        // clamps to the in point today, and that is not this ticket's).
        bool         pre = false;
    };

    // One OPEN input port. Two tracks on the same port share ONE sink, because
    // a ring has exactly one consumer (tw/devices inv. 20); the per-track
    // channel filter is applied when the buffer is read, not when it is
    // filled.
    struct PortIn {
        QString                    name;
        audio::MidiInFanout::Sink *sink = nullptr;
        qint64                     offsetProj = 0;   // midi/inputOffsetMs
        std::vector<Msg>           msgs;
    };

    struct Armed {
        STrack    *track = nullptr;
        QList<int> path;
        int        portIndex = -1;
        int        channel   = -1;    // -1 = any
    };

    // One port's pre-capture buffer, kept across `start()`'s own
    // `collectArmed_()` - which CLEARS `ports_`, so the messages have to be
    // carried over by PORT NAME rather than by index (QBX-213).
    struct PreMsgs {
        QString          port;
        std::vector<Msg> msgs;
    };

    bool   collectArmed_();
    /**
     * THE PUNCH REGION, if one is in force: the project's one range with Cycle
     * OFF (plan 53 P3, QBX-181). The same reading `SAudioRecorder` gives it,
     * and necessarily the same, because one press drives both: "the project
     * has ONE range, and what it means to a take depends on the cycle flag".
     *
     * THE MIDI RECORDER HAD NO PUNCH AT ALL, and the consequence was not a
     * missing feature but LOST NOTES. Under a punch the audio recorder stops
     * itself at the out point, which stops the transport, while this recorder
     * kept running to the `record-stop`; its final poll then re-anchored
     * `SPlayheadClock` from a dead clock, every host time mapped to a negative
     * frame, and `stopFrame_` read 0 -- so `passEnd` was 0, every note was
     * outside its own pass, and the whole take was dropped with
     * `placements=0`. Measured exactly that way on a mixed press with a
     * [48000, 96000) punch: 8 messages captured, 4 notes clamped, nothing
     * placed.
     *
     * So the fix is the audio recorder's, mirrored: END THE TAKE AT THE OUT
     * POINT, on this class's own tick, while the clock is still live. The pass
     * window is then the MARKERS, exactly, which is what punch means ("the
     * take is clipped to [in, out) exactly however late the tick") and makes
     * it independent of when the stop arrives.
     */
    bool   punchRegion_( offset_t &in, offset_t &out ) const;
    /// One last tick at the stop, before the transport is stopped by whoever
    /// owns it: the anchor is only valid while the RT is publishing.
    void   pollFinal_();
    void   drainRings_();
    int    portIndexFor_( const QString &portName );
    void   commit_();
    double projectRate_() const;
    bool   cycleRegion_( offset_t &in, offset_t &out ) const;

    SApplication *app_   = nullptr;
    QTimer       *timer_ = nullptr;

    bool     active_ = false;
    offset_t recordStart_ = 0;
    offset_t stopFrame_   = 0;
    QString  error_;

    SPlayheadClock clock_;
    qint64         lastPos_  = 0;
    int            playIter_ = 0;    // loop iteration the PLAYHEAD is in

    std::vector<PortIn> ports_;
    std::vector<Armed>  armed_;

    // Read ONCE at start (see the class comment).
    QString mode_     = QStringLiteral( "takes" );
    QString quantize_ = QStringLiteral( "off" );
    // `midi/recordCatchRangeMs` (QBX-213), in FRAMES at the take's rate, read
    // once at the start with its neighbours. 0 = off, which is exactly the
    // behaviour before the key existed.
    qint64  catchFrames_ = 0;
    bool    cycle_    = false;
    offset_t cycleIn_ = 0, cycleOut_ = 0;
    bool    punch_    = false;
    offset_t punchIn_ = 0, punchOut_ = 0;
    /**
     * Guards the punch-out against re-entering `stop()`. `stop()` runs
     * `pollFinal_()`, which sets `active_` and calls `poll()` -- the very
     * place the punch-out test lives -- so without this the out point would
     * recurse.
     */
    bool    stopping_ = false;
    /// The preamble's buffer is filling: the tick drains the rings and maps
    /// nothing, and `active_` is still false. See `beginPreCapture()`.
    bool    preCapture_ = false;

    int lastPassCount_  = 0;
    int lastPlacements_ = 0;
    int lastNotes_      = 0;
    int lastEvents_     = 0;
    int captured_       = 0;
    int clamped_        = 0;
    int preMsgs_        = 0;
    int uncaught_       = 0;
    int lastTracks_     = 0;
};

#endif // _SMIDIRECORDER_H_
