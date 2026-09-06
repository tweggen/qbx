#include "app/testkit/sdrainpendingdeletesaction.h"

#include <QCoreApplication>
#include <QDomElement>
#include <QEvent>

#include "app/actions/sactionregistry.h"

SApplyResult SDrainPendingDeletesAction::apply( SProject * /*project*/ )
{
    // The same call smaragdOrderlyShutdown() makes before std::exit(), for the
    // same reason: nothing else in a --test-case run delivers these.
    QCoreApplication::sendPostedEvents( nullptr, QEvent::DeferredDelete );
    return { true, nullptr };
}

void SDrainPendingDeletesAction::writeXml( QDomElement & ) const
{
}

bool SDrainPendingDeletesAction::readXml( const QDomElement &, int )
{
    return true;
}

static const bool s_reg_drain_pending_deletes = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "drain-pending-deletes" ),
        []{ return new SDrainPendingDeletesAction; } ), true );
