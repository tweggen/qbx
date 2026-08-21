#include "app/theme/sbrownprostyle.h"

#include <QAbstractButton>
#include <QAbstractSpinBox>
#include <QComboBox>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QScrollBar>
#include <QSlider>
#include <QStyleFactory>
#include <QStyleOption>
#include <QTabBar>
#include <QToolButton>

#include <QtGlobal>

namespace {

// A 1 px stroke lands on a pixel BOUNDARY, so an integer rect is painted half
// into each neighbouring pixel and reads as a 2 px blur. Every outline here is
// therefore stroked on the half-pixel inset.
inline QRectF crisp( const QRect &r )
{
    return QRectF( r ).adjusted( 0.5, 0.5, -0.5, -0.5 );
}

inline QColor withAlpha( QColor c, int a )
{
    c.setAlpha( a );
    return c;
}

// The one fill used by every raised control: a very shallow vertical gradient.
// Shallow on purpose -- enough to say "this is a surface with a light above
// it", not enough to read as a 1990s bevel.
QLinearGradient raisedFill( const QRect &r, const QColor &c )
{
    QLinearGradient g( r.topLeft(), r.bottomLeft() );
    g.setColorAt( 0.0, c.lighter( 108 ) );
    g.setColorAt( 1.0, c.darker( 104 ) );
    return g;
}

void fillRounded( QPainter *p, const QRect &r, qreal radius, const QBrush &b )
{
    p->setPen( Qt::NoPen );
    p->setBrush( b );
    p->drawRoundedRect( QRectF( r ), radius, radius );
}

void strokeRounded( QPainter *p, const QRect &r, qreal radius,
                    const QColor &c, qreal width = 1.0 )
{
    p->setBrush( Qt::NoBrush );
    p->setPen( QPen( c, width ) );
    p->drawRoundedRect( crisp( r ), radius, radius );
}

// A chevron rather than a filled triangle: it is the one shape that reads the
// same at 8 px and at 24 px, which matters because the SAME primitive draws a
// combo box arrow, a spin button, a tree branch and a header sort indicator.
void drawChevron( QPainter *p, const QRect &r, Qt::ArrowType dir, const QColor &c )
{
    const qreal s = qMax( qreal( 2.0 ), qMin( r.width(), r.height() ) * 0.30 );
    QPolygonF poly;
    switch( dir ) {
        case Qt::UpArrow:
            poly << QPointF( -s, s * 0.5 ) << QPointF( 0, -s * 0.5 ) << QPointF( s, s * 0.5 );
            break;
        case Qt::LeftArrow:
            poly << QPointF( s * 0.5, -s ) << QPointF( -s * 0.5, 0 ) << QPointF( s * 0.5, s );
            break;
        case Qt::RightArrow:
            poly << QPointF( -s * 0.5, -s ) << QPointF( s * 0.5, 0 ) << QPointF( -s * 0.5, s );
            break;
        case Qt::DownArrow:
        default:
            poly << QPointF( -s, -s * 0.5 ) << QPointF( 0, s * 0.5 ) << QPointF( s, -s * 0.5 );
            break;
    }
    poly.translate( QRectF( r ).center() );
    p->setBrush( Qt::NoBrush );
    p->setPen( QPen( c, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin ) );
    p->drawPolyline( poly );
}

// RAII for the two things every element here wants, and no element wants to
// leak into the next one.
struct PainterGuard
{
    explicit PainterGuard( QPainter *p ) : p_( p )
    {
        p_->save();
        p_->setRenderHint( QPainter::Antialiasing, true );
    }
    ~PainterGuard() { p_->restore(); }
    QPainter *p_;
};

} // namespace


SBrownProStyle::SBrownProStyle( const SThemeColors &colors )
    // Fusion, not the platform style: the point of having a style at all is
    // that the app looks the SAME on all three platforms, and the platform
    // styles disagree about metrics as well as about paint (a Windows-style
    // combo box and a macOS one differ in height, so a layout tuned against one
    // clips against the other).
    : QProxyStyle( QStyleFactory::create( QStringLiteral( "Fusion" ) ) )
    , colors_( colors )
{
    setObjectName( QStringLiteral( "BrownPro" ) );
}


// ---------------------------------------------------------------- polish ----

void SBrownProStyle::polish( QPalette &palette )
{
    // The style carries its own palette, so installing it is enough -- a caller
    // does not have to remember QApplication::setPalette() as well, and
    // switching BACK to a platform style restores that style's palette instead
    // of leaving ours behind in QApplication.
    palette = colors_.palette();
}

void SBrownProStyle::polish( QWidget *widget )
{
    QProxyStyle::polish( widget );

    // State_MouseOver is only ever set for a widget carrying WA_Hover, so a
    // style that paints a hover state has to ask for the attribute. Fusion
    // already sets it for most of these; naming them here means the hover paint
    // does not depend on which base style is underneath.
    if( qobject_cast<QAbstractButton *>( widget ) ||
        qobject_cast<QComboBox *>( widget ) ||
        qobject_cast<QAbstractSpinBox *>( widget ) ||
        qobject_cast<QSlider *>( widget ) ||
        qobject_cast<QScrollBar *>( widget ) ||
        qobject_cast<QTabBar *>( widget ) ||
        qobject_cast<QLineEdit *>( widget ) ) {
        widget->setAttribute( Qt::WA_Hover, true );
    }
}


// --------------------------------------------------------- pixel metrics ----

