#ifndef STRACKDETAILPANEL_H
#define STRACKDETAILPANEL_H

#include <QWidget>

#include "tw/metering/tw_level_probe.h"

class STrack;
class SLevelMeter;
class SPluginEffectStrip;
class QVBoxLayout;
class QSlider;
class QLabel;

// Track detail panel: shows plugin grid and large volume control.
// Lives in the main window's "Track Detail" dock (left area, below the extern
// file list) — it carries no title row or collapse button of its own, because
// the dock's title bar already names it and the View menu / its close button
// already show and hide it.
class STrackDetailPanel : public QWidget {
    Q_OBJECT
public:
    STrackDetailPanel(QWidget *parent = nullptr);
    ~STrackDetailPanel();

    // Set the track to display (nullptr hides content)
    void setTrack(STrack *track);

    // Size constraint (50% of screen, max 450px; small when empty)
    QSize sizeHint() const override;

protected:
    // A QWidget subclass must draw its own style-sheet background.
    void paintEvent(QPaintEvent *) override;
    // Watches volumeSlider_ for Home/End (item j): forwarded to the transport
    // instead of letting QAbstractSlider jump the fader to its min/max. See
    // SSMVMixerControl::eventFilter, which does the same thing for the
    // arranger track head's fader.
    bool eventFilter( QObject *watched, QEvent *event ) override;

private slots:
    // Fader drag -> track volume, through the action system (undoable).
    void onVolumeSliderMoved( int sliderValue );
    // Proposal 34: one metering tick for the selected track.
    void onMeterTick( offset_t pos, qint64 nowMs, bool live );

private:
    void rebuildUI();
    // Index of currentTrack_ in the mixer, or -1.

    STrack *currentTrack_ = nullptr;

    // UI components
    QWidget *contentWidget_;
    QVBoxLayout *contentLayout_;
    SPluginEffectStrip *pluginStrip_;
    QSlider *volumeSlider_;
    QLabel *volumeLabel_;
    QLabel *placeholder_;   // shown instead of the content when no track is set
    SLevelMeter *meter_ = nullptr;   // proposal 34
    twLevelProbe probe_;             // re-bound whenever the track changes
};

#endif
