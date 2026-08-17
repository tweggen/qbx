#ifndef _SEVENTTIMEAXIS_H_
#define _SEVENTTIMEAXIS_H_

#include <QObject>

#include "tw/core/twtypes.h"

/**
 * SEventTimeAxis - the event editor's horizontal time axis (proposal 37 6.2).
 *
 * ONE px<->frame conversion, and it is deliberately the SAME arithmetic
 * SMVActualView::getXPosOfOffset / getTimeOf use (timeline invariant 4: pixel
 * <-> frame conversion is owned by the view's zoom state, and there is one of
 * it). Spelling a second, "nicer" conversion here would put the piano roll's
 * notes half a pixel away from the arranger's clip that contains them at some
 * zooms, which is exactly the kind of drift a linked axis exists to prevent.
 *
 * The state is therefore the arranger's own state: `secondWidth` (pixels per
 * SECOND) and `leftPixels` (the pixel origin, i.e. the scroll position already
 * expressed in pixels - NOT frames; `SMVActualView::upperLeftX_` is a pixel
 * count and `leftOffsetChanged` carries the frame it was derived from).
 *
 * LINKED (the default) means the shell pushes the arranger's zoom/scroll in on
 * every change, so the editor scrolls with the arrangement. Unlinked, the dock
 * keeps whatever it was last given and the user zooms the editor alone. The
 * flag lives here rather than in the dock because it is a property of the axis,
 * and `describe()` reports it (`linked=0/1`).
 *
 * Plain QObject: no widget, no painting, no model access. Everything that
 * draws over time - the ruler, the piano roll, the velocity lane, the CC lanes
 * - shares one instance, which is what keeps them column-aligned.
 */
class SEventTimeAxis : public QObject
{
    Q_OBJECT
public:
    explicit SEventTimeAxis( QObject *parent = nullptr );

    double   secondWidth() const { return secondWidth_; }
    int      leftPixels() const { return leftPixels_; }
    int      sampleRate() const { return sampleRate_; }
    bool     linked() const { return linked_; }

    /** The frame at the axis origin (x = 0). Derived, never stored. */
    offset_t leftFrame() const { return frameOfX( 0 ); }

    // The two conversions. Mirrors of SMVActualView's, integer-truncating in
    // exactly the same places.
    int      xOfFrame( offset_t frame ) const;
    offset_t frameOfX( int x ) const;

public slots:
    void setSecondWidth( double pixelsPerSecond );
    void setLeftPixels( int px );
    /** Scroll so that `frame` sits at x = 0 (what the arranger's signal says). */
    void setLeftFrame( offset_t frame );
    void setSampleRate( int srate );
    void setLinked( bool linked );

signals:
    /** Any of the four above moved; every consumer just repaints. */
    void axisChanged();

private:
    double secondWidth_ = 20.0;   // px per second (SStdMixerView's default)
    int    leftPixels_  = 0;
    int    sampleRate_  = 48000;
    bool   linked_      = true;
};

#endif // _SEVENTTIMEAXIS_H_