int SBrownProStyle::pixelMetric( PixelMetric metric, const QStyleOption *opt,
                                 const QWidget *widget ) const
{
    switch( metric ) {
        // --- the sketch's compact metrics, kept verbatim -------------------
        case PM_DefaultFrameWidth:      return 1;
        case PM_ButtonMargin:           return 4;
        case PM_ScrollBarExtent:        return 12;
        case PM_SliderThickness:        return 12;
        case PM_TabBarTabHSpace:        return 8;
        case PM_TabBarTabVSpace:        return 4;

        // --- what those compact metrics imply elsewhere --------------------
        // A 12 px scrollbar with Fusion's 16 px minimum slider is a slider you
        // cannot grab in a long document; 28 is about a fingertip at 100 %.
        case PM_ScrollBarSliderMin:     return 28;
        case PM_SliderLength:           return 12;
        case PM_SliderControlThickness: return 12;

        // No default-button ring and no press-time content shift: both are
        // platform-look tells, and the press is already shown by the fill.
        case PM_ButtonDefaultIndicator: return 0;
        case PM_ButtonShiftHorizontal:  return 0;
        case PM_ButtonShiftVertical:    return 0;

        case PM_IndicatorWidth:           return 13;
        case PM_IndicatorHeight:          return 13;
        case PM_ExclusiveIndicatorWidth:  return 13;
        case PM_ExclusiveIndicatorHeight: return 13;
        case PM_CheckBoxLabelSpacing:     return 6;
        case PM_RadioButtonLabelSpacing:  return 6;

        case PM_MenuPanelWidth:         return 1;
        case PM_MenuBarPanelWidth:      return 0;
        case PM_MenuBarVMargin:         return 2;
        case PM_MenuBarHMargin:         return 2;
        case PM_MenuBarItemSpacing:     return 2;
        case PM_MenuVMargin:            return 4;
        case PM_MenuHMargin:            return 2;

        case PM_ToolBarFrameWidth:      return 1;
        case PM_ToolBarItemMargin:      return 1;
        case PM_ToolBarItemSpacing:     return 2;
        case PM_ToolBarSeparatorExtent: return 9;
        case PM_ToolBarHandleExtent:    return 9;

        case PM_DockWidgetTitleMargin:     return 3;
        case PM_DockWidgetSeparatorExtent: return 4;
        case PM_SplitterWidth:             return 4;

        case PM_TabBarTabShiftHorizontal: return 0;
        case PM_TabBarTabShiftVertical:   return 0;

        case PM_FocusFrameHMargin: return 1;
        case PM_FocusFrameVMargin: return 1;

        default:
            return QProxyStyle::pixelMetric( metric, opt, widget );
    }
}

int SBrownProStyle::styleHint( StyleHint hint, const QStyleOption *opt,
                               const QWidget *widget, QStyleHintReturn *ret ) const
{
    switch( hint ) {
        // A dark theme cannot afford Qt's legacy disabled-text effects: both
        // draw a LIGHT copy of the glyphs, which on this ground reads as a
        // rendering fault rather than as "unavailable". The Disabled palette
        // group carries that meaning instead.
        case SH_EtchDisabledText:   return 0;
        case SH_DitherDisabledText: return 0;

        case SH_Menu_MouseTracking:              return 1;
        case SH_MenuBar_MouseTracking:           return 1;
        case SH_ComboBox_ListMouseTracking:      return 1;
        case SH_ComboBox_Popup:                  return 0;  // never a native popup menu
        case SH_ItemView_ShowDecorationSelected: return 1;
        case SH_UnderlineShortcut:               return 1;

        // Click-anywhere-to-seek on a slider and on a scrollbar groove. A DAW
        // convention (a fader jumps to where you clicked) rather than the
        // desktop default of paging toward the click, and the one hint here
        // chosen for the application rather than for the look.
        case SH_Slider_AbsoluteSetButtons: return Qt::LeftButton;
        case SH_Slider_PageSetButtons:     return Qt::MiddleButton;
        case SH_ScrollBar_LeftClickAbsolutePosition: return 1;

        default:
            return QProxyStyle::styleHint( hint, opt, widget, ret );
    }
}

QSize SBrownProStyle::sizeFromContents( ContentsType type, const QStyleOption *opt,
                                        const QSize &contentsSize,
                                        const QWidget *widget ) const
{
    QSize s = QProxyStyle::sizeFromContents( type, opt, contentsSize, widget );

    // FLOORS only, never caps: a minimum keeps a compact metric from producing
    // a control too small to hit, while a cap would clip a long label or a
    // large font. Everything here is a floor.
    switch( type ) {
        case CT_PushButton:
            s.setHeight( qMax( s.height(), 22 ) );
            s.setWidth( qMax( s.width(), 64 ) );
            break;
        case CT_ComboBox:
        case CT_LineEdit:
        case CT_SpinBox:
            s.setHeight( qMax( s.height(), 22 ) );
            break;
        case CT_TabBarTab:
            s.setHeight( qMax( s.height(), 24 ) );
            break;
        case CT_HeaderSection:
            s.setHeight( qMax( s.height(), 20 ) );
            break;
        case CT_MenuBarItem:
            s.setHeight( qMax( s.height(), 22 ) );
            break;
        default:
            break;
    }
    return s;
}


// ------------------------------------------------------------ primitives ----

