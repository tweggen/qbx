#include "app/testkit/sqtfreetestactions.h"
#include "app/actions/sactionregistry.h"
#include "tw/core/twqtfree.h"

#include <QDebug>
#include <QDomElement>

// ========================================================== watch-qt-free ===

SApplyResult SWatchQtFreeAction::apply( SProject * /*project*/ )
{
    if( !twQtFreeWatch::install() ) {
        qWarning().noquote() << "watch-qt-free: REJECTED: this Qt offers no QObject-construction "
                                "hook (qtHookData), so nothing can be watched";
        return { false, nullptr };
    }
    twQtFreeWatch::reset();
    qInfo().noquote() << "watch-qt-free: watching QObject construction inside Qt-free scopes";
    return { true, nullptr };
}

void SWatchQtFreeAction::writeXml( QDomElement & /*elem*/ ) const {}

bool SWatchQtFreeAction::readXml( const QDomElement & /*elem*/, int /*version*/ )
{
    return true;
}

QStringList SWatchQtFreeAction::knownAttributes() const
{
    return {};
}

static const bool s_reg_watchqtfree =
    ( SActionRegistry::instance().registerType( QStringLiteral( "watch-qt-free" ),
                                                [] { return new SWatchQtFreeAction; } ),
      true );

// ========================================================= assert-qt-free ===

SApplyResult SAssertQtFreeAction::apply( SProject * /*project*/ )
{
    if( !twQtFreeWatch::installed() ) {
        qWarning().noquote() << "assert-qt-free: REJECTED: no watch-qt-free before it";
        return { false, nullptr };
    }
    const qint64 v = (qint64)twQtFreeWatch::violations();
    const qint64 s = (qint64)twQtFreeWatch::scopesEntered();
    const QString what = QStringLiteral( "assert-qt-free: %1 QObject(s) constructed inside %2 "
                                         "Qt-free scope(s) (max %3, min scopes %4)" )
                             .arg( v ).arg( s ).arg( maxViolations_ ).arg( minScopes_ );
    if( v > maxViolations_ || s < minScopes_ ) {
        qWarning().noquote() << what << "FAIL";
        return { false, nullptr };
    }
    qInfo().noquote() << what << "OK";
    return { true, nullptr };
}

void SAssertQtFreeAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "maxViolations", QString::number( maxViolations_ ) );
    elem.setAttribute( "minScopes", QString::number( minScopes_ ) );
}

bool SAssertQtFreeAction::readXml( const QDomElement &elem, int /*version*/ )
{
    maxViolations_ = elem.attribute( "maxViolations", "0" ).toLongLong();
    minScopes_     = elem.attribute( "minScopes", "1" ).toLongLong();
    return true;
}

QStringList SAssertQtFreeAction::knownAttributes() const
{
    return { QStringLiteral( "maxViolations" ), QStringLiteral( "minScopes" ) };
}

static const bool s_reg_assertqtfree =
    ( SActionRegistry::instance().registerType( QStringLiteral( "assert-qt-free" ),
                                                [] { return new SAssertQtFreeAction; } ),
      true );
