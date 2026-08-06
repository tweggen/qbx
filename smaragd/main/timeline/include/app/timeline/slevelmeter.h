#ifndef _SLEVELMETER_H
#define _SLEVELMETER_H

// Proposal 34 — the level meter widget: a peak bar with a held peak tick, a
// slower RMS bar drawn inside it, and a latching clip cap.
//
// Owns its own twMeterBallistics; the caller only feeds it measurements and a
// timestamp (see SApplication::meterTick). It knows nothing about tracks, pages
// or the graph, which is what lets the same widget serve a track head, the Track
// Detail dock and the master meter in the transport bar.

#include "tw/metering/tw_meter_ballistics.h"

#include <QWidget>

class SLevelMeter : public QWidget
{
    Q_OBJECT
public:
    // Thickness of the bar across its short axis. 8 px fits the ~13 px of spare
    // width the 120 px track control column already had (see the ctor's note in
    // ssmvmixercontrol.cpp) without squeezing the fader.
    static constexpr int BAR_THICKNESS = 8;

    explicit SLevelMeter( QWidget *parent = nullptr );

    // Vertical grows upward from the bottom; horizontal grows rightward. Also
    // applies the matching size constraints, so callers switching density only
    // have to call this — setting a fixed width for one orientation and a fixed
    // height for the other would otherwise leave the previous axis pinned.
    void setOrientation( Qt::Orientation o );
    Qt::Orientation orientation() const { return orientation_; }

    // Feed a measurement, or (pushIdle) declare that there is nothing to measure
    // so the bars decay. nowMs is monotonic; the ballistics integrate over the
    // real dt, so an irregular tick rate is harmless.
    void pushLevel( const twLevelSample &s, qint64 nowMs );
    void pushIdle( qint64 nowMs );

    // Back to the floor with the clip latch cleared (transport start).
    void resetMeter();

    // Stable one-line description for headless tests, e.g.
    // "vis=1|orient=v|len=48|peak=17|rms=11|hold=22|clip=0|db=-11.4".
    QString describe() const;

protected:
    void paintEvent( QPaintEvent * ) override;
    void mousePressEvent( QMouseEvent * ) override;   // clears the clip latch
    void resizeEvent( QResizeEvent * ) override;

private:
    // Length of the bar along its growth axis.
    int barLength() const;
    // dBFS -> pixels along the growth axis, clamped to [0, barLength()].
    int dbToPx( float db ) const;
    // Recompute the pixel geometry and repaint ONLY if what would be drawn
    // actually changed. This is the whole repaint-storm defence: 30 heads at
    // 30 Hz mostly produce no-ops, because a 20 dB/s decay moves the bar by
    // 0.66 dB per tick and a 48 px bar quantises most of that away.
    void refresh_();

    twMeterBallistics ballistics_;
    Qt::Orientation   orientation_ = Qt::Vertical;

    // What was last PAINTED — the ground truth for change detection, never what
    // was last computed (the SMVActualView::lastPaintedCursorX_ discipline).
    int  lastPaintedPeakPx_ = -1;
    int  lastPaintedRmsPx_  = -1;
    int  lastPaintedHoldPx_ = -1;
    bool lastPaintedClip_   = false;
};

#endif
