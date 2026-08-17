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
    /** Design D8's three input-record modes; a GLOBAL setting, not per track. */
    enum class Mode { NewTake = 0, Overdub = 1, Replace = 2 };

    // The tick. Same 20 ms as SMidiOutPump: fast enough that a 1024-slot ring
    // cannot fill from a human performance, cheap enough to run beside the
    // arranger.
    static constexpr int kTickMs = 20;

    explicit SMidiRecorder( SApplication *app, QObject *parent = nullptr );
    ~SMidiRecorder() override;

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
    Mode mode() const { return mode_; }
    QString quantizeGrid() const { return quantize_; }

    QString describe() const;

    static Mode    modeFromString( const QString &s, bool *ok = nullptr );
    static QString modeToString( Mode m );

private slots:
    void poll();

private:
    // One message as it left the device. POD, and copied out of the ring on
    // the main thread - the ring's own storage is the producer's.
    struct Msg {
        qint64       hostNs = 0;
        std::uint8_t size   = 0;
        std::uint8_t bytes[16] = {};
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

    bool   collectArmed_();
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
    bool     wasPlaying_ = false;
    offset_t recordStart_ = 0;
    offset_t stopFrame_   = 0;
    QString  error_;

    SPlayheadClock clock_;
    qint64         lastPos_  = 0;
    int            playIter_ = 0;    // loop iteration the PLAYHEAD is in

    std::vector<PortIn> ports_;
    std::vector<Armed>  armed_;

    // Read ONCE at start (see the class comment).
    Mode    mode_     = Mode::NewTake;
    QString quantize_ = QStringLiteral( "off" );
    bool    cycle_    = false;
    offset_t cycleIn_ = 0, cycleOut_ = 0;

    int lastPassCount_  = 0;
    int lastPlacements_ = 0;
    int lastNotes_      = 0;
    int lastEvents_     = 0;
    int captured_       = 0;
    int clamped_        = 0;
    int lastTracks_     = 0;
};

#endif // _SMIDIRECORDER_H_
