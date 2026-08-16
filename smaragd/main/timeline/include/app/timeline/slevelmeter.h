#ifndef _SLEVELMETER_H
#define _SLEVELMETER_H

// Proposal 34 — the level meter widget: a peak bar with a held peak tick, a
// slower RMS bar drawn inside it, and a latching clip cap.
//
// Owns its own twMeterBallistics; the caller only feeds it measurements and a
// timestamp (see SApplication::meterTick). It knows nothing about tracks, pages
// or the graph, which is what lets the same widget serve a track head, the Track
// Detail dock and the master meter in the transport bar.
//
// Proposal 36 B8 — N LANES. One twMeterBallistics per lane, all fed from one
// twLevelSampleSet, all integrated against the same wall-clock dt, so lanes
// cannot drift apart. The ballistics themselves are untouched and still scalar:
// a lane IS one scalar meter, and frame-rate independence (the reason they live
// on the UI thread at all) is a per-lane property that metering_test asserts
// lane by lane.
//
// HOW MANY LANES a mount shows is the MOUNT's decision, not this widget's — see
// setLanes(). Two shapes exist because the answers differ:
//
//   Split (default): the widget's thickness stays BAR_THICKNESS whatever the
//     lane count, and the lanes divide it. This is what the track head needs:
//     proposal 34 measured the 120 px control column as having ~13 px of slack,
//     an 8 px meter + 4 px spacing fits it, and nothing about a wider project
//     may make that untrue.
//   Grow (setGrowWithLanes): the thickness grows with the lane count, so every
//     lane keeps a readable bar. This is what the Track Detail dock uses, and
//     it is where a user sees all six channels of a six-channel project.

#include "tw/metering/tw_meter_ballistics.h"

#include <QWidget>

#include <vector>

class SLevelMeter : public QWidget
{
    Q_OBJECT
public:
    // Thickness of the bar across its short axis in Split mode, whatever the
    // lane count. 8 px fits the ~13 px of spare width the 120 px track control
    // column already had (see the ctor's note in ssmvmixercontrol.cpp) without
    // squeezing the fader.
    static constexpr int BAR_THICKNESS = 8;

    // Per-lane thickness in Grow mode. One lane comes out at exactly
    // BAR_THICKNESS, so a mono project's dock meter looks like it always did.
    static constexpr int GROW_LANE_THICKNESS = 5;

    // THE LANE CAP FOR SPACE-CONSTRAINED MOUNTS (proposal 36 B8 decision 2):
    // the track head and the transport master meter show at most the first TWO
    // channels, whatever the project's width.
    //
    // It follows the device rule the requester settled at B5 and twSpeaker
    // states in one line — `L = ch0; R = (width >= 2) ? ch1 : ch0`, the rest
    // computed in full and dropped at the device. A track head sits beside a
    // fader in a 120 px column with ~13 px of slack (proposal 34's measurement);
    // six lanes in it would be 1 px each, which is not a meter. Capping at the
    // pair you can actually HEAR keeps the head honest about the monitor path
    // instead of inventing a different reduction for the eye than for the ear.
    //
    // The cap is announced, not hidden: the tooltip names the width and points
    // at the Track Detail dock, which meters every channel (Grow mode), and
    // describe() reports `lanes=` and `width=` separately so a test — and a bug
    // report — can tell a cap from a narrow project.
    static constexpr int MONITOR_LANES = 2;

    explicit SLevelMeter( QWidget *parent = nullptr );

    // Vertical grows upward from the bottom; horizontal grows rightward. Also
    // applies the matching size constraints, so callers switching density only
    // have to call this — setting a fixed width for one orientation and a fixed
    // height for the other would otherwise leave the previous axis pinned.
    void setOrientation( Qt::Orientation o );
    Qt::Orientation orientation() const { return orientation_; }

    // `shown` is how many lanes to DRAW, `available` how many the project
    // actually has. They differ where a mount caps the count (the track head
    // shows two of six), and the difference is what the tooltip explains — a
    // user who sees two bars on a six-channel project must be able to find out
    // why. `available` <= 0 means "same as shown".
    //
    // Cheap to call every tick: a no-op when neither number changed.
    void setLanes( int shown, int available = -1 );
    int  lanes() const { return (int) ballistics_.size(); }
    int  availableChannels() const { return available_; }

    // Grow the widget's short axis with the lane count instead of dividing a
    // fixed BAR_THICKNESS among the lanes. See the class comment.
    void setGrowWithLanes( bool grow );

    // First line of the tooltip. The lane explanation is appended to it, so a
    // mount that wants to name itself ("Master level") does not have to
    // re-state — or silently drop — the reason it shows two of six lanes.
    void setMeterLabel( const QString &label );

    // Feed a measurement, or (pushIdle) declare that there is nothing to measure
    // so the bars decay. nowMs is monotonic; the ballistics integrate over the
    // real dt, so an irregular tick rate is harmless.
    //
    // A set carrying FEWER lanes than are drawn idles the rest rather than
    // holding them: a page narrower than the meter is a dropout for the lanes it
    // does not carry, and a frozen bar is the one reading a meter may never give.
    void pushLevel( const twLevelSampleSet &s, qint64 nowMs );
    void pushLevel( const twLevelSample &s, qint64 nowMs );   // lane 0; rest idle
    void pushIdle( qint64 nowMs );

    // Back to the floor with the clip latch cleared (transport start).
    void resetMeter();

    // Stable one-line description for headless tests, e.g.
    // "vis=1|orient=v|lanes=2|width=6|len=48|peak=17,9|rms=11,5|hold=22,14|clip=0|db=-11.4,-19.8".
    // Per-lane fields are comma-separated in lane order; a one-lane meter
    // therefore reads exactly as it did before B8.
    QString describe() const;

protected:
    void paintEvent( QPaintEvent * ) override;
    void mousePressEvent( QMouseEvent * ) override;   // clears the clip latch
    void resizeEvent( QResizeEvent * ) override;

private:
    // Length of the bar along its growth axis.
    int barLength() const;
    // Extent across the short axis, which in Grow mode depends on the lanes.
    int thickness() const;
    // Where lane `i` sits across the short axis, in widget coordinates, and how
    // thick it is. Returns false when there is no room to draw it at all.
    bool laneGeom( int i, int &offset, int &extent ) const;
    // dBFS -> pixels along the growth axis, clamped to [0, barLength()].
    int dbToPx( float db ) const;
    // Recompute the pixel geometry and repaint ONLY if what would be drawn
    // actually changed. This is the whole repaint-storm defence: 30 heads at
    // 30 Hz mostly produce no-ops, because a 20 dB/s decay moves the bar by
    // 0.66 dB per tick and a 48 px bar quantises most of that away.
    void refresh_();
    void applySizeConstraints_();
    void updateToolTip_();

    std::vector<twMeterBallistics> ballistics_;   // one per lane, never empty
    QString           label_;                     // tooltip's first line
    int               available_   = 1;           // the project's channel count
    bool              grow_        = false;
    Qt::Orientation   orientation_ = Qt::Vertical;

    // What was last PAINTED — the ground truth for change detection, never what
    // was last computed (the SMVActualView::lastPaintedCursorX_ discipline).
    // One entry per lane; -1 means "nothing painted yet".
    struct LanePx { int peak = -1; int rms = -1; int hold = -1; bool clip = false; };
    std::vector<LanePx> lastPainted_;
};

#endif
