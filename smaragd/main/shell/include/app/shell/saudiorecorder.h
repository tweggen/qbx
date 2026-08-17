#ifndef _SAUDIORECORDER_H_
#define _SAUDIORECORDER_H_

#include <cstdint>
#include <memory>
#include <vector>

#include <QList>
#include <QObject>
#include <QString>

#include "app/shell/srecordplacement.h"
#include "tw/core/twtypes.h"

class QTimer;
class SApplication;
class SCut;
class SLink;
class SRecordingContent;
class STrack;

namespace audio {
class CaptureBridge;
}

/**
 * THE AUDIO RECORDER (proposal 21 L3b, design D6/D7/D9).
 *
 * A sibling of `SMidiOutPump`, `SAutomationRecorder` and `SLiveMonitor`: one
 * per `SApplication`, main thread only, owning a `QTimer`. It is what a record
 * start and a record stop MEAN in the app, and everything that used to live
 * spread across `SApplication::startRecording`, `SMainWindow::onRecordTriggered`
 * and `SMainWindow::onRecordingCompleted` is here instead.
 *
 * WHAT IT DOES, in order.
 *
 * START (`start()`):
 *   1. collect the armed tracks; refuse if there are none;
 *   2. `SApplication::setPlaying(true)` — the transport funnel, so the MIDI-out
 *      pump, the automation recorder and the live monitor all learn that the
 *      transport started (design D7: "`startRecording` goes through
 *      `setPlaying()`"). `locatorHeldElsewhere()` is retired with it: the
 *      OUTPUT publication is the playhead authority in every mode;
 *   3. take a hold on `SLiveMonitor`'s bridge — THE app's one input pump — and
 *      open a capture SEGMENT on it with one WAV sink per armed track;
 *   4. put a GROWING CLIP on each armed track: one `SRecordingContent` over
 *      the segment's pages, one `SCut` per track, one `SLink` at the record
 *      start. Direct model mutation, deliberately NOT an action — like
 *      auto-disarm, it is transient UI state that the ONE undoable step at
 *      stop replaces;
 *   5. start a 100 ms tick.
 *
 * TICK (`poll()`), three jobs, in this order:
 *   - ANCHOR. Until the engine clock has published since the run began, the
 *     placement has no `P0`. As soon as it does, `P0` is computed by mapping
 *     the bridge's `captureStartHostNs()` through that anchor — BACKWARD
 *     extrapolation, which is the ordinary case for a record start from a
 *     stopped transport (design D6) — and every clip is re-seated at
 *     `placementFrame(0)`, clamped to the record start;
 *   - GROWTH. `SRecordingContent::publishGrowth()` extends the peak ladder and
 *     emits `durationChanged`; each cut's window follows;
 *   - PUNCH-OUT. With a punch region set, the take ends by itself when the
 *     placement passes the out point.
 *
 * STOP (`stop()`): end the segment (which finalises the files out of the
 * pages), remove the growing clips, then ONE undo macro of `place-recording`
 * calls — one per armed track, and one per LOOP PASS when cycling, which is
 * how proposal 17's "phase 5" falls out of a verb that already existed.
 *
 * THREADING. Everything here is main thread. The bridge thread never touches
 * the model: it appends to the pages and stores a frontier, and this class
 * polls. That is design 4's contract and the reason the growing clip is safe
 * at all.
 */
class SAudioRecorder : public QObject
{
    Q_OBJECT
public:
    explicit SAudioRecorder( SApplication *app, QObject *parent = nullptr );
    ~SAudioRecorder() override;

    /// Begin a take. False (and nothing changed) when there are no armed
    /// tracks, no project, or the input would not open.
    bool start();
    /// End the take and commit the placement. Safe to call when not recording.
    void stop();

    bool isActive() const { return active_; }
    double recordedSeconds() const;
    QString errorMessage() const { return error_; }

    // ---- what the placement did, for the UI and for assert-recorded-clip --
    const SRecordPlacement &placement() const { return placement_; }
    /// Capture frames dropped off the FRONT because their placement landed
    /// before the transport start (design D6: "frames captured before the
    /// transport start are trimmed").
    std::int64_t trimmedFrames() const { return trimmed_; }
    /// Where the take was told to begin, in project frames.
    offset_t recordStartFrame() const { return recordStart_; }
    /// Loop passes committed by the last stop (1 when not cycling).
    int lastPassCount() const { return lastPassCount_; }
    /// The files the last take wrote, in armed-track order.
    const QList<QString> &lastFiles() const { return lastFiles_; }

    QString describe() const;

private slots:
    void poll();

private:
    struct Armed {
        STrack           *track = nullptr;
        QList<int>        path;
        SCut             *cut   = nullptr;   // the growing clip (borrowed)
        SLink            *link  = nullptr;   // its placement (borrowed)
        QString           file;
        std::uint32_t     mask  = 0;
    };

    bool collectArmed_();
    void placeGrowingClips_();
    void removeGrowingClips_();
    void tryAnchor_();
    void reseatClips_();
    void commitPlacement_();
    bool punchRegion_( offset_t &in, offset_t &out ) const;
    bool cycleRegion_( offset_t &in, offset_t &out ) const;
    double projectRate_() const;

    SApplication         *app_ = nullptr;
    QTimer               *timer_ = nullptr;
    audio::CaptureBridge *bridge_ = nullptr;   // borrowed from SLiveMonitor

    bool     active_ = false;
    // Was the transport ALREADY running when the take began? It decides the
    // TRIM FLOOR: design D6 trims "frames captured before the TRANSPORT
    // start", which is the record start only when the take started the
    // transport. Recording into a run that was already playing legitimately
    // places audio EARLIER than the button press — that is what latency
    // compensation IS — and trimming to the button press would throw the
    // compensation away again.
    bool     wasPlaying_ = false;
    offset_t recordStart_ = 0;
    QString  error_;
    QString  deviceName_;

    SRecordPlacement placement_;
    std::uint64_t    startPublishSeq_ = 0;
    std::int64_t     trimmed_ = 0;
    int              lastPassCount_ = 0;
    QList<QString>   lastFiles_;

    SRecordingContent                 *content_ = nullptr;  // owned by the cuts
    std::vector<Armed>                 armed_;

    // Punch, read once at start so a UI edit mid-take cannot move the goalposts.
    bool     punch_ = false;
    offset_t punchIn_ = 0, punchOut_ = 0;
    bool     cycle_ = false;
    offset_t cycleIn_ = 0, cycleOut_ = 0;
};

#endif // _SAUDIORECORDER_H_
