#ifndef SFEELFLOWPANEL_H
#define SFEELFLOWPANEL_H

#include <QWidget>

#include "app/objects/track/strack.h"

class QLabel;
class QPushButton;
class QComboBox;
class QListWidget;
class QListWidgetItem;
class SFeelFlowMetricStrip;

// Proposal 40 "Feel Flow" M3 -- the Track Detail dock's "Feel Flow" section
// (design section 4.4's "tuning device" readout + controls). Mounted from
// STrackDetailPanel exactly like SPluginEffectStrip: one member pointer,
// deleted and re-created per track switch in rebuildUI() (rebuildUI() OWNS
// NOTHING long-lived across a track switch -- the holder this reads lives on
// the TRACK, main/objects/track/CONTRACT.md's own rule for this kind of
// section).
//
// Everything this widget shows is a READ, pumped from SApplication::
// meterTick exactly like the fader's P6 read-value display and the level
// meters (CLAUDE.md's "Playhead-pumped readouts" note): never blocks, never
// demands, never touches the sidecar store beyond STrack::feelFlowForUi()'s
// own lock-free cached read. No epoch is ever bumped from here.
//
// describe() is the SAME computation the on-screen labels are built from
// (never a second implementation that could disagree with what is drawn),
// which is what lets SMainWindow::describeFeelFlow()/grabFeelFlow() build
// one of these off screen (parentless, never shown) and get an honest
// answer synchronously -- the assert-track-head precedent, not the
// assert-midi-options one (a single widget, not a whole dialog page).
class SFeelFlowPanel : public QWidget {
    Q_OBJECT
public:
    explicit SFeelFlowPanel( STrack *track, QWidget *parent = nullptr );
    ~SFeelFlowPanel();

    // A `state=... mode=... trained=0/1 [compliance=... units=... lean=...
    // drive=...]` line -- see the .cpp for the exact grammar. Never blocks;
    // reads the SAME feelFlowForUi()/feelFlowStale() calls the labels do.
    QString describe() const;

private slots:
    void onAnalyzeClicked();
    void onLearnClicked();
    void onModeChanged( int index );
    void onBandMetricToggled( QListWidgetItem *item );
    void onMeterTick( offset_t pos, qint64 nowMs, bool live );

private:
    void refresh();   // re-reads track_ and rewrites every label/control

    STrack     *track_ = nullptr;   // not owned
    QLabel     *stateLabel_    = nullptr;
    QLabel     *complianceLabel_ = nullptr;
    QLabel     *unitsLabel_    = nullptr;
    QLabel     *tensionLabel_  = nullptr;
    QPushButton *analyzeButton_ = nullptr;
    QPushButton *learnButton_   = nullptr;
    QComboBox   *modeCombo_     = nullptr;

    // --- proposal 40 M3b/M3d: the metric lab ---------------------------
    // The stacked strip (one heatmap row per derived series, defined in
    // the .cpp) and the band-metric selector -- since M3d a compact
    // CHECK-LIST (one checkable row per series), so a COUPLE of metrics
    // can ride the arranger band as stacked sub-rows. Toggling writes the
    // track's RUNTIME feelFlowBandMetricId() (comma-joined, in series
    // order) -- a view preference, a plain call, never an action.
    SFeelFlowMetricStrip *metricStrip_ = nullptr;
    QListWidget          *bandList_    = nullptr;

    // True while onModeChanged() writes modeCombo_ from the model, so the
    // resulting currentIndexChanged never turns a refresh into a new action.
    bool applyingExternal_ = false;
};

#endif // SFEELFLOWPANEL_H
