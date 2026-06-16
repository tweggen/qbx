#include "strackheaderresizer.h"
#include "sstdmixerview.h"
#include <QMouseEvent>
#include <QCursor>

STrackHeaderResizer::STrackHeaderResizer(SStdMixerView *mixerView, QWidget *parent)
    : QWidget(parent), mixerView_(mixerView)
{
    setFixedWidth(8);
    setStyleSheet("QWidget { background-color: #444; border-left: 1px solid #333; border-right: 1px solid #555; }");
    setCursor(Qt::SizeHorCursor);
}

void STrackHeaderResizer::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        dragging_ = true;
        event->accept();
    }
}

void STrackHeaderResizer::mouseMoveEvent(QMouseEvent *event)
{
    if (dragging_ && mixerView_) {
        // Get the global x position and convert to mixer view coordinates
        int globalX = mapToGlobal(event->pos()).x();
        int mixerX = mixerView_->mapFromGlobal(QPoint(globalX, 0)).x();

        // Clamp to valid range (120-450)
        mixerX = qBound(120, mixerX, 450);
        mixerView_->setTrackControlWidth(mixerX);
        event->accept();
    }
}

void STrackHeaderResizer::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        dragging_ = false;
        event->accept();
    }
}

void STrackHeaderResizer::enterEvent(QEnterEvent *event)
{
    setCursor(Qt::SizeHorCursor);
    setStyleSheet("QWidget { background-color: #555; border-left: 1px solid #333; border-right: 1px solid #666; }");
}

void STrackHeaderResizer::leaveEvent(QEvent *event)
{
    if (!dragging_) {
        setStyleSheet("QWidget { background-color: #444; border-left: 1px solid #333; border-right: 1px solid #555; }");
    }
}
