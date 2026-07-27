#include "app/timeline/strackheaderresizer.h"
#include "app/timeline/sstdmixerview.h"
#include <QMouseEvent>
#include <QCursor>
#include <QPainter>

// The divider is a plain QWidget subclass, so a style sheet would not reach the
// screen (Qt only paints one for widgets that draw PE_Widget themselves) while
// still suppressing the palette fill — the strip then keeps whatever was last
// in the backing store. Painting it directly is both correct and cheaper.
static const QColor kIdleFill( 0x44, 0x44, 0x44 );
static const QColor kHotFill ( 0x55, 0x55, 0x55 );

STrackHeaderResizer::STrackHeaderResizer(SStdMixerView *mixerView, QWidget *parent)
    : QWidget(parent), mixerView_(mixerView)
{
    setFixedWidth(8);
    setCursor(Qt::SizeHorCursor);
}

void STrackHeaderResizer::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), hovered_ || dragging_ ? kHotFill : kIdleFill);
    p.setPen(QColor(0x33, 0x33, 0x33));
    p.drawLine(0, 0, 0, height() - 1);
    p.setPen(QColor(hovered_ || dragging_ ? 0x66 : 0x55,
                    hovered_ || dragging_ ? 0x66 : 0x55,
                    hovered_ || dragging_ ? 0x66 : 0x55));
    p.drawLine(width() - 1, 0, width() - 1, height() - 1);
}

void STrackHeaderResizer::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        dragging_ = true;
        update();
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
        update();
        event->accept();
    }
}

void STrackHeaderResizer::enterEvent(QEnterEvent *event)
{
    setCursor(Qt::SizeHorCursor);
    hovered_ = true;
    update();
}

void STrackHeaderResizer::leaveEvent(QEvent *event)
{
    hovered_ = false;
    update();
}
