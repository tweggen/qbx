#ifndef STRACKHEADERRESIZER_H
#define STRACKHEADERRESIZER_H

#include <QWidget>

class SStdMixerView;

// Draggable divider between track header and content area
class STrackHeaderResizer : public QWidget {
    Q_OBJECT
public:
    STrackHeaderResizer(SStdMixerView *mixerView, QWidget *parent = nullptr);

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    SStdMixerView *mixerView_;
    bool dragging_ = false;
};

#endif
