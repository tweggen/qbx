#include "app/testkit/ssavewindowlayoutaction.h"

#include <QApplication>
#include <QByteArray>
#include <QDebug>
#include <QDomElement>

#include "app/actions/sactionregistry.h"
#include "app/shell/smainwindow.h"
#include "app/shell/ssettings.h"

SApplyResult SSaveWindowLayoutAction::apply( SProject * )
{
    SMainWindow *win = nullptr;
    for( QWidget *w : QApplication::topLevelWidgets() )
        if( ( win = qobject_cast<SMainWindow*>( w ) ) ) break;
    if( !win ) {
        qWarning() << "save-window-layout: no main window";
        return { false, nullptr };
    }

    SSettings &st = SSettings::instance();

    // Snapshot BOTH keys, including whether they existed at all: putting an
    // empty QByteArray back where there was no key is not the same state, and
    // restoreWindowLayout() tells the two apart (`!geo.isEmpty()`).
    const bool       hadGeo   = st.contains( QStringLiteral( "ui/windowGeometry" ) );
    const bool       hadState = st.contains( QStringLiteral( "ui/windowState" ) );
    const QByteArray oldGeo   = st.windowGeometry();
    const QByteArray oldState = st.windowState();

    const bool wrote = win->saveWindowLayout( force_ );

    // What the call left behind, read back through the same accessors the
    // restore path uses — this is the assertion, not `wrote` alone.
    const QByteArray newGeo   = st.windowGeometry();
    const QByteArray newState = st.windowState();

    // ...and put the user's INI back before asserting anything, so a FAILING
    // assertion cannot leave junk behind either.
    if( hadGeo )   st.setWindowGeometry( oldGeo );
    else           st.remove( QStringLiteral( "ui/windowGeometry" ) );
    if( hadState ) st.setWindowState( oldState );
    else           st.remove( QStringLiteral( "ui/windowState" ) );

    if( wrote != expectWritten_ ) {
        qWarning() << "save-window-layout FAILED: force=" << force_
                   << "wrote=" << wrote << "expected" << expectWritten_;
        return { false, nullptr };
    }

    if( expectWritten_ ) {
        if( newState.isEmpty() ) {
            qWarning() << "save-window-layout FAILED: ui/windowState is empty"
                          " after a write that reported success";
            return { false, nullptr };
        }
        if( newGeo.isEmpty() ) {
            qWarning() << "save-window-layout FAILED: ui/windowGeometry is"
                          " empty after a write that reported success";
            return { false, nullptr };
        }
        qDebug() << "save-window-layout: wrote geometry" << newGeo.size()
                 << "bytes, state" << newState.size() << "bytes";
    } else {
        // The suppression assertion: nothing may have MOVED. Comparing against
        // the snapshot rather than against "empty" is what makes this work on
        // a developer box whose INI already holds a real layout.
        if( newGeo != oldGeo || newState != oldState ) {
            qWarning() << "save-window-layout FAILED: a refused call still"
                          " changed the stored layout";
            return { false, nullptr };
        }
    }
    return { true, nullptr };
}

void SSaveWindowLayoutAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "force", force_ ? "true" : "false" );
    elem.setAttribute( "expectWritten", expectWritten_ ? "true" : "false" );
}

bool SSaveWindowLayoutAction::readXml( const QDomElement &elem, int )
{
    force_ = elem.attribute( "force", "true" ).startsWith( "true" );
    expectWritten_ =
        elem.attribute( "expectWritten", "true" ).startsWith( "true" );
    return true;
}

static const bool s_reg_save_window_layout = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "save-window-layout" ),
        []{ return new SSaveWindowLayoutAction; } ), true );
