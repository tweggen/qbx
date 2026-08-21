#ifndef SBROWNPROSTYLE_H
#define SBROWNPROSTYLE_H

#include <QProxyStyle>

#include "app/theme/sthemecolors.h"

class QPainterPath;

// Smaragd's own widget style: a warm, compact, dark look that is IDENTICAL on
// Windows, macOS and Linux.
//
// THE BASE IS FUSION, NOT QCommonStyle, and that is the load-bearing decision
// here. QCommonStyle is a partial implementation -- it is the shared arithmetic
// (sub-control rects, layout metrics) that concrete styles are built ON, not a
// style you can ship. Deriving from it means every element you have not written
// yet paints as an unshaded rectangle or not at all, so a style that covers
// eight elements makes the other two hundred WORSE than the platform default,
// and there is no point on the way from here to "all required widgets" at which
// the app looks finished. Fusion is Qt's own cross-platform style, present in
// every Qt build we ship against, it draws almost everything out of QPalette,
// and it is what the elements below are progressively REPLACING. Coverage can
// therefore grow one element at a time with the app looking whole at every
// commit.
//
// What is overridden here, and what is still Fusion's, is listed in
// main/theme/CONTRACT.md. Add to that list when you add an element.
//
// Two things it deliberately does NOT do:
//   - it does not paint the arranger. Clips, lanes, waveforms, meters and the
//     ruler are custom paintEvent() code with their own colours (proposal 34
//     and 39 gate some of those pixels); a QStyle is the CHROME around them.
//   - it does not override a widget that carries its own setStyleSheet(). Qt
//     routes a styled widget through QStyleSheetStyle instead, so the handful
//     of stylesheet call sites in timeline/ and pluginui/ keep their own look
//     until they are migrated to these tokens.
class SBrownProStyle : public QProxyStyle
{
    Q_OBJECT

public:
    explicit SBrownProStyle( const SThemeColors &colors = SThemeColors::brownProDark() );

    const SThemeColors &colors() const { return colors_; }

    // --- QStyle -----------------------------------------------------------
    void polish( QPalette &palette ) override;
    void polish( QWidget *widget ) override;

    int pixelMetric( PixelMetric metric, const QStyleOption *opt,
                     const QWidget *widget ) const override;

    int styleHint( StyleHint hint, const QStyleOption *opt, const QWidget *widget,
                   QStyleHintReturn *ret ) const override;

    QSize sizeFromContents( ContentsType type, const QStyleOption *opt,
                            const QSize &contentsSize,
                            const QWidget *widget ) const override;

    QRect subControlRect( ComplexControl cc, const QStyleOptionComplex *opt,
                          SubControl sc, const QWidget *widget ) const override;

    void drawPrimitive( PrimitiveElement elem, const QStyleOption *opt,
                        QPainter *p, const QWidget *widget ) const override;

    void drawControl( ControlElement elem, const QStyleOption *opt,
                      QPainter *p, const QWidget *widget ) const override;

    void drawComplexControl( ComplexControl cc, const QStyleOptionComplex *opt,
                             QPainter *p, const QWidget *widget ) const override;

    // Corner radius of a control, in device-independent pixels. One number,
    // used by every rounded element, so the family resemblance is structural
    // rather than something that has to be kept in step by hand.
    static constexpr int kRadius = 3;

private:
    SThemeColors colors_;
};

#endif // SBROWNPROSTYLE_H
