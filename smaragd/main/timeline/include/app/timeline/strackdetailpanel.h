#ifndef STRACKDETAILPANEL_H
#define STRACKDETAILPANEL_H

#include <QWidget>

class STrack;
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

    // Size constraint (50% of screen, max 450px)
    QSize sizeHint() const override;
    int heightForWidth(int w) const override;

private:
    void rebuildUI();

    STrack *currentTrack_ = nullptr;

    // UI components
    QWidget *contentWidget_;
    QVBoxLayout *contentLayout_;
    SPluginEffectStrip *pluginStrip_;
    QSlider *volumeSlider_;
    QLabel *volumeLabel_;
};

#endif
