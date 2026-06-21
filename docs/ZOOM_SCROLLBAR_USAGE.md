# SZoomScrollBar: Usage Guide

A custom Qt scrollbar with zoom control at the ends. Drag the tiny zones at each end to zoom; drag the middle for normal scrolling.

## Features

- **End-zone zoom control:** Drag the first/last 8% (configurable) of the scrollbar to zoom in/out
- **Normal scrolling:** Drag the middle portion to scroll normally
- **Visual feedback:** End zones highlight on hover or during zoom interactions
- **Cross-platform:** Works consistently on Windows, Linux, macOS
- **Qt signal integration:** Emits `zoomRequested(factor)` for zoom events

## API

```cpp
SZoomScrollBar* scrollbar = new SZoomScrollBar(Qt::Horizontal, parent);

// Configure zone size (default: 8%)
scrollbar->setZoneSize(0.10);  // 10% of scrollbar on each end

// Configure zoom sensitivity (default: 20 pixels per zoom step)
scrollbar->setZoomSensitivity(15.0);  // faster zoom on smaller movements

// Connect to zoom events
connect(scrollbar, &SZoomScrollBar::zoomRequested, this, [](double factor) {
    // factor > 1.0 = zoom in, factor < 1.0 = zoom out
    myView->scale(factor, factor);
});

// Use like a normal QScrollBar for scrolling
scrollbar->setMinimum(0);
scrollbar->setMaximum(1000);
scrollbar->setValue(500);
```

## How It Works

### Interaction Zones

The scrollbar is divided into three zones:

```
|← Zoom Start →|←─── Scroll Zone ───→|← Zoom End →|
```

- **Zoom Start (left/top 8%):** Drag up/left to zoom out, down/right to zoom in
- **Scroll Zone (middle):** Normal scrollbar behavior
- **Zoom End (right/bottom 8%):** Drag up/left to zoom out, down/right to zoom in

### Visual Feedback

- End zones highlight with the highlight color when hovered
- Color alpha is low (80/255) to preserve visibility of the scrollbar thumb
- Feedback updates in real-time during interaction

### Zoom Calculation

Dragging generates a zoom factor based on pixel distance and sensitivity:

```cpp
double factor = 1.0 + (pixel_distance / sensitivity);
factor = clamp(factor, 0.5, 2.0);  // Limit zoom range
```

Positive pixel movement (right/down) zooms in; negative (left/up) zooms out.

## Integration Example

```cpp
// In a custom scroll area or view widget

class MyTimelineView : public QWidget {
    Q_OBJECT
public:
    MyTimelineView(QWidget* parent = nullptr) : QWidget(parent) {
        auto layout = new QVBoxLayout(this);
        
        // Scrollable content area
        auto scrollArea = new QScrollArea;
        scrollArea->setWidget(new TimelineContent);
        layout->addWidget(scrollArea);
        
        // Custom zoom scrollbar below
        scrollbar_ = new SZoomScrollBar(Qt::Horizontal);
        layout->addWidget(scrollbar_);
        
        connect(scrollbar_, &SZoomScrollBar::zoomRequested, this, 
                &MyTimelineView::onZoom);
        
        // Wire scrollbar to scroll area
        connect(scrollbar_, QOverload<int>::of(&QScrollBar::valueChanged),
                scrollArea->horizontalScrollBar(), 
                &QScrollBar::setValue);
    }

private slots:
    void onZoom(double factor) {
        currentZoom_ *= factor;
        updateTimelineScale();
        updateScrollbar();
    }

private:
    SZoomScrollBar* scrollbar_;
    double currentZoom_ = 1.0;
};
```

## Architecture Notes

- Inherits from `QScrollBar`, so it's a drop-in replacement with bonus zoom features
- Overrides `mousePressEvent()`, `mouseMoveEvent()`, `mouseReleaseEvent()` for interaction
- Overrides `paintEvent()` to draw zone highlights with `QPainter`
- Uses `QStyleOptionSlider` to query platform-specific scrollbar dimensions
- Maintains separate state for scrolling vs. zooming (prevents mode confusion)
- All geometry calculations are relative to scrollbar length, so they adapt to platform/style
- `setMouseTracking(true)` enables hover detection for visual feedback

## Platform Portability

✅ Windows (native WASAPI, Win32 look)
✅ Linux (ALSA, Fusion/native look)
✅ macOS (CoreAudio, macOS look)

The implementation uses pure Qt and no platform-specific APIs, so it works uniformly across all platforms. Scrollbar rendering respects the native style (Windows theme, macOS native, Fusion, etc.), while the zoom zones are painted with `QPainter` in a consistent way.

## Tuning

Adjust these properties for different use cases:

```cpp
// Fine, precise zooming (small movements = big changes)
scrollbar->setZoomSensitivity(5.0);

// Coarse, deliberate zooming (large movements = small changes)
scrollbar->setZoomSensitivity(50.0);

// Larger zoom zones (easier to hit)
scrollbar->setZoneSize(0.15);

// Smaller zones (more screen real estate for scrolling)
scrollbar->setZoneSize(0.05);
```

## Known Limitations

- Touch gestures (pinch-zoom) are not yet integrated; only mouse drag
- No haptic feedback (future enhancement)
- Zoom zones are uniform across the bar; could be asymmetric (e.g., larger on one end) if needed
