#include "app/testkit/srestorewindowlayoutaction.h"

#include <QApplication>
#include <QByteArray>
#include <QDataStream>
#include <QDebug>
#include <QDomElement>
#include <QIODevice>
#include <QRect>
#include <QScreen>
#include <QWidget>

#include "app/actions/sactionregistry.h"
#include "app/shell/smainwindow.h"
#include "app/shell/ssettings.h"

namespace {

// A blob Qt writes for a never-shown widget in `state`.
QByteArray qtBlob( Qt::WindowStates state )
{
    QWidget w;
    w.setGeometry( 100, 120, 640, 480 );
    if( state != Qt::WindowNoState ) w.setWindowState( state );
    return w.saveGeometry();
}

// The field report's blob (QBX-294), in Qt 6's version-3 layout, with the
// running screen's width in the screen-width field: Qt's restoreGeometry()
// refuses a whole blob whose stored width is off by more than 0.8..1.25 (a
// high-DPI sanity check), and the report's machine had a 1280-wide screen.
QByteArray reportBlob( qint32 screenWidth )
{
    QByteArray b;
    QDataStream s( &b, QIODevice::WriteOnly );
    s.setVersion( QDataStream::Qt_4_0 );
    const QRect frame( QPoint( 0, 26 ), QPoint( 1279, 827 ) );
    s << quint32( 0x01D9D0CB ) << quint16( 3 ) << quint16( 0 )
      << frame << frame << qint32( 0 )
      << quint8( 0 ) << quint8( 0x04 )
      << screenWidth << frame;
    return b;
}

QString describeState( Qt::WindowStates s )
{
    QStringList parts;
    if( s & Qt::WindowMinimized )  parts << "minimized";
    if( s & Qt::WindowMaximized )  parts << "maximized";
    if( s & Qt::WindowFullScreen ) parts << "fullscreen";
    return parts.isEmpty() ? QStringLiteral( "normal" ) : parts.join( '+' );
}

} // namespace

SApplyResult SRestoreWindowLayoutAction::apply( SProject * )
{
    SMainWindow *win = nullptr;
    for( QWidget *w : QApplication::topLevelWidgets() )
        if( ( win = qobject_cast<SMainWindow*>( w ) ) ) break;
    if( !win ) {
        qWarning() << "restore-window-layout: no main window";
        return { false, nullptr };
    }

    QByteArray seedBlob;
    if( seed_ == "fullscreen" )      seedBlob = qtBlob( Qt::WindowFullScreen );
    else if( seed_ == "maximized" )  seedBlob = qtBlob( Qt::WindowMaximized );
    else if( seed_ == "normal" )     seedBlob = qtBlob( Qt::WindowNoState );
    else if( seed_ == "report" )     seedBlob = reportBlob( QGuiApplication::primaryScreen()
                                         ? QGuiApplication::primaryScreen()->geometry().width()
                                         : 0 );
    else {
        qWarning() << "restore-window-layout: unknown seed" << seed_;
        return { false, nullptr };
    }
    if( expectState_ != "maximized" && expectState_ != "normal"
        && expectState_ != "fullscreen" ) {
        qWarning() << "restore-window-layout: unknown expectState" << expectState_;
        return { false, nullptr };
    }

    SSettings &st = SSettings::instance();
    const QString geoKey   = QStringLiteral( "ui/windowGeometry" );
    const QString stateKey = QStringLiteral( "ui/windowState" );
    const bool       hadGeo   = st.contains( geoKey );
    const bool       hadState = st.contains( stateKey );
    const QByteArray oldGeo   = st.windowGeometry();
    const QByteArray oldState = st.windowState();

    const QByteArray       winGeo   = win->saveGeometry();
    const Qt::WindowStates winState = win->windowState();

    st.setWindowGeometry( seedBlob );
    st.remove( stateKey );

    const bool restored = win->restoreWindowLayout();
    const Qt::WindowStates got = win->windowState();

    // Put the INI and the window back before asserting anything.
    if( hadGeo )   st.setWindowGeometry( oldGeo );
    else           st.remove( geoKey );
    if( hadState ) st.setWindowState( oldState );
    else           st.remove( stateKey );
    win->restoreGeometry( winGeo );
    win->setWindowState( winState );

    if( !restored ) {
        qWarning() << "restore-window-layout FAILED: seed" << seed_
                   << "was not accepted by restoreWindowLayout()";
        return { false, nullptr };
    }

    bool ok = false;
    if( expectState_ == "maximized" )
        ok = ( got & Qt::WindowMaximized ) && !( got & Qt::WindowFullScreen );
    else if( expectState_ == "normal" )
        ok = !( got & ( Qt::WindowMaximized | Qt::WindowFullScreen ) );
    else
        ok = ( got & Qt::WindowFullScreen ) != 0;

    if( !ok ) {
        qWarning() << "restore-window-layout FAILED: seed" << seed_
                   << "restored as" << describeState( got )
                   << "expected" << expectState_;
        return { false, nullptr };
    }
    qDebug() << "restore-window-layout: seed" << seed_ << "restored as"
             << describeState( got );
    return { true, nullptr };
}

void SRestoreWindowLayoutAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "seed", seed_ );
    elem.setAttribute( "expectState", expectState_ );
}

bool SRestoreWindowLayoutAction::readXml( const QDomElement &elem, int )
{
    seed_        = elem.attribute( "seed" );
    expectState_ = elem.attribute( "expectState" );
    return true;
}

static const bool s_reg_restore_window_layout = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "restore-window-layout" ),
        []{ return new SRestoreWindowLayoutAction; } ), true );
