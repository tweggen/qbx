#ifndef _SAUTOMATIONRECORDER_H
#define _SAUTOMATIONRECORDER_H

#include "app/model/sautomationlane.h"

#include <QList>
#include <QObject>
#include <QString>
#include <vector>

class SProject;

/**
 * THE TOUCH / LATCH / WRITE RECORDER (proposal 37 P6, design D5).
 *
 * Touch, Latch and Write are not engine modes — the engine reads a Read-family
 * lane and cannot tell them apart. They are UI RECORDERS: a control (the track
 * head's fader, the plugin parameter editor's slider, or the testkit's
 * `automation-write-tick`) feeds values in while the transport runs, and the
 * whole pass lands as **ONE** `set-automation-points` on the undo stack.
 *
 * "One action per gesture, never one per block" is the load-bearing rule
 * (design §3.4). A recorder that submitted an action per tick would put ~30
 * entries per second on the undo stack, and a single Ctrl-Z would peel one
 * 33 ms sliver off a four-second pass.
 *
 * THE THREE MODES, and what actually differs between them:
 *
 *   Touch  the pass is exactly the span the control was HELD:
 *          [firstTick, lastTick]. Releasing commits at once, so the curve
 *          outside the touched span is untouched — the fader "releases back to
 *          the curve".
 *   Latch  the pass starts at the first tick and runs to the TRANSPORT STOP.
 *          After the control is released the last value is HELD, which is
 *          expressed as one extra point at the stop frame rather than as a
 *          stream of identical ticks.
 *   Write  the pass is the whole transport run: [transportStart, stop]. The
 *          first recorded value is written back to the pass start too, so
 *          everything the pass rolled over is OVERWRITTEN — that is the only
 *          thing that distinguishes Write from Latch.
 *
 * EVERYTHING RUNS ON THE MAIN THREAD. The values arrive from widgets and from
 * the meter pump; nothing here is touched by an audio or worker thread, and
 * nothing here blocks. The commit goes through the ordinary action history, so
 * it is undoable, scriptable and coalescable exactly like a typed-in edit.
 */
class SAutomationRecorder : public QObject
{
    Q_OBJECT
public:
    /// Which lane a pass writes to — the same address every automation verb
    /// takes (design §3.4: `owner` + `target`, disambiguated by the target's
    /// space, plus `slotIndex` for a `param:` lane).
    struct Target {
        QList<int> ownerPath;
        QString    target;
        int        slotIndex = -1;
        int        take = -1;

        bool operator==( const Target &o ) const
        {
            return ownerPath == o.ownerPath && target == o.target
                && slotIndex == o.slotIndex && take == o.take;
        }
    };

    explicit SAutomationRecorder( QObject *parent = nullptr );
    virtual ~SAutomationRecorder();

    /// The transport started at `pos`. Write mode's overwrite window begins
    /// here, which is why the recorder has to be told and cannot ask.
    void transportStarted( offset_t pos );
    /// The transport stopped at `pos`; any open pass COMMITS.
    void transportStopped( offset_t pos );

    /// One live tick. Punches in on the first call, appends afterwards.
    /// Returns false — and records nothing — when the addressed lane does not
    /// exist or its mode is not Touch/Latch/Write, which is what keeps a plain
    /// Read lane's fader an ordinary undoable `set-track-volume`.
    bool writeTick( const Target &t, double value, offset_t frame );

    /// The control was released. Touch commits here; Latch and Write hold the
    /// last value until the transport stops.
    void releaseControl();

    /// True while a pass is open on this exact lane. The read-value display
    /// asks, because a control that is BEING recorded must show the hand, not
    /// the curve.
    bool isRecording( const Target &t ) const;
    bool isActive() const { return active_; }

    /// The lane's mode, or Off when there is no such lane. Static helper so
    /// callers do not have to resolve an owner twice.
    static SAutomationMode modeOf( const Target &t );
    static bool isRecordMode( SAutomationMode m )
    { return m == SAutomationMode::Touch || m == SAutomationMode::Latch
          || m == SAutomationMode::Write; }
    /// Read-family: the curve is the whole value (design §11 decision 3), so
    /// the control DISPLAYS it.
    static bool isReadFamily( SAutomationMode m )
    { return m == SAutomationMode::Read || isRecordMode( m ); }

private:
    void commit_( offset_t stopFrame );

    bool            active_ = false;
    bool            released_ = false;
    Target          target_;
    SAutomationMode mode_ = SAutomationMode::Touch;
    offset_t        passStart_ = 0;      // transport start (Write's window)
    offset_t        firstFrame_ = 0;
    offset_t        lastFrame_ = 0;
    double          lastValue_ = 0.0;
    bool            transportRunning_ = false;
    std::vector<SAutomationPoint> pts_;
};

#endif // _SAUTOMATIONRECORDER_H
