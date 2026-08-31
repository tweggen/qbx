#include "app/testkit/sdetailpanetestactions.h"

#include <QApplication>
#include <QDebug>
#include <QDomElement>

#include "app/actions/sactionregistry.h"
#include "app/shell/smainwindow.h"

namespace {

SMainWindow *mainWindow()
{
    SMainWindow *win = nullptr;
    // NOT QApplication::activeWindow(): a --test-case run never shows the
    // window, so nothing is ever "active" under QT_QPA_PLATFORM=offscreen.
    for( QWidget *w : QApplication::topLevelWidgets() )
        if( ( win = qobject_cast<SMainWindow *>( w ) ) ) break;
    return win;
}

int fieldOf( const QString &desc, const QString &key )
{
    for( const QString &part : desc.split( QLatin1Char( '|' ) ) )
        if( part.startsWith( key + QLatin1Char( '=' ) ) )
            return part.mid( key.size() + 1 ).toInt();
    return -1;
}

}  // namespace

SApplyResult SAssertTrackDetailLayoutAction::apply( SProject * )
{
    SMainWindow *win = mainWindow();
    if( !win ) {
        qWarning() << "assert-track-detail-layout: no main window";
        return { false, nullptr };
    }
    const QString desc =
        win->describeTrackDetailLayout( trackPath_, panelWidth_, panelHeight_ );
    if( desc.isEmpty() ) {
        qWarning() << "assert-track-detail-layout: no track at" << trackPath_;
        return { false, nullptr };
    }

    const int crushed = fieldOf( desc, QStringLiteral( "crushed" ) );
    const int overlap = fieldOf( desc, QStringLiteral( "overlap" ) );
    if( crushed > maxCrushed_ ) {
        qWarning() << "assert-track-detail-layout FAILED:" << crushed
                   << "widget(s) given less height than they need, max"
                   << maxCrushed_ << "-" << desc;
        return { false, nullptr };
    }
    if( overlap > maxOverlap_ ) {
        qWarning() << "assert-track-detail-layout FAILED:" << overlap
                   << "overlapping widget pair(s), max" << maxOverlap_
                   << "-" << desc;
        return { false, nullptr };
    }
    if( !contains_.isEmpty() && !desc.contains( contains_ ) ) {
        qWarning() << "assert-track-detail-layout FAILED: expected"
                   << contains_ << "in" << desc;
        return { false, nullptr };
    }
    qDebug() << "assert-track-detail-layout: OK -" << desc;
    return { true, nullptr };
}

void SAssertTrackDetailLayoutAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "trackPath", trackPath_ );
    elem.setAttribute( "panelWidth", panelWidth_ );
    elem.setAttribute( "panelHeight", panelHeight_ );
    elem.setAttribute( "maxCrushed", maxCrushed_ );
    elem.setAttribute( "maxOverlap", maxOverlap_ );
    elem.setAttribute( "contains", contains_ );
}

bool SAssertTrackDetailLayoutAction::readXml( const QDomElement &elem, int )
{
    trackPath_   = elem.attribute( "trackPath", "0" );
    panelWidth_  = elem.attribute( "panelWidth", "320" ).toInt();
    panelHeight_ = elem.attribute( "panelHeight", "260" ).toInt();
    maxCrushed_  = elem.attribute( "maxCrushed", "0" ).toInt();
    maxOverlap_  = elem.attribute( "maxOverlap", "0" ).toInt();
    contains_    = elem.attribute( "contains" );
    return true;
}

SApplyResult SDoubleClickControlAction::apply( SProject * )
{
    SMainWindow *win = mainWindow();
    if( !win ) {
        qWarning() << "double-click-control: no main window";
        return { false, nullptr };
    }
    // REJECTED, not silently ignored, when the control cannot be reached — an
    // unknown name, a track path that names no track, a Clip Detail field the
    // current selection does not show. The script's own `expectReject="true"`
    // is what turns that into an assertion, exactly as it does for every other
    // verb; this action carries no expectReject of its own.
    if( !win->doubleClickDetailControl( control_, trackPath_ ) ) {
        qWarning() << "double-click-control: control" << control_
                   << "could not be reached";
        return { false, nullptr };
    }
    qDebug() << "double-click-control:" << control_ << "clicked";
    return { true, nullptr };   // a gesture: what it submitted is the undo step
}

void SDoubleClickControlAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "control", control_ );
    elem.setAttribute( "trackPath", trackPath_ );
}

bool SDoubleClickControlAction::readXml( const QDomElement &elem, int )
{
    control_   = elem.attribute( "control", "track-volume" );
    trackPath_ = elem.attribute( "trackPath", "0" );
    return true;
}

static const bool s_reg_assert_track_detail_layout = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "assert-track-detail-layout" ),
        []{ return new SAssertTrackDetailLayoutAction; } ), true );

static const bool s_reg_double_click_control = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "double-click-control" ),
        []{ return new SDoubleClickControlAction; } ), true );
