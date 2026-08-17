#ifndef _TIMELINE_SAUTOMATIONLANE_H
#define _TIMELINE_SAUTOMATIONLANE_H

// THE AUTOMATION LANE UI (proposal 37 P6, design §6.1).
//
// Deliberately a file of its own and NOT another 300 lines in
// sstdmixerview.cpp, which is already the largest file in the app (timeline
// CONTRACT, known debt). Everything an automation sub-lane needs lives here:
// which lanes a track shows, the value<->pixel scale per target, the painting,
// the hit test, the whole gesture state machine, the picker menu, the clip
// envelope's hit test, and the testkit driver. `sstdmixerview.cpp` keeps only
// the CALL SITES.
//
// Several SStdMixerView / SMVActualView member functions are DEFINED in
// sautomationlane.cpp rather than in sstdmixerview.cpp for the same reason —
// they are automation code that happens to be spelled as a member of the view.
//
// This header is private to app/timeline (it lives in src/, not in
// include/app/timeline/). It is NOT app/model/sautomationlane.h — that one is
// the MODEL lane; this one is its face.

#include "app/model/sautomationlane.h"
#include "tw/core/twtypes.h"

#include <QHash>
#include <QList>
#include <QPoint>
#include <QRect>
#include <QSet>
#include <QString>
#include <QVector>

class QMenu;
class QPainter;
class SLink;
class SMVActualView;
class SObject;
class SStdMixerView;
class STrack;
struct STrackRow;

/// One shown automation lane on a track: the ParamRef spelling plus, for a
/// `param:` lane, which SLOT it belongs to. The pair is the lane's identity in
/// the arranger exactly as it is in the verbs.
struct SAutoLaneRef {
    QString target;
    int     slotIndex = -1;

    bool operator==( const SAutoLaneRef &o ) const
    { return target == o.target && slotIndex == o.slotIndex; }
    bool isValid() const { return !target.isEmpty(); }
};

/**
 * The value <-> pixel scale of ONE lane. Each target has its own domain
 * (app/model/sautomationlane.h): dB for `self:Volume`, 0/1 for `self:Muted`,
 * the plugin's declared range for `param:<id>`, a linear factor for `cut:Gain`.
 * A lane draws its own domain and nothing else — a shared 0..1 scale would show
 * a −60 dB fade as a flat line at the bottom.
 *
 * `fader` is the one non-affine case: `self:Volume` maps through the ONE fader
 * curve (app/timeline/sfadercurve.h, timeline inv. 13), so a point drawn at
 * mid-height sits where the head's fader would sit at mid-travel. Anything else
 * would make the lane and the fader disagree about the same number.
 */
struct SAutoValueScale {
    double lo = 0.0;
    double hi = 1.0;
    bool   fader = false;
    bool   stepped = false;      // draw as a staircase (self:Muted)

    double toNorm( double v ) const;      // -> [0,1], clamped
    double fromNorm( double n ) const;    // [0,1] -> the target's own units
};

/// The scale for `target` on `owner` (the owner is needed only to ask a plugin
/// slot for a parameter's declared range).
SAutoValueScale sAutoScaleFor( const QString &target, SObject *owner );

class SAutomationLaneUi
{
public:
    explicit SAutomationLaneUi( SStdMixerView &view );
    ~SAutomationLaneUi();

    // --- which lanes each track shows ------------------------------------
    const QVector<SAutoLaneRef> &shownLanes( const STrack *t ) const;
    bool isLaneShown( const STrack *t, const SAutoLaneRef &r ) const;
    void toggleLane( STrack *t, const SAutoLaneRef &r );
    /// Drop every entry whose track is gone. Called by the ONE prune walk
    /// (SStdMixerView::pruneUiState, proposal 30 §E.5).
    void pruneTo( const QSet<const STrack *> &live );

    // --- clip envelopes ---------------------------------------------------
    bool envelopeEditEnabled() const { return envelopeEdit_; }
    void setEnvelopeEditEnabled( bool on ) { envelopeEdit_ = on; }

    // --- painting ---------------------------------------------------------
    /// Paint one automation sub-lane: the value axis, the segments between
    /// breakpoints (per-point shape) and the points themselves.
    void drawAutomationLane( QPainter &p, SMVActualView &view,
                             const STrackRow &row, const QRect &laneRect );

