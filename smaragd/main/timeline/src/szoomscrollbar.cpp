#include "app/timeline/szoomscrollbar.h"

#include <QMouseEvent>
#include <QPainter>
#include <QStyleOptionSlider>
#include <cmath>

SZoomScrollBar::SZoomScrollBar(Qt::Orientation orientation, QWidget* parent)
    : QScrollBar(orientation, parent),
      zoneSize_(0.08),
      zoomSensitivity_(20.0),
      currentMode_(None),
      dragStartPos_(0),
      lastZoomPos_(0),
      hoverZoneType_(0) {
  setMouseTracking(true);
}

void SZoomScrollBar::setZoneSize(double fraction) {
  zoneSize_ = std::clamp(fraction, 0.01, 0.25);
  update();
}

void SZoomScrollBar::setZoomSensitivity(double pixels_per_zoom_step) {
  zoomSensitivity_ = std::max(pixels_per_zoom_step, 1.0);
}

int SZoomScrollBar::getBarLength() const {
  return orientation() == Qt::Horizontal ? width() : height();
}

SZoomScrollBar::InteractionMode SZoomScrollBar::detectInteractionMode(int pos)
    const {
  int barLength = getBarLength();
  int zonePixels = std::max(1, static_cast<int>(barLength * zoneSize_));

  if (pos < zonePixels) {
    return ZoomingStart;
  }
  if (pos > barLength - zonePixels) {
    return ZoomingEnd;
  }
  return Scrolling;
}

void SZoomScrollBar::mousePressEvent(QMouseEvent* event) {
  int pos = orientation() == Qt::Horizontal ? event->pos().x() : event->pos().y();
  currentMode_ = detectInteractionMode(pos);
  dragStartPos_ = pos;
  lastZoomPos_ = pos;

  if (currentMode_ == Scrolling) {
    QScrollBar::mousePressEvent(event);
  }
}

void SZoomScrollBar::mouseMoveEvent(QMouseEvent* event) {
  int pos = orientation() == Qt::Horizontal ? event->pos().x() : event->pos().y();

  if (currentMode_ == Scrolling) {
    QScrollBar::mouseMoveEvent(event);
  } else if (currentMode_ == ZoomingStart || currentMode_ == ZoomingEnd) {
    int delta = pos - lastZoomPos_;
    if (delta != 0) {
      double factor = 1.0 + (static_cast<double>(delta) / zoomSensitivity_);
      factor = std::clamp(factor, 0.5, 2.0);
      lastZoomPos_ = pos;
      emit zoomRequested(factor);
    }
  } else {
    updateHoverZone(pos);
    QScrollBar::mouseMoveEvent(event);
  }
}

void SZoomScrollBar::mouseReleaseEvent(QMouseEvent* event) {
  if (currentMode_ == Scrolling) {
    QScrollBar::mouseReleaseEvent(event);
  }
  currentMode_ = None;
  update();
}

void SZoomScrollBar::updateHoverZone(int pos) {
  int barLength = getBarLength();
  int zonePixels = std::max(1, static_cast<int>(barLength * zoneSize_));
  int newHoverType = 0;

  if (pos < zonePixels) {
    newHoverType = 1;
  } else if (pos > barLength - zonePixels) {
    newHoverType = -1;
  }

  if (newHoverType != hoverZoneType_) {
    hoverZoneType_ = newHoverType;
    update();
  }
}

void SZoomScrollBar::enterEvent(QEnterEvent* event) {
  int pos = orientation() == Qt::Horizontal ? event->pos().x() : event->pos().y();
  updateHoverZone(pos);
  QScrollBar::enterEvent(event);
}

void SZoomScrollBar::leaveEvent(QEvent* event) {
  hoverZoneType_ = 0;
  update();
  QScrollBar::leaveEvent(event);
}

void SZoomScrollBar::paintEvent(QPaintEvent* event) {
  QScrollBar::paintEvent(event);

  if (hoverZoneType_ == 0 && currentMode_ == None) {
    return;
  }

  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);

  int barLength = getBarLength();
  int zonePixels = std::max(1, static_cast<int>(barLength * zoneSize_));
  QColor hoverColor = palette().highlight().color();
  hoverColor.setAlpha(80);

  if (orientation() == Qt::Horizontal) {
    if (hoverZoneType_ == 1 || currentMode_ == ZoomingStart) {
      painter.fillRect(0, 0, zonePixels, height(), hoverColor);
    }
    if (hoverZoneType_ == -1 || currentMode_ == ZoomingEnd) {
      painter.fillRect(width() - zonePixels, 0, zonePixels, height(),
                       hoverColor);
    }
  } else {
    if (hoverZoneType_ == 1 || currentMode_ == ZoomingStart) {
      painter.fillRect(0, 0, width(), zonePixels, hoverColor);
    }
    if (hoverZoneType_ == -1 || currentMode_ == ZoomingEnd) {
      painter.fillRect(0, height() - zonePixels, width(), zonePixels,
                       hoverColor);
    }
  }
}