void SBrownProStyle::drawPrimitive( PrimitiveElement elem, const QStyleOption *opt,
                                    QPainter *p, const QWidget *widget ) const
{
    const bool enabled = opt->state & State_Enabled;
    const bool hovered = enabled && ( opt->state & State_MouseOver );
    const bool sunken  = opt->state & ( State_Sunken | State_On );
    const bool focused = opt->state & State_HasFocus;

    switch( elem ) {

        // --- buttons ------------------------------------------------------
        case PE_PanelButtonCommand:
        case PE_PanelButtonBevel: {
            PainterGuard g( p );
            // CHECKED is TINTED, not merely darkened. A transport, a mute or a
            // solo has to be readable across the room, and checked-vs-pressed
            // has to be distinguishable at a glance -- with both mapped onto the
            // grey ramp they differ by one step of lightness, which the widget
            // gallery showed is no difference at all.
            const bool checked = opt->state & State_On;
            QColor fill = colors_.surface;
            if( !enabled )     fill = colors_.surfaceDisabled;
            else if( checked ) fill = ( opt->state & State_Sunken )
                                          ? colors_.accentMuted.lighter( 120 )
                                          : colors_.accentMuted;
            else if( sunken )  fill = colors_.pressed;
            else if( hovered ) fill = colors_.hover;

            fillRounded( p, opt->rect, kRadius,
                         enabled ? QBrush( raisedFill( opt->rect, fill ) )
                                 : QBrush( fill ) );

            QColor line = colors_.border;
            if( !enabled )                          line = colors_.divider;
            else if( checked )                      line = colors_.accent;
            else if( focused )                      line = colors_.accent;
            else if( hovered )                      line = colors_.borderStrong;
            strokeRounded( p, opt->rect, kRadius, line );
            break;
        }

        case PE_PanelButtonTool: {
            PainterGuard g( p );
            // A tool button is FLAT at rest: a toolbar of framed boxes is
            // noise. It grows a surface only when it is hovered, held or on.
            if( !enabled ) break;
            if( opt->state & State_On ) {
                fillRounded( p, opt->rect, kRadius, colors_.accentMuted );
                strokeRounded( p, opt->rect, kRadius, colors_.accent );
            } else if( opt->state & State_Sunken ) {
                fillRounded( p, opt->rect, kRadius, colors_.pressed );
            } else if( hovered ) {
                fillRounded( p, opt->rect, kRadius, colors_.hover );
                strokeRounded( p, opt->rect, kRadius, colors_.border );
            }
            break;
        }

        // --- check and radio ------------------------------------------------
        case PE_IndicatorCheckBox: {
            PainterGuard g( p );
            const QRect box = opt->rect;
            fillRounded( p, box, 2, enabled ? colors_.surfaceSunken : colors_.surfaceDisabled );

            const bool on   = opt->state & State_On;
            const bool tri  = opt->state & State_NoChange;
            QColor line = colors_.border;
            if( !enabled )                line = colors_.divider;
            else if( on || tri )          line = colors_.accent;
            else if( hovered || focused ) line = colors_.borderStrong;
            strokeRounded( p, box, 2, line );

            if( on ) {
                // A drawn tick, not a filled square: at 13 px a square says
                // "partially checked" to anyone who has seen a tri-state box.
                const QRectF r( box );
                QPolygonF tick;
                tick << QPointF( r.left()  + r.width() * 0.24, r.top() + r.height() * 0.52 )
                     << QPointF( r.left()  + r.width() * 0.44, r.top() + r.height() * 0.72 )
                     << QPointF( r.left()  + r.width() * 0.78, r.top() + r.height() * 0.30 );
                p->setBrush( Qt::NoBrush );
                p->setPen( QPen( enabled ? colors_.accent : colors_.textDisabled,
                                 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin ) );
                p->drawPolyline( tick );
            } else if( tri ) {
                QRect bar = box.adjusted( 3, 0, -3, 0 );
                bar.setTop( box.center().y() - 1 );
                bar.setHeight( 2 );
                p->setPen( Qt::NoPen );
                p->setBrush( enabled ? colors_.accent : colors_.textDisabled );
                p->drawRect( bar );
            }
            break;
        }

        case PE_IndicatorRadioButton: {
            PainterGuard g( p );
            const QRectF box = crisp( opt->rect );
            p->setPen( Qt::NoPen );
            p->setBrush( enabled ? colors_.surfaceSunken : colors_.surfaceDisabled );
            p->drawEllipse( box );

            QColor line = colors_.border;
            if( !enabled )                     line = colors_.divider;
            else if( opt->state & State_On )   line = colors_.accent;
            else if( hovered || focused )      line = colors_.borderStrong;
            p->setBrush( Qt::NoBrush );
            p->setPen( QPen( line, 1.0 ) );
            p->drawEllipse( box );

            if( opt->state & State_On ) {
                p->setPen( Qt::NoPen );
                p->setBrush( enabled ? colors_.accent : colors_.textDisabled );
                p->drawEllipse( box.adjusted( 3.5, 3.5, -3.5, -3.5 ) );
            }
            break;
        }

        // --- text entry -------------------------------------------------------
        case PE_PanelLineEdit: {
            PainterGuard g( p );
            fillRounded( p, opt->rect, kRadius,
                         enabled ? colors_.surfaceSunken : colors_.surfaceDisabled );
            // The FRAME is drawn by PE_FrameLineEdit, which Qt sends straight
            // after this one for a framed line edit -- drawing an outline here
            // too would double-stroke it a shade darker.
            break;
        }

        case PE_FrameLineEdit: {
            PainterGuard g( p );
            QColor line = colors_.border;
            if( !enabled )     line = colors_.divider;
            else if( focused ) line = colors_.accent;
            else if( hovered ) line = colors_.borderStrong;
            strokeRounded( p, opt->rect, kRadius, line );
            break;
        }

        // --- frames ------------------------------------------------------------
        case PE_Frame:
        case PE_FrameGroupBox:
        case PE_FrameTabWidget:
        case PE_FrameDockWidget:
        case PE_FrameStatusBarItem: {
            PainterGuard g( p );
            strokeRounded( p, opt->rect, kRadius, colors_.border );
            break;
        }

        case PE_FrameFocusRect: {
            // One accent hairline, never Qt's dotted XOR rectangle -- which is
            // drawn by inverting the ground and therefore looks like damage on
            // a coloured surface.
            if( const auto *f = qstyleoption_cast<const QStyleOptionFocusRect *>( opt ) ) {
                if( f->state & State_KeyboardFocusChange ) {
                    PainterGuard g( p );
                    strokeRounded( p, opt->rect, kRadius, withAlpha( colors_.accent, 200 ) );
                }
            }
            break;
        }

        // --- item views ----------------------------------------------------------
        case PE_PanelItemViewItem:
        case PE_PanelItemViewRow: {
            const auto *vi = qstyleoption_cast<const QStyleOptionViewItem *>( opt );
            const bool selected = opt->state & State_Selected;
            if( !selected && !hovered ) {
                // Let the base paint alternating rows and anything else it
                // knows about; we only take over the two INTERACTIVE states.
                QProxyStyle::drawPrimitive( elem, opt, p, widget );
                break;
            }
            PainterGuard g( p );
            QColor fill = selected
                ? ( ( opt->state & State_Active ) ? colors_.accentMuted
                                                  : withAlpha( colors_.accentMuted, 150 ) )
                : withAlpha( colors_.hover, 140 );
            fillRounded( p, opt->rect, 2, fill );
            if( selected ) {
                // A 2 px accent bar on the leading edge, so a selected row is
                // still identifiable when the window is not focused.
                QRect bar = opt->rect;
                bar.setWidth( 2 );
                p->setPen( Qt::NoPen );
                p->setBrush( colors_.accent );
                p->drawRect( bar );
            }
            Q_UNUSED( vi );
            break;
        }

        // --- menus -----------------------------------------------------------------
        case PE_PanelMenu:
        case PE_FrameMenu: {
            PainterGuard g( p );
            fillRounded( p, opt->rect, kRadius, colors_.surfaceRaised );
            strokeRounded( p, opt->rect, kRadius, colors_.borderStrong );
            break;
        }

        // --- toolbars ----------------------------------------------------------------
        case PE_PanelToolBar: {
            p->fillRect( opt->rect, colors_.surfaceRaised );
            break;
        }

        case PE_IndicatorToolBarSeparator: {
            const QRect r = opt->rect;
            const bool horiz = !( opt->state & State_Horizontal );
            p->setPen( colors_.divider );
            if( horiz )
                p->drawLine( r.center().x(), r.top() + 3, r.center().x(), r.bottom() - 3 );
            else
                p->drawLine( r.left() + 3, r.center().y(), r.right() - 3, r.center().y() );
            break;
        }

        case PE_IndicatorToolBarHandle: {
            // Two dotted columns: a grip that is legible at 9 px and does not
            // need an image resource on any platform.
            const QRect r = opt->rect;
            p->setPen( Qt::NoPen );
            p->setBrush( colors_.borderStrong );
            const bool vertical = opt->state & State_Horizontal;
            for( int i = 0; i < 2; ++i ) {
                if( vertical ) {
                    const int x = r.center().x() - 2 + i * 3;
                    for( int y = r.top() + 3; y < r.bottom() - 2; y += 3 )
                        p->drawRect( QRect( x, y, 1, 1 ) );
                } else {
                    const int y = r.center().y() - 2 + i * 3;
                    for( int x = r.left() + 3; x < r.right() - 2; x += 3 )
                        p->drawRect( QRect( x, y, 1, 1 ) );
                }
            }
            break;
        }

        // --- arrows (combo, spin, header, scroll, tool) ---------------------------------
        case PE_IndicatorArrowUp:
        case PE_IndicatorSpinUp:
        case PE_IndicatorSpinPlus: {
            PainterGuard g( p );
            drawChevron( p, opt->rect, Qt::UpArrow,
                         enabled ? ( hovered ? colors_.text : colors_.textDim )
                                 : colors_.textDisabled );
            break;
        }
        case PE_IndicatorArrowDown:
        case PE_IndicatorSpinDown:
        case PE_IndicatorSpinMinus:
        case PE_IndicatorButtonDropDown: {
            PainterGuard g( p );
            drawChevron( p, opt->rect, Qt::DownArrow,
                         enabled ? ( hovered ? colors_.text : colors_.textDim )
                                 : colors_.textDisabled );
            break;
        }
        case PE_IndicatorArrowLeft: {
            PainterGuard g( p );
            drawChevron( p, opt->rect, Qt::LeftArrow,
                         enabled ? colors_.textDim : colors_.textDisabled );
            break;
        }
        case PE_IndicatorArrowRight: {
            PainterGuard g( p );
            drawChevron( p, opt->rect, Qt::RightArrow,
                         enabled ? colors_.textDim : colors_.textDisabled );
            break;
        }

        case PE_IndicatorHeaderArrow: {
            const auto *h = qstyleoption_cast<const QStyleOptionHeader *>( opt );
            if( !h || h->sortIndicator == QStyleOptionHeader::None ) break;
            PainterGuard g( p );
            drawChevron( p, opt->rect,
                         h->sortIndicator == QStyleOptionHeader::SortUp ? Qt::UpArrow
                                                                       : Qt::DownArrow,
                         colors_.accent );
            break;
        }

        // --- tree branches -------------------------------------------------------------------
        case PE_IndicatorBranch: {
            if( !( opt->state & State_Children ) ) break;
            PainterGuard g( p );
            QRect r = opt->rect;
            // The branch cell is as wide as one indent step and can be much
            // taller than the chevron; centre a square in it so the arrow does
            // not stretch on a tall row.
            const int side = qMin( r.width(), r.height() );
            r = QRect( r.center().x() - side / 2, r.center().y() - side / 2, side, side );
            drawChevron( p, r,
                         ( opt->state & State_Open ) ? Qt::DownArrow : Qt::RightArrow,
                         hovered ? colors_.text : colors_.textDim );
            break;
        }

        default:
            QProxyStyle::drawPrimitive( elem, opt, p, widget );
    }
}


