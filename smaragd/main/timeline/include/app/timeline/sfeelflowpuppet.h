#ifndef SFEELFLOWPUPPET_H
#define SFEELFLOWPUPPET_H

#include <QWidget>

#include "app/objects/track/sfeelflowpose.h"
#include "app/model/sfeelflowskeleton.h"

// Proposal 40 "Feel Flow" M3e AC 5 -- the PUPPET's widget half.
//
// PAINT ONLY. This widget reads NOTHING: not the model, not the track, not
// the sidecar store, not the locator. It draws the ONE pose it was last
// handed, and the only way a pose gets in is setPose(). That is the whole
// discipline (main/timeline/CONTRACT.md inv. 1's rule, stated as a type):
// with no reachable model there is no path from paintEvent to a demand, a
// freeze or a blocking read, and no future edit can add one without first
// adding an include.
//
// WHO decides which track, WHEN to read it, and whether the analysis is
// stale is the DOCK's business (SMainWindow) -- see main/shell/CONTRACT.md.
// A stale analysis arrives here as an INVALID pose, exactly as "no analysis"
// does, so this widget has one rule for both and the gate can mirror it.
//
// Repaints are throttled by MATERIAL CHANGE, not by the tick: setPose()
// update()s only when some component moved by more than kEpsilon. A ~30 Hz
// meterTick over a static playhead would otherwise repaint a stick figure
// that is not moving.
class SFeelFlowPuppetWidget : public QWidget {
    Q_OBJECT
public:
    explicit SFeelFlowPuppetWidget( QWidget *parent = nullptr );

    // Stores the pose and update()s only if it MATERIALLY changed (any
    // component, or validity, moved). Cheap enough to call from every tick.
    void setPose( const SFeelFlowPose &pose );

    const SFeelFlowPose &pose() const { return pose_; }

    /** Where the wireframe is viewed from. The MODEL is 3D and the camera is
     * display, so this changes nothing a gate asserts -- which is why it is a
     * plain setter with no action, no undo and no persistence. There is no UI
     * for it yet; the default is a three-quarter view because a straight-on one
     * foreshortens every sagittal motion to nothing, which is what made the
     * shipped 2D figure unable to show a forward bend at all. */
    void setCamera( const SFeelFlowCamera &cam ) { camera_ = cam; update(); }
    const SFeelFlowCamera &camera() const { return camera_; }

    QSize sizeHint() const override { return QSize( 220, 260 ); }
    QSize minimumSizeHint() const override { return QSize( 80, 100 ); }

    // Any single component moving by more than this counts as a material
    // change. 0.001 of a normalized joint excursion is well under a pixel on
    // any plausible dock size.
    static constexpr float kEpsilon = 0.001f;

protected:
    void paintEvent( QPaintEvent * ) override;

private:
    SFeelFlowPose   pose_;
    SFeelFlowCamera camera_;
};

#endif // SFEELFLOWPUPPET_H
