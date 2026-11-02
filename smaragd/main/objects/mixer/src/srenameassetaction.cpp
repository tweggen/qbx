#include "app/objects/mixer/srenameassetaction.h"

#include "app/actions/sactionregistry.h"
#include "app/model/sobject.h"
#include "app/model/sproject.h"

#include <QDebug>
#include <QDomElement>

SRenameAssetAction::SRenameAssetAction( const QString &assetName, const QString &newName )
    : assetName_( assetName ), newName_( newName )
{
}

SApplyResult SRenameAssetAction::apply( SProject *project )
{
    auto refuse = [this]( const QString &why ) -> SApplyResult {
        qWarning().noquote() << QStringLiteral( "rename-asset: \"%1\" -> \"%2\": REFUSED: %3" )
                                    .arg( assetName_, newName_, why );
        return { false, nullptr };
    };
    if( !project ) return refuse( QStringLiteral( "no project" ) );
    if( assetName_.isEmpty() || newName_.isEmpty() )
        return refuse( QStringLiteral( "both assetName and newName are required" ) );
    SObject *body = project->asset( assetName_ );
    if( !body ) return refuse( QStringLiteral( "there is no asset \"%1\"" ).arg( assetName_ ) );
    if( newName_ == assetName_ ) return refuse( QStringLiteral( "the asset already has that name" ) );
    if( project->hasAsset( newName_ ) )
        return refuse( QStringLiteral( "the name \"%1\" is in use" ).arg( newName_ ) );

    // The new entry FIRST: it takes its pin before the old entry drops its
    // own, so the body's reference count never passes through zero.
    project->registerAsset( newName_, body );
    project->unregisterAsset( assetName_ );
    if( body->getSName() == assetName_ ) body->setSName( newName_ );
    qInfo().noquote() << QStringLiteral( "rename-asset: \"%1\" -> \"%2\"" ).arg( assetName_, newName_ );
    return { true, new SRenameAssetAction( newName_, assetName_ ) };
}

void SRenameAssetAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "assetName", assetName_ );
    elem.setAttribute( "newName", newName_ );
}

bool SRenameAssetAction::readXml( const QDomElement &elem, int /*version*/ )
{
    assetName_ = elem.attribute( "assetName" );
    newName_   = elem.attribute( "newName" );
    return true;
}

QStringList SRenameAssetAction::knownAttributes() const
{
    return { QStringLiteral( "assetName" ), QStringLiteral( "newName" ) };
}

static const bool s_reg_renameasset = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "rename-asset" ),
        [] { return new SRenameAssetAction; } ),
    true );