// -------------------------------------------------------------- controls ----

void SBrownProStyle::drawControl( ControlElement elem, const QStyleOption *opt,
                                  QPainter *p, const QWidget *widget ) const
{
    const bool enabled = opt->state & State_Enabled;
    const bool hovered = enabled && ( opt->state & State_MouseOver );

    switch( elem ) {

        // --- progress ---------------------------------------------------------
        case CE_ProgressBarGroove: {
            PainterGuard g( p );
            fillRounded( p, opt->rect, kRadius, colors_.surfaceSunken );
            strokeRounded( p, opt->rect, kRadius, colors_.border );
            break;
        }

        case CE_ProgressBarContents: {
            const auto *pb = qstyleoption_cast<const QStyleOptionProgressBar *>( opt );
            if( !pb ) break;
            PainterGuard g( p );
            const QRect r = opt->rect.adjusted( 1, 1, -1, -1 );
            if( r.isEmpty() ) break;

            // An indeterminate bar is min == max, and dividing by that span is
            // the classic divide-by-zero in a style. Draw a full accent-alt bar
            // instead of inventing a busy indicator we would have to animate.
            if( pb->minimum >= pb->maximum ) {
                fillRounded( p, r, 2, colors_.accentAlt );
                break;
            }
            const double frac = double( pb->progress - pb->minimum ) /
                                double( pb->maximum - pb->minimum );
            const bool horizontal = opt->state & State_Horizontal;
            QRect fill = r;
            if( horizontal ) {
                int w = int( r.width() * qBound( 0.0, frac, 1.0 ) );
                if( pb->invertedAppearance ) fill.setLeft( r.right() - w );
                else                         fill.setWidth( w );
            } else {
                int h = int( r.height() * qBound( 0.0, frac, 1.0 ) );
                if( pb->invertedAppearance ) fill.setHeight( h );
                else                         fill.setTop( r.bottom() - h );
            }
            if( !fill.isEmpty() )
                fillRounded( p, fill, 2, colors_.accentAlt );
            break;
        }

        case CE_ProgressBarLabel: {
            const auto *pb = qstyleoption_cast<const QStyleOptionProgressBar *>( opt );
            if( !pb || !pb->textVisible ) break;
            // Full-contrast ink, never HighlightedText: the label straddles the
            // filled and unfilled halves, so a colour chosen for one of them is
            // unreadable on the other.
            p->setPen( enabled ? colors_.text : colors_.textDisabled );
            p->drawText( pb->rect, pb->textAlignment, pb->text );
            break;
        }

        // --- tabs ---------------------------------------------------------------
        case CE_TabBarTabShape: {
            const auto *tab = qstyleoption_cast<const QStyleOptionTab *>( opt );
            if( !tab ) break;
            PainterGuard g( p );
            const bool selected = tab->state & State_Selected;
            const QRect r = tab->rect;

            QColor fill = colors_.surfaceDisabled;
            if( selected )     fill = colors_.surface;
            else if( hovered ) fill = colors_.hover;
            p->setPen( Qt::NoPen );
            p->setBrush( fill );
            p->drawRect( r );

            // The selected tab is marked by an ACCENT EDGE on the side facing
            // the page, plus a divider on the other three -- no notched tab
            // outline, which is the shape that dates a UI fastest.
            const QColor edge = selected ? colors_.accent : colors_.divider;
            const int th = selected ? 2 : 1;
            switch( tab->shape ) {
                case QTabBar::RoundedSouth:
                case QTabBar::TriangularSouth:
                    p->fillRect( QRect( r.left(), r.bottom() - th + 1, r.width(), th ), edge );
                    break;
                case QTabBar::RoundedWest:
                case QTabBar::TriangularWest:
                    p->fillRect( QRect( r.left(), r.top(), th, r.height() ), edge );
                    break;
                case QTabBar::RoundedEast:
                case QTabBar::TriangularEast:
                    p->fillRect( QRect( r.right() - th + 1, r.top(), th, r.height() ), edge );
                    break;
                default:   // North
                    p->fillRect( QRect( r.left(), r.top(), r.width(), th ), edge );
                    break;
            }
            if( !selected ) {
                p->setPen( colors_.divider );
                p->drawLine( r.topRight(), r.bottomRight() );
            }
            break;
        }

        case CE_TabBarTabLabel: {
            const auto *tab = qstyleoption_cast<const QStyleOptionTab *>( opt );
            if( !tab ) break;
            QStyleOptionTab t( *tab );
            // Selected tabs get full-contrast ink and the rest get the dim
            // token, which is what makes the active tab findable without a
            // heavier shape.
            t.palette.setColor( QPalette::WindowText,
                                !enabled                        ? colors_.textDisabled
                                : ( tab->state & State_Selected ) ? colors_.text
                                                                  : colors_.textDim );
            QProxyStyle::drawControl( elem, &t, p, widget );
            break;
        }

        // --- headers ----------------------------------------------------------------
        case CE_HeaderSection: {
            PainterGuard g( p );
            QColor fill = colors_.surfaceRaised;
            if( !enabled )                          fill = colors_.surfaceDisabled;
            else if( opt->state & State_Sunken )    fill = colors_.pressed;
            else if( hovered )                      fill = colors_.hover;
            p->fillRect( opt->rect, fill );
            p->setPen( colors_.divider );
            p->drawLine( opt->rect.topRight(), opt->rect.bottomRight() );
            p->drawLine( opt->rect.bottomLeft(), opt->rect.bottomRight() );
            break;
        }

        case CE_HeaderEmptyArea: {
            p->fillRect( opt->rect, colors_.surfaceRaised );
            p->setPen( colors_.divider );
            p->drawLine( opt->rect.bottomLeft(), opt->rect.bottomRight() );
            break;
        }

        // --- menu bar ------------------------------------------------------------------
        case CE_MenuBarEmptyArea: {
            p->fillRect( opt->rect, colors_.surfaceRaised );
            break;
        }

        case CE_MenuBarItem: {
            const auto *mi = qstyleoption_cast<const QStyleOptionMenuItem *>( opt );
            if( !mi ) break;
            PainterGuard g( p );
            p->fillRect( mi->rect, colors_.surfaceRaised );
            const bool open = mi->state & ( State_Selected | State_Sunken );
            if( open && enabled ) {
                fillRounded( p, mi->rect.adjusted( 1, 2, -1, -2 ), 2, colors_.accentMuted );
            }
            const int flags = Qt::AlignCenter | Qt::TextShowMnemonic |
                              Qt::TextDontClip | Qt::TextSingleLine |
                              ( proxy()->styleHint( SH_UnderlineShortcut, mi, widget )
                                    ? 0 : Qt::TextHideMnemonic );
            p->setPen( enabled ? colors_.text : colors_.textDisabled );
            p->drawText( mi->rect, flags, mi->text );
            break;
        }

        // --- chrome -------------------------------------------------------------------------
        case CE_ToolBar: {
            p->fillRect( opt->rect, colors_.surfaceRaised );
            // One divider on the edge that faces the content, so a toolbar is
            // separated from the arranger without a full frame around it.
            p->setPen( colors_.divider );
            if( opt->state & State_Horizontal )
                p->drawLine( opt->rect.bottomLeft(), opt->rect.bottomRight() );
            else
                p->drawLine( opt->rect.topRight(), opt->rect.bottomRight() );
            break;
        }

        case CE_Splitter: {
            p->fillRect( opt->rect, hovered ? colors_.hover : colors_.surfaceDisabled );
            p->setPen( colors_.divider );
            if( opt->rect.width() < opt->rect.height() )
                p->drawLine( opt->rect.center().x(), opt->rect.top(),
                             opt->rect.center().x(), opt->rect.bottom() );
            else
                p->drawLine( opt->rect.left(), opt->rect.center().y(),
                             opt->rect.right(), opt->rect.center().y() );
            break;
        }

        case CE_DockWidgetTitle: {
            const auto *dw = qstyleoption_cast<const QStyleOptionDockWidget *>( opt );
            PainterGuard g( p );
            p->fillRect( opt->rect, colors_.surfaceRaised );
            p->setPen( colors_.divider );
            p->drawLine( opt->rect.bottomLeft(), opt->rect.bottomRight() );
            if( dw && !dw->title.isEmpty() ) {
                QRect tr = opt->rect.adjusted( 6, 0, -6, 0 );
                // The float/close buttons live at the trailing edge; reserve
                // room for them so a long dock title is elided rather than
                // painted underneath them.
                tr.setRight( qMax( tr.left(),
                                   tr.right() - 2 * proxy()->pixelMetric(
                                       PM_SmallIconSize, opt, widget ) - 8 ) );
                p->setPen( enabled ? colors_.text : colors_.textDisabled );
                const QString t = p->fontMetrics().elidedText( dw->title, Qt::ElideRight,
                                                               tr.width() );
                p->drawText( tr, Qt::AlignVCenter | Qt::AlignLeft, t );
            }
            break;
        }

        case CE_ShapedFrame: {
            const auto *f = qstyleoption_cast<const QStyleOptionFrame *>( opt );
            if( !f ) break;
            const int shape = f->frameShape & QFrame::Shape_Mask;
            if( shape == QFrame::HLine ) {
                p->setPen( colors_.divider );
                p->drawLine( opt->rect.left(), opt->rect.center().y(),
                             opt->rect.right(), opt->rect.center().y() );
            } else if( shape == QFrame::VLine ) {
                p->setPen( colors_.divider );
                p->drawLine( opt->rect.center().x(), opt->rect.top(),
                             opt->rect.center().x(), opt->rect.bottom() );
            } else if( shape == QFrame::NoFrame ) {
                // nothing
            } else {
                PainterGuard g( p );
                strokeRounded( p, opt->rect, kRadius, colors_.border );
            }
            break;
        }

        default:
            QProxyStyle::drawControl( elem, opt, p, widget );
    }
}