    // --- the picker -------------------------------------------------------
    /// "Show automation >" with Volume, Mute and every parameter of every
    /// plugin slot on the track, each checkable.
    void buildPickerMenu( QMenu *parent, STrack *t );

    // --- gestures ---------------------------------------------------------
    // Canvas coordinates throughout. Each returns true when the event was
    // CONSUMED, which is what keeps the clip gestures untouched.
    bool press( SMVActualView &view, int rowIdx, const QPoint &pos,
                Qt::KeyboardModifiers mods, bool rightButton );
    bool move( SMVActualView &view, const QPoint &pos );
    bool release( SMVActualView &view, const QPoint &pos );
    bool deleteSelection();
    bool dragging() const { return drag_ != Drag::None; }

    // --- testkit ----------------------------------------------------------
    /// The `drag-clip-edge` twin: work out where the addressed point IS and
    /// send REAL press/move/release events into the canvas, so the gesture
    /// arithmetic under test is the one a pointer drives.
    bool tkDrag( const QList<int> &ownerPath, const QString &target,
                 int slotIndex, int take, offset_t time, double value,
                 offset_t toTime, double toValue, Qt::KeyboardModifiers mods );

private:
    struct Hit {
        SObject         *owner = nullptr;
        SAutomationLane *lane  = nullptr;
        int              index = -1;      // point index, -1 = none under the pointer
        QList<int>       ownerPath;
        SAutoLaneRef     ref;
        SAutoValueScale  scale;
        QRect            rect;
        // Point times are the OWNER's domain: 0 for a track/slot lane, the
        // clip's start for a `cut:` envelope (design 3.3 - a clip envelope is
        // clip-relative and travels with the clip).
        offset_t         timeBase = 0;
        int              take = -1;
        bool valid() const { return owner && lane; }
    };
    /// Resolve the lane a row shows, its owner path and its scale. `lane` is
    /// null when the track no longer owns it (the row is then drawn empty).
    Hit resolveRow( const STrackRow &row, const QRect &laneRect ) const;
    /// The `cut:Gain` envelope of the clip under `pos` on a normal track lane.
    /// Only ever consulted while envelope editing is armed.
    Hit resolveClipEnvelope( SMVActualView &view, int rowIdx,
                             const QPoint &pos ) const;
    /// Arm a point drag on `h`, remembering the pre-drag table so the release
    /// can REVERT and then commit through an action (timeline inv. 3).
    void beginPointDrag( const Hit &h, int index );
    /// Write the live drag state into the lane (no action, no engine push).
    void applyLivePoint();
    /// The point index within grab distance of `pos`, or -1.
    int pointAt( const Hit &h, SMVActualView &view, const QPoint &pos ) const;

    enum class Drag { None, Point, Tension, Marquee };

    SStdMixerView  &view_;
    QHash<const STrack *, QVector<SAutoLaneRef> > shown_;
    bool            envelopeEdit_ = false;

    // live gesture state
    // `consumed_` spans the WHOLE press/move/release triple, not just a drag:
    // a press we handled without arming a drag (a delete, an unresolvable lane)
    // must still swallow the move and the release, or they fall through to the
    // clip gestures on a lane that has no clips.
    bool        consumed_ = false;
    Drag        drag_ = Drag::None;
    QList<int>  dragOwnerPath_;
    SAutoLaneRef dragRef_;
    int         dragTake_ = -1;
    SAutomationLane *dragLane_ = nullptr;
    SAutoValueScale  dragScale_;
    QRect       dragRect_;
    offset_t    dragBase_ = 0;          // the owner domain's time base
    offset_t    dragFromTime_ = 0;      // the point's ORIGINAL (time, value)
    double      dragFromValue_ = 0.0;
    offset_t    dragToTime_ = 0;        // where it currently sits
    double      dragToValue_ = 0.0;
    double      dragTension_ = 0.0;
    std::vector<SAutomationPoint> dragUndoPoints_;   // for revert-then-action
    QPoint      marqueeFrom_;
    QPoint      marqueeTo_;
    // Selected points of the lane being edited, by frame (frames survive a
    // re-sort; indices do not).
    QList<qint64> selected_;
};

#endif // _TIMELINE_SAUTOMATIONLANE_H
