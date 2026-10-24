#include "app/testkit/sassertpropertyaction.h"

#include "app/actions/sactionregistry.h"
#include "app/model/sproject.h"

#include <QDebug>
#include <QDomElement>

QStringList SAssertPropertyAction::knownAttributes() const
{
    return { QStringLiteral( "key" ), QStringLiteral( "equals" ) };
}

SApplyResult SAssertPropertyAction::apply( SProject *project )
{
    if( !project ) return { false, nullptr };
    const QString got = project->prop( key_ ).toString();
    if( got != equals_ ) {
        qWarning().noquote() << "assert-property FAILED:" << key_ << "is"
                             << ( "\"" + got + "\"" ) << "expected"
                             << ( "\"" + equals_ + "\"" );
        return { false, nullptr };
    }
    qDebug().noquote() << "assert-property: OK -" << key_ << "="
                       << ( "\"" + got + "\"" );
    return { true, nullptr };
}

void SAssertPropertyAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "key", key_ );
    elem.setAttribute( "equals", equals_ );
}

bool SAssertPropertyAction::readXml( const QDomElement &elem, int )
{
    key_    = elem.attribute( "key" );
    equals_ = elem.attribute( "equals" );
    if( key_.isEmpty() ) {
        qWarning() << "assert-property: key is required";
        return false;
    }
    return true;
}

static const bool s_reg_assertproperty = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "assert-property" ),
        []{ return new SAssertPropertyAction; } ), true );
