#include "app/testkit/smainactiontestactions.h"

#include <QAction>
#include <QApplication>
#include <QDebug>
#include <QDomElement>
#include <QKeySequence>
#include <QStringList>
#include <QWidget>

#include "app/actions/sactionregistry.h"
#include "app/shell/smainwindow.h"

namespace {

SMainWindow *mainWindow()
{
    for( QWidget *w : QApplication::topLevelWidgets() )
        if( SMainWindow *win = qobject_cast<SMainWindow *>( w ) ) return win;
    return nullptr;
}

QString roleName( QAction::MenuRole r )
{
    switch( r ) {
    case QAction::NoRole:                  return QStringLiteral( "none" );
    case QAction::TextHeuristicRole:       return QStringLiteral( "text" );
    case QAction::ApplicationSpecificRole: return QStringLiteral( "application" );
    case QAction::AboutQtRole:             return QStringLiteral( "aboutQt" );
    case QAction::AboutRole:               return QStringLiteral( "about" );
    case QAction::PreferencesRole:         return QStringLiteral( "preferences" );
    case QAction::QuitRole:                return QStringLiteral( "quit" );
    }
    return QStringLiteral( "?" );
}

} // namespace

QStringList SAssertMainActionAction::knownAttributes() const
{
    return { QStringLiteral( "objectName" ), QStringLiteral( "menuRole" ),
             QStringLiteral( "shortcut" ), QStringLiteral( "standardKey" ) };
}

SApplyResult SAssertMainActionAction::apply( SProject * )
{
    SMainWindow *win = mainWindow();
    if( !win ) {
        qWarning() << "assert-main-action: no main window";
        return { false, nullptr };
    }
    QAction *act = objectName_.isEmpty()
                       ? nullptr : win->findChild<QAction *>( objectName_ );
    if( !act ) {
        qWarning().noquote() << "assert-main-action FAILED: no action named"
                             << objectName_;
        return { false, nullptr };
    }
    QStringList keys;
    for( const QKeySequence &k : act->shortcuts() )
        keys << k.toString( QKeySequence::PortableText );
    const QString got = QStringLiteral( "role=%1 shortcuts=[%2]" )
                            .arg( roleName( act->menuRole() ), keys.join( ' ' ) );

    QStringList why;
    if( !menuRole_.isEmpty() && roleName( act->menuRole() ) != menuRole_ )
        why << QStringLiteral( "menuRole %1" ).arg( menuRole_ );
    if( !shortcut_.isEmpty() && !keys.contains( shortcut_ ) )
        why << QStringLiteral( "shortcut %1" ).arg( shortcut_ );
    if( !standardKey_.isEmpty() ) {
        if( standardKey_ != QLatin1String( "preferences" ) ) {
            why << QStringLiteral( "unknown standardKey %1" ).arg( standardKey_ );
        } else {
            for( const QKeySequence &k :
                 QKeySequence::keyBindings( QKeySequence::Preferences ) ) {
                const QString t = k.toString( QKeySequence::PortableText );
                if( !keys.contains( t ) )
                    why << QStringLiteral( "standard Preferences key %1" ).arg( t );
            }
        }
    }
    if( !why.isEmpty() ) {
        qWarning().noquote() << "assert-main-action FAILED:" << objectName_ << got
                             << "missing:" << why.join( ", " );
        return { false, nullptr };
    }
    qDebug().noquote() << "assert-main-action: OK -" << objectName_ << got;
    return { true, nullptr };
}

void SAssertMainActionAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "objectName", objectName_ );
    if( !menuRole_.isEmpty() )    elem.setAttribute( "menuRole", menuRole_ );
    if( !shortcut_.isEmpty() )    elem.setAttribute( "shortcut", shortcut_ );
    if( !standardKey_.isEmpty() ) elem.setAttribute( "standardKey", standardKey_ );
}

bool SAssertMainActionAction::readXml( const QDomElement &elem, int )
{
    objectName_  = elem.attribute( "objectName" );
    menuRole_    = elem.attribute( "menuRole" );
    shortcut_    = elem.attribute( "shortcut" );
    standardKey_ = elem.attribute( "standardKey" );
    return true;
}

static const bool s_reg_assertmainaction = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "assert-main-action" ),
        []{ return new SAssertMainActionAction; } ), true );
