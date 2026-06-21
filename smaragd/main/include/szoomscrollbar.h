#pragma once

#include <QScrollBar>

class SZoomScrollBar : public QScrollBar {
  Q_OBJECT

 public:
  explicit SZoomScrollBar(Qt::Orientation orientation, QWidget* parent = nullptr);

  void setZoneSize(double fraction);
  double zoneSize() const { return zoneSize_; }

  void setZoomSensitivity(double pixels_per_zoom_step);
  double zoomSensitivity() const { return zoomSensitivity_; }

 signals:
  void zoomRequested(double factor);

 protected:
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void paintEvent(QPaintEvent* event) override;
  void enterEvent(QEnterEvent* event) override;
  void leaveEvent(QEvent* event) override;

 private:
  enum InteractionMode { None, Scrolling, ZoomingStart, ZoomingEnd };

  int getBarLength() const;
  InteractionMode detectInteractionMode(int pos) const;
  void updateHoverZone(int pos);

  double zoneSize_;
  double zoomSensitivity_;
  InteractionMode currentMode_;
  int dragStartPos_;
  int lastZoomPos_;
  int hoverZoneType_;  // -1: end zone, 0: none, 1: start zone
};
