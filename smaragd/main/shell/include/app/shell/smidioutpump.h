#ifndef _SMIDIOUTPUMP_H_
#define _SMIDIOUTPUMP_H_

#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include <QObject>
#include <QString>
#include <QStringList>

#include "tw/core/twtypes.h"
#include "tw/devices/midi_input.h"
#include "tw/devices/midi_out_scheduler.h"
#include "tw/devices/midi_output.h"
#include "tw/events/tweventsource.h"

class QTimer;
class SObject;
class SProject;
class STrack;

/**
 * SMidiOutPump - MIDI out at PLAY time (proposal 36 D6, phase P7b).
 *
 * THE METERING LESSON (proposal 34) APPLIES VERBATIM, and it is the reason
 * this class exists at all rather than a hook in the freeze path: pages are
 * frozen ~1.4 s ahead of the playhead by the readahead, and by renders that
 * have no playhead at all. Anything emitted at freeze time would therefore be
 * the FUTURE, and a render would spray notes at a MIDI device with the
 * transport stopped. So MIDI out is driven from the PLAYHEAD, on the main
 * thread, exactly like the meters:
 *
 *   - a `QTimer` at kTickMs (20 ms), started when the transport starts and
 *     stopped when it stops - a sibling of SApplication::meterTimer_, never a
 *     fold into it (the meters need a tail after the stop; this one must go
 *     silent the instant the transport does);
 *   - each tick reads the playhead atomic, slices every MIDI-out track's event
 *     FEED (STrack::eventFeed - so a folder parent's port carries its
 *     children's patterns, 3.2.1) over a kLookaheadMs window, converts the
 *     events to bytes and hands them to a MidiOutScheduler with a due HOST
 *     TIME;
 *   - the scheduler's std::thread does the sending. NOTHING below this class
 *     touches Qt (THREADING.md rule 1): a Qt signal from the sender thread
 *     would make Qt adopt it and the adopted thread's TLS cleanup deadlocks
 *     the join at teardown.
 *
 * Alignment (D6):
 *
 *   dueHostTime = hostTime(playhead) + deviceOutputLatency
 *                 - midiOutLatency - globalOffset - trackOffset
 *
 * `hostTime(playhead)` comes from an ANCHOR (playhead value, host time) that
 * is re-taken every time the published playhead CHANGES; between publications
 * the position is extrapolated at the project rate. The anchor is what makes
 * the pump wait for audio to actually flow: twSpeaker defers the device start
 * until the readahead is primed, and until the RT thread has published a
 * second position there is no clock to hang a due time on.
 *
 * De-dup is a per-track FRONTIER (`emittedTo` + the loop iteration it lives
 * in) rather than a set of `(clip key, event ordinal, loop iteration)` keys:
 * windows are contiguous and never overlap, so a monotone frontier gives the
 * same guarantee with no bookkeeping - and it also survives an edit that
 * renumbers ordinals mid-flight, which a key set would not.
 *
 * Chase / wrap / stop, per D6:
 *   - start and locate: CC / program / bend / channel pressure ALWAYS, note-ons
 *     only when Options -> MIDI says so (default OFF for MIDI out - a chased
 *     note-on to a hardware synth is usually a surprise, not a service);
 *   - loop wrap: the window is SPLIT at the cycle end, every note this pump
 *     has sent and not released gets a note-off there, and the chase is
 *     re-issued at the cycle start;
 *   - stop / locate / panic: sustain-off (CC64=0) + all-notes-off (CC123=0) on
 *     every channel this run used, through MidiOutScheduler::panic(), which
 *     discards the queued future first (a queued note-on that escaped after
 *     the stop is a stuck note, not a courtesy).
 *
 * Renders emit nothing: the pump is never started for one (SApplication does
 * not start it, and tick() returns immediately while a render is active).
 */
class SMidiOutPump : public QObject
{
    Q_OBJECT
public:
    // 20 ms tick / 250 ms lookahead (D6). The lookahead is what absorbs a
    // late tick: a message is queued with its due time up to 250 ms before it
    // is due, so a 60 ms hiccup on the GUI thread moves nothing on the wire.
    static constexpr int kTickMs      = 20;
    static constexpr int kLookaheadMs = 250;
    // If the cursor ever falls this far behind the playhead (a long GUI stall,
    // a debugger), the backlog is DROPPED rather than flushed at the device:
    // a burst of notes whose moment has passed is worse than a gap.
    static constexpr int kMaxBacklogMs = 1000;

    explicit SMidiOutPump( QObject *parent = nullptr );
    ~SMidiOutPump() override;

    // Transport edges. start() clears the capture MIDI recording (so a
    // captured host time always belongs to the CURRENT playback session,
    // exactly like CaptureBackend::clearCapture at startOutput), stop() panics
    // and flushes.
    void start();
    void stop();
    // The transport jumped while running: panic, drop the queued future, and
    // re-chase at the new position.
    void locate( offset_t newPos );