// ------------------------------------------------------- complex controls ----

QRect SBrownProStyle::subControlRect( ComplexControl cc, const QStyleOptionComplex *opt,
                                      SubControl sc, const QWidget *widget ) const
{
    // The scrollbar is the one complex control whose GEOMETRY changes here, not
    // just its paint: it has no stepper buttons at all, which is what a modern
    // scrollbar looks like on every platform we ship to. That cannot be done
    // from drawComplexControl alone -- the base would still reserve, hit-test
    // and page from two arrow rects that are never drawn.
    if( cc == CC_ScrollBar ) {
        const auto *sb = qstyleoption_cast<const QStyleOptionSlider *>( opt );
        if( !sb ) return QProxyStyle::subControlRect( cc, opt, sc, widget );

        const bool horiz = sb->orientation == Qt::Horizontal;
        const QRect r = sb->rect;
        const int extent = horiz ? r.width() : r.height();

        int len = extent;
        if( sb->maximum > sb->minimum ) {
            const qint64 span = qint64( sb->maximum ) - qint64( sb->minimum ) + sb->pageStep;
            len = span > 0 ? int( qint64( extent ) * sb->pageStep / span ) : extent;
            len = qBound( proxy()->pixelMetric( PM_ScrollBarSliderMin, opt, widget ),
                          len, extent );
        }
        const int maxPos = extent - len;
        const int pos = ( sb->maximum > sb->minimum )
            ? sliderPositionFromValue( sb->minimum, sb->maximum, sb->sliderPosition,
                                       maxPos, sb->upsideDown )
            : 0;

        const QRect slider = horiz ? QRect( r.x() + pos, r.y(), len, r.height() )
                                   : QRect( r.x(), r.y() + pos, r.width(), len );

        switch( sc ) {
            case SC_ScrollBarSubLine:
            case SC_ScrollBarAddLine:
            case SC_ScrollBarFirst:
            case SC_ScrollBarLast:
                return QRect();                    // no steppers
            case SC_ScrollBarGroove:
                return r;
            case SC_ScrollBarSlider:
                return slider;
            case SC_ScrollBarSubPage:
                return horiz ? QRect( r.x(), r.y(), pos, r.height() )
                             : QRect( r.x(), r.y(), r.width(), pos );
            case SC_ScrollBarAddPage:
                return horiz
                    ? QRect( slider.right() + 1, r.y(), r.right() - slider.right(), r.height() )
                    : QRect( r.x(), slider.bottom() + 1, r.width(), r.bottom() - slider.bottom() );
            default:
                break;
        }
    }

    return QProxyStyle::subControlRect( cc, opt, sc, widget );
}

