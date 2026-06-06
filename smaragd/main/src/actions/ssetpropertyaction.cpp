#include "actions/ssetpropertyaction.h"
#include "sproject.h"
#include "sactionregistry.h"
#include <QDomElement>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonValue>

SSetPropertyAction::SSetPropertyAction( const QString &key, const QVariant &value )
    : key_( key ), value_( value )
{
}

SApplyResult SSetPropertyAction::apply( SProject *project )
{
    if( !project || key_.isEmpty() ) {
        return { false, nullptr };
    }
    project->setProp( key_, value_ );
    return { true, nullptr };   // non-undoable
}

void SSetPropertyAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "key", key_ );
    // Encode the value as JSON (wrapped in a 1-element array so scalars are
    // representable) to preserve its type across save/load.
    QJsonArray arr;
    arr.append( QJsonValue::fromVariant( value_ ) );
    elem.setAttribute( "value",
        QString::fromUtf8( QJsonDocument( arr ).toJson( QJsonDocument::Compact ) ) );
}

bool SSetPropertyAction::readXml( const QDomElement &elem, int /*version*/ )
{
    key_ = elem.attribute( "key" );
    QJsonDocument d = QJsonDocument::fromJson( elem.attribute( "value" ).toUtf8() );
    if( d.isArray() && !d.array().isEmpty() ) {
        value_ = d.array().at( 0 ).toVariant();
    }
    return true;
}

static const bool s_reg_setproperty = (
    SActionRegistry::instance().registerType(
        QStringLiteral("set-property"),
        []{ return new SSetPropertyAction; } ),
    true
);