    bool isActive() const;

    // Port enumeration for the Options dialog. Served from ONE long-lived
    // probe port owned here, never from a throwaway: CaptureMidiOutput
    // registers the most recently constructed instance as the one the testkit
    // reads, so minting and destroying a MidiOutput to ask it a question would
    // unregister the live one.
    std::vector<audio::MidiPortInfo> outputPorts() const;
    std::vector<audio::MidiPortInfo> inputPorts() const;
    // The MIDI backend actually in use ("winmm", "capture", "null", ...).
    QString backendName() const;
    // Whether this platform can create a virtual port at all (WinMM cannot -
    // tw/devices CONTRACT inv. 18; loopMIDI-style drivers appear as ordinary
    // devices instead). A UI gates its offer on this, like the MP3 option.
    bool supportsVirtualPorts() const;

    // Resolve a portable port NAME to the machine-local id open() wants:
    // an explicit SSettings mapping first, then a case-insensitive match
    // against the backend's own id/name list, then the name verbatim.
    QString resolvePortId( const QString &portName ) const;

private slots:
    void tick();

private:
    struct Held {
        int channel = 0;
        int key     = 0;
    };
    // One MIDI-out track's emission state for the current transport run.
    struct Cursor {
        QString        portId;
        qint64         pos    = 0;   // frontier, in project frames
        int            iter   = 0;   // which loop iteration `pos` lives in
        bool           chased = false;
        std::uint16_t  usedChannels = 0;   // bit 0 = channel 0
        std::vector<Held> held;            // note-ons sent and not released
    };

    struct Cycle {
        bool   on    = false;
        qint64 start = 0;
        qint64 end   = 0;
        qint64 len() const { return end - start; }
    };

    void  resetRun( offset_t pos );
    void  collectTracks( SObject *root, bool anySolo,
                         std::vector<STrack *> &out ) const;
    audio::MidiOutScheduler *schedulerFor( const QString &portId );
    void  emitBlock( const twEventBlock &block, Cursor &c, STrack *track,
                     qint64 windowStartAbs, qint64 rate,
                     audio::MidiOutScheduler *sched );
    void  emitChase( const twEventBlock &block, Cursor &c, STrack *track,
                     qint64 atAbs, qint64 rate, audio::MidiOutScheduler *sched );
    void  releaseHeld( Cursor &c, qint64 atAbs, qint64 rate, int offsetMs,
                       audio::MidiOutScheduler *sched );
    void  panicAll();
    // dueHostTime for an absolute (wrap-counted) project frame.
    qint64 dueNsFor( qint64 absFrame, qint64 rate, int trackOffsetMs,
                     qint64 midiLatencyNs ) const;
    void  send3( audio::MidiOutScheduler *sched, qint64 dueNs,
                 int status, int d1, int d2 );
    void  send2( audio::MidiOutScheduler *sched, qint64 dueNs,
                 int status, int d1 );
    static Cycle readCycle( SProject *project );

    QTimer *timer_ = nullptr;

    // One scheduler (thread + port) per distinct resolved port id. Created
    // lazily on first use and kept for the process: opening a MIDI device is
    // not free, and a stop/start cycle must not drop the port.
    std::map<QString, std::unique_ptr<audio::MidiOutScheduler>> schedulers_;
    // Ports whose open() failed - logged once, then skipped silently.
    QStringList failedPorts_;

    // The enumeration probes (see outputPorts()). Constructed with the pump,
    // i.e. before any scheduler's port, so the scheduler's capture instance is
    // the one the testkit reads.
    std::unique_ptr<audio::MidiOutput> probeOut_;
    std::unique_ptr<audio::MidiInput>  probeIn_;

    std::map<STrack *, Cursor> cursors_;
    std::map<QString, QString> portIdCache_;   // portable name -> device id

    // The playhead anchor (see the class comment).
    bool    haveAnchor_ = false;
    qint64  anchorAbs_  = 0;   // frame just delivered + iter * cycle length
    qint64  anchorNs_   = 0;   // host time that frame will be HEARD at
    qint64  lastPos_    = 0;
    qint64  runStartPos_ = 0;  // where this transport run began
    quint64 lastPublishSeq_ = 0;   // last RT position publication we anchored on
    int     playIter_   = 0;   // loop iteration the PLAYHEAD is in
    bool    running_    = false;

    // Read once per run from Options -> MIDI, so a mid-run settings change
    // cannot make the chase inconsistent with what was already emitted.
    bool   chaseNoteOns_  = false;
    int    globalOffsetMs_ = 0;

    twEventBlock scratch_;     // reused across ticks; the pump never allocates
                               // per event if it can help it
};

#endif // _SMIDIOUTPUMP_H_
