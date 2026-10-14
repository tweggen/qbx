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
 * START (`start()`), and the ORDER is load-bearing:
 *   1. collect the armed tracks (refuse if there are none) and take a hold on
 *      `SLiveMonitor`'s bridge - THE app's one input pump - resolving the
 *      device name the way the monitor does;
 *   2. open a capture SEGMENT on it, with one WAV sink per armed track;
 *   3. compute the placement terms that are knowable now (`P0` and the output
 *      latency both arrive with the anchor);
 *   4. put a GROWING CLIP on each armed track: one `SRecordingContent` over
 *      the segment's pages, one `SCut` per track, one `SLink` at the record
 *      start. Direct model mutation, deliberately NOT an action - like
 *      auto-disarm, it is transient UI state that the ONE undoable step at
 *      stop replaces;
 *   5. LAST, and only after `active_` is set:
 *      `SApplication::setPlaybackRunning(true)` - the transport funnel, so the
 *      MIDI-out pump, the automation recorder and the live monitor all learn
 *      that the transport started (design D7: "`startRecording` goes through
 *      `setPlaying()`"). It is last because monitor AUTO is "input while
 *      stopped OR RECORDING" (design D9): the plan has to be built with
 *      `isRecordingActive()` already true, or plain Play would take the input
 *      away from the take. `locatorHeldElsewhere()` is retired with it: the
 *      OUTPUT publication is the playhead authority in every mode.
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

    /**
     * HOW FAR THE PASS-WIDE SNAP MOVED THE LAST PLAIN PASS'S START
     * (plan 53 §3.5, QBX-171). 0 when nothing snapped, and always 0 for a
     * cycle or punch pass, which are not snapped.
     *
     * It exists because the snap BREAKS an identity the suite relies on.
     * `assert-recorded-clip` checks, on every plain single-pass take, that the
     * clip's link start equals `placementFrame( trimmedFrames() )` -- the one
     * assertion that catches a placement term reported correctly and never
     * applied. A snapped pass lands deliberately somewhere else, so without
     * this term that check would fail on every snapped take, and would fail
     * INTERMITTENTLY on existing cases whose landing happens to come within
     * N of a neighbouring clip's edge. With it the identity becomes
     * `placementFrame( trimmed ) + snapDeltaA()`, which still catches exactly
     * what it caught before and now covers the snap as well.
     */
    std::int64_t snapDeltaA() const { return snapDeltaA_; }

    /**
     * A LANE THIS PASS COULD NOT PLACE ON, and why (plan 53 B6, QBX-164).
     *
     * A skipped lane used to be a `TW_LOGW` and a `continue`, which is
     * indistinguishable from the take never having been recorded: the user
     * armed a lane, watched the meters move, pressed stop, and got nothing,
     * with the only explanation in a log nobody reads. The capture FILE is
     * still on disk in every case -- nothing is lost -- so the two things worth
     * reporting are which lane missed out and where its audio went.
     *
     * Two causes reach here. The capture file is missing (the backend never
     * wrote it), and the lane's PLANNER REFUSED the pass -- which only became
     * reachable when QBX-172 gave `takes-split` and `replace` the wrapped-column
     * refusal, and is why plan 53 made this row wait for P1.
     */
    struct SkippedLane {
        QList<int> path;    ///< the lane, as a path from the root mixer
        QString    file;    ///< its capture file; still on disk
        QString    why;     ///< one line, already user-facing
    };

    /** The lanes the LAST pass skipped. Reset at the start of every pass. */
    const QList<SkippedLane> &skippedLanes() const { return skipped_; }

    /**
     * The capture files of this pass that no lane took (plan 53 Q13).
     *
     * Distinct, in pass order. This is the LIST only; the Unused recordings
     * dock that presents it is P6's, and `scleanupdialog` is a different thing
     * again -- it sweeps the recordings DIRECTORY for files no project
     * references, which is housekeeping rather than "the take you just made
     * went nowhere".
     */
    QStringList unusedRecordings() const;
    /// The files the last take wrote, in armed-track order.
    const QList<QString> &lastFiles() const { return lastFiles_; }
    /// THE INPUT DEVICE THE LAST/CURRENT TAKE RESOLVED TO, and therefore the
    /// key `audio/recordingOffsetMs/<name>` was read under (see start()).
    /// Exposed because that resolution is the one term of the placement
    /// conversion a case cannot otherwise see: a lookup that misses returns
    /// 0.0, which is indistinguishable from a calibration of zero, so a gate
    /// on the offset is only worth anything if it can also assert WHICH device
    /// was asked about.
    QString inputDeviceName() const { return deviceName_; }

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
    /**
     * THE PASS-WIDE SNAP (plan 53 §3.5, QBX-171).
     *
     * Rounds the pass's placed span to an edge the armed lanes AGREE on, when
     * there is one within `N = max( one device buffer, 10 ms )` of it.
     *
     * Why a pass has to be snapped at all: a take begun or ended by hand next
     * to existing material lands a few hundred frames off, and the sliver that
     * leaves behind is audible as a click and invisible at any sane zoom. The
     * snap moves the clip WINDOW only -- the file is untouched, so the material
     * the window no longer shows is still there and Alt-slip reveals it. That
     * is plan 53 G2's answer in the user's words: the head is trimmed from the
     * window, not from the source.
     *
     * UNANIMITY, not nearest. A candidate set that disagrees means the lanes
     * have different material at that moment, and snapping to one lane's edge
     * would move the recording relative to the others. So: every candidate
     * within `N` must be the SAME frame, or there is no snap.
     *
     * Only for the PLAIN pass. A punch or cycle span IS the markers, which are
     * exact already (§3.5 rule 3).
     *
     * Called from `commitPlacement_()` AFTER `removeGrowingClips_()`: a growing
     * clip is an ordinary lane child with a duration whose end trails the
     * capture by up to one 100 ms tick, so a snap that could see one would find
     * an agreeing candidate for `b` on EVERY armed lane and snap to it,
     * silently and non-deterministically. `snapshot()` skips them as well; this
     * is the ordering that makes that belt-and-braces rather than load-bearing.
     */
    void snapPassWide_( std::int64_t *a, std::int64_t *b ) const;

    /**
     * WHAT THE USER SEES WHEN A TAKE LANDS (plan 53 Q11, P6).
     *
     * `landed` is the lanes this pass actually placed on, as paths from the
     * root mixer. Either their take lanes are OPENED, or -- when a pass
     * touched more than two tracks -- a one-line hint says what happened and
     * how to see it, because expanding eight lanes adds a row per take and
     * would rearrange the arranger under the user's hands.
     *
     * Called AFTER `endRecordCommit()` so the expand is not part of the undo
     * step: the take is the edit, how the arranger is folded is not, and an
     * undo that re-collapsed the lanes would hide the thing the user is
     * undoing. It announces itself with `SProject::notifyArrangementChanged()`
     * because `setTakesExpanded` is a plain model write with no signal of its
     * own, and `SStdMixerView::onArrangementChangedRows` is already the slot
     * that rebuilds take rows for exactly this class of change.
     */
    void applyLandingFeedback_( const QList<QList<int>> &landed,
                                int takesPerLane );

    /**
     * HOW MANY TRACKS MAY OPEN THEMSELVES (plan 53 Q11's "<= 2 tracks").
     *
     * The bound is about the ARRANGER, not about taste: expanding a track adds
     * one row per take, so a band pass across eight lanes would rearrange the
     * whole arranger at the moment the user is reaching for stop. Two is what
     * still fits beside what they were looking at.
     */
    static constexpr int kMaxAutoExpandTracks = 2;
    /** N in project frames: one device buffer, or 10 ms, whichever is more. */
    std::int64_t snapWindowFrames_() const;

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
    std::int64_t     snapDeltaA_ = 0;
    // Reset per pass, like snapDeltaA_: the report is about the pass that just
    // ended, never an accumulation over the session.
    QList<SkippedLane> skipped_;
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
