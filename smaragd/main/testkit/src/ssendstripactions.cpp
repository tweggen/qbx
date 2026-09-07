#include "app/testkit/ssendstripactions.h"

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

}  // namespace

SApplyResult SAssertSendStripAction::apply( SProject * )
{
    SMainWindow *win = mainWindow();
    if( !win ) {
        qWarning() << "assert-send-strip: no main window";
        return { false, nullptr };
    }
    const QString desc = win->describeSendStrip( trackPath_ );

    bool ok = true;
    if( rows_ >= 0 ) {
        const int n = desc.isEmpty()
                          ? 0
                          : desc.split( QLatin1Char( ';' ) ).size();
        if( n != rows_ ) {
            qWarning().noquote()
                << "assert-send-strip FAILED: rows" << n << "want" << rows_
                << "-" << desc;
            ok = false;
        }
    }
    if( !contains_.isEmpty() && !desc.contains( contains_ ) ) {
        qWarning().noquote()
            << "assert-send-strip FAILED: missing" << contains_ << "-" << desc;
        ok = false;
    }
    if( !absent_.isEmpty() && desc.contains( absent_ ) ) {
        qWarning().noquote()
            << "assert-send-strip FAILED: present but must not be" << absent_
            << "-" << desc;
        ok = false;
    }
    if( ok ) qInfo().noquote() << "assert-send-strip OK -" << desc;
    return { ok, nullptr };
}

void SAssertSendStripAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "trackPath", trackPath_ );
    if( rows_ >= 0 )               elem.setAttribute( "rows", rows_ );
    if( !contains_.isEmpty() )     elem.setAttribute( "contains", contains_ );
    if( !absent_.isEmpty() )       elem.setAttribute( "absent", absent_ );
}

bool SAssertSendStripAction::readXml( const QDomElement &elem, int )
{
    trackPath_ = elem.attribute( "trackPath" );
    contains_  = elem.attribute( "contains" );
    absent_    = elem.attribute( "absent" );
    rows_ = elem.hasAttribute( "rows" ) ? elem.attribute( "rows" ).toInt() : -1;
    return !trackPath_.isEmpty();
}

static const bool s_reg_assert_send_strip = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "assert-send-strip" ),
        []{ return new SAssertSendStripAction; } ), true );

// --- send-strip-set ---------------------------------------------------------

SApplyResult SSendStripSetAction::apply( SProject * )
{
    SMainWindow *win = mainWindow();
    if( !win ) {
        qWarning() << "send-strip-set: no main window";
        return { false, nullptr };
    }
    if( !win->driveSendStrip( trackPath_, lane_, control_, value_ ) ) {
        qWarning().noquote()
            << "send-strip-set: no row for lane" << lane_ << "control"
            << control_ << "on track" << trackPath_;
        return { false, nullptr };
    }
    // The CONTROL's own signal submits the verb, which carries its own undo
    // step; this action is the gesture and adds none of its own.
    return { true, nullptr };
}

void SSendStripSetAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "trackPath", trackPath_ );
    elem.setAttribute( "lane", lane_ );
    elem.setAttribute( "control", control_ );
    elem.setAttribute( "value", value_ );
}

bool SSendStripSetAction::readXml( const QDomElement &elem, int )
{
    trackPath_ = elem.attribute( "trackPath" );
    lane_      = elem.attribute( "lane" );
    control_   = elem.attribute( "control" );
    value_     = elem.attribute( "value", "0" ).toDouble();
    return !trackPath_.isEmpty() && !lane_.isEmpty() && !control_.isEmpty();
}

static const bool s_reg_send_strip_set = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "send-strip-set" ),
        []{ return new SSendStripSetAction; } ), true );