void SBrownProStyle::drawComplexControl( ComplexControl cc, const QStyleOptionComplex *opt,
                                         QPainter *p, const QWidget *widget ) const
{
    const bool enabled = opt->state & State_Enabled;
    const bool hovered = enabled && ( opt->state & State_MouseOver );

    switch( cc ) {

        // --- scroll bars ------------------------------------------------------
        case CC_ScrollBar: {
            const auto *sb = qstyleoption_cast<const QStyleOptionSlider *>( opt );
            if( !sb ) break;
            PainterGuard g( p );
            p->fillRect( sb->rect, colors_.surfaceSunken );

            const QRect handle = proxy()->subControlRect( CC_ScrollBar, sb,
                                                          SC_ScrollBarSlider, widget );
            if( handle.isEmpty() || !enabled ) break;

            const bool active = ( sb->activeSubControls & SC_ScrollBarSlider ) &&
                                ( sb->state & State_Sunken );
            QColor c = colors_.borderStrong;
            if( active )       c = colors_.accent;
            else if( hovered ) c = colors_.hover.lighter( 130 );

            // Inset by 2 px on the long axis so the handle floats in its track
            // rather than filling the bar edge to edge.
            const QRect h = ( sb->orientation == Qt::Horizontal )
                ? handle.adjusted( 1, 2, -1, -2 )
                : handle.adjusted( 2, 1, -2, -1 );
            fillRounded( p, h, qMin( h.width(), h.height() ) / 2.0, c );
            break;
        }

        // --- sliders -------------------------------------------------------------
        case CC_Slider: {
            const auto *sl = qstyleoption_cast<const QStyleOptionSlider *>( opt );
            if( !sl ) break;
            PainterGuard g( p );

            const QRect groove = proxy()->subControlRect( CC_Slider, sl, SC_SliderGroove, widget );
            const QRect handle = proxy()->subControlRect( CC_Slider, sl, SC_SliderHandle, widget );
            const bool horiz = sl->orientation == Qt::Horizontal;

            // A 4 px track centred in the groove, regardless of how thick the
            // widget itself is.
            QRect track = groove;
            if( horiz ) { track.setTop( groove.center().y() - 2 ); track.setHeight( 4 ); }
            else        { track.setLeft( groove.center().x() - 2 ); track.setWidth( 4 ); }

            fillRounded( p, track, 2, colors_.surfaceSunken );
            strokeRounded( p, track, 2, colors_.border );

            // The travelled part of the track is accented, so a fader reads its
            // own value without needing a scale beside it.
            if( enabled && !handle.isEmpty() ) {
                QRect done = track;
                if( horiz ) {
                    if( sl->upsideDown ) done.setLeft( handle.center().x() );
                    else                 done.setRight( handle.center().x() );
                } else {
                    if( sl->upsideDown ) done.setBottom( handle.center().y() );
                    else                 done.setTop( handle.center().y() );
                }
                if( done.isValid() && !done.isEmpty() )
                    fillRounded( p, done, 2, colors_.accentMuted );
            }

            if( sl->subControls & SC_SliderTickmarks ) {
                QStyleOptionSlider t( *sl );
                t.subControls = SC_SliderTickmarks;
                t.palette.setColor( QPalette::WindowText, colors_.divider );
                QProxyStyle::drawComplexControl( CC_Slider, &t, p, widget );
            }

            if( !handle.isEmpty() ) {
                const bool pressed = ( sl->activeSubControls & SC_SliderHandle ) &&
                                     ( sl->state & State_Sunken );
                QColor fill = colors_.surface;
                if( !enabled )     fill = colors_.surfaceDisabled;
                else if( pressed ) fill = colors_.pressed;
                else if( hovered ) fill = colors_.hover;
                fillRounded( p, handle, 2, enabled ? QBrush( raisedFill( handle, fill ) )
                                                   : QBrush( fill ) );
                strokeRounded( p, handle, 2,
                               !enabled ? colors_.divider
                                        : ( ( sl->state & State_HasFocus ) || pressed )
                                              ? colors_.accent : colors_.borderStrong );
            }
            break;
        }

        // --- combo boxes ------------------------------------------------------------
        case CC_ComboBox: {
            const auto *cb = qstyleoption_cast<const QStyleOptionComboBox *>( opt );
            if( !cb ) break;
            PainterGuard g( p );

            const bool open = cb->state & State_On;
            QColor fill = cb->editable ? colors_.surfaceSunken : colors_.surface;
            if( !enabled )                 fill = colors_.surfaceDisabled;
            else if( open )                fill = colors_.pressed;
            else if( hovered && !cb->editable ) fill = colors_.hover;

            fillRounded( p, cb->rect, kRadius,
                         ( enabled && !cb->editable ) ? QBrush( raisedFill( cb->rect, fill ) )
                                                      : QBrush( fill ) );

            QColor line = colors_.border;
            if( !enabled )                                line = colors_.divider;
            else if( open || ( cb->state & State_HasFocus ) ) line = colors_.accent;
            else if( hovered )                            line = colors_.borderStrong;
            strokeRounded( p, cb->rect, kRadius, line );

            const QRect arrowRect = proxy()->subControlRect( CC_ComboBox, cb,
                                                             SC_ComboBoxArrow, widget );
            if( !arrowRect.isEmpty() ) {
                // A hairline between the field and the arrow only when the box
                // is editable -- there the two halves do different things.
                if( cb->editable ) {
                    p->setPen( colors_.border );
                    p->drawLine( arrowRect.left(), arrowRect.top() + 3,
                                 arrowRect.left(), arrowRect.bottom() - 3 );
                }
                drawChevron( p, arrowRect, Qt::DownArrow,
                             enabled ? ( hovered ? colors_.text : colors_.textDim )
                                     : colors_.textDisabled );
            }
            break;
        }

        // --- spin boxes ---------------------------------------------------------------
        case CC_SpinBox: {
            const auto *sp = qstyleoption_cast<const QStyleOptionSpinBox *>( opt );
            if( !sp ) break;
            PainterGuard g( p );

            fillRounded( p, sp->rect, kRadius,
                         enabled ? colors_.surfaceSunken : colors_.surfaceDisabled );

            QColor line = colors_.border;
            if( !enabled )                        line = colors_.divider;
            else if( sp->state & State_HasFocus ) line = colors_.accent;
            else if( hovered )                    line = colors_.borderStrong;
            strokeRounded( p, sp->rect, kRadius, line );

            const struct { SubControl sc; Qt::ArrowType dir; } parts[2] = {
                { SC_SpinBoxUp,   Qt::UpArrow },
                { SC_SpinBoxDown, Qt::DownArrow },
            };
            for( const auto &part : parts ) {
                if( !( sp->subControls & part.sc ) ) continue;
                const QRect r = proxy()->subControlRect( CC_SpinBox, sp, part.sc, widget );
                if( r.isEmpty() ) continue;

                const bool partEnabled = enabled &&
                    ( sp->stepEnabled & ( part.sc == SC_SpinBoxUp
                                              ? QAbstractSpinBox::StepUpEnabled
                                              : QAbstractSpinBox::StepDownEnabled ) );
                const bool partActive = ( sp->activeSubControls & part.sc );
                if( partActive && partEnabled ) {
                    fillRounded( p, r.adjusted( 1, 1, -1, -1 ), 2,
                                 ( sp->state & State_Sunken ) ? colors_.pressed : colors_.hover );
                }
                drawChevron( p, r, part.dir,
                             partEnabled ? ( partActive ? colors_.text : colors_.textDim )
                                         : colors_.textDisabled );
            }
            break;
        }

        default:
            QProxyStyle::drawComplexControl( cc, opt, p, widget );
    }
}
