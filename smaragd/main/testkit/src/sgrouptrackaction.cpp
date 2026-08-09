#include "app/testkit/sgrouptrackaction.h"
#include "app/actions/sactionregistry.h"
#include "app/shell/smainwindow.h"
#include <QApplication>
#include <QDebug>
#include <QDomElement>

SApplyResult SGroupTrackAction::apply( SProject * /*project*/ )
{
    SMainWindow *win = nullptr;
    for( QWidget *w : QApplication::topLevelWidgets() ) {
        if( ( win = qobject_cast<SMainWindow *>( w ) ) ) break;
    }
    if( !win ) {
        qWarning() << "group-track: no main window";
        return { false, nullptr };
    }
    if( !win->groupTrackGesture( trackPath_, ungroup_ ) ) {
        qWarning() << "group-track: no track at" << trackPath_;
        return { false, nullptr };
    }
    // The gesture pushed its own macro; undoing THAT reverses it.
    return { true, nullptr };
}

void SGroupTrackAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "trackPath", trackPath_ );
    elem.setAttribute( "ungroup", ungroup_ ? "1" : "0" );
}

bool SGroupTrackAction::readXml( const QDomElement &elem, int /*version*/ )
{
    trackPath_ = elem.attribute( "trackPath", "0" );
    const QString u = elem.attribute( "ungroup", "0" );
    ungroup_ = ( u == "1" || u == "true" );
    return true;
}

static const bool s_reg_grouptrack =
    ( SActionRegistry::instance().registerType(
          QStringLiteral( "group-track" ),
          [] { return new SGroupTrackAction; } ),
      true );
