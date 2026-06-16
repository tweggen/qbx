#ifndef STRACKDETAILPANEL_H
#define STRACKDETAILPANEL_H

#include <QWidget>

class STrack;
class SPluginEffectStrip;
class QVBoxLayout;
class QSlider;
class QLabel;
class QPushButton;

// Track detail panel: shows plugin grid and large volume control
// Lives in the bottom half of the mixer view, expandable/collapsible
class STrackDetailPanel : public QWidget {
    Q_OBJECT
public:
    STrackDetailPanel(QWidget *parent = nullptr);
    ~STrackDetailPanel();

    // Set the track to display (nullptr hides content)
    void setTrack(STrack *track);

    // Collapse/expand state
    bool isExpanded() const { return expanded_; }
    void setExpanded(bool expanded);

    // Persist state
    void saveState();
    void loadState();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    void rebuildUI();
    void updateCollapsedState();

    STrack *currentTrack_ = nullptr;
    bool expanded_ = true;

    // UI components
    QPushButton *expandCollapseBtn_;
    QWidget *contentWidget_;
    QVBoxLayout *contentLayout_;
    SPluginEffectStrip *pluginStrip_;
    QSlider *volumeSlider_;
    QLabel *volumeLabel_;
};

#endif
