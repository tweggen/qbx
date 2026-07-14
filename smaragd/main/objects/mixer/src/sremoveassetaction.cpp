#include "app/objects/mixer/sremoveassetaction.h"
#include "app/model/splacements.h"
#include "app/objects/mixer/screateassetaction.h"
#include "app/objects/track/strackpath.h"
#include "app/model/sproject.h"
#include "app/objects/mixer/sstdmixer.h"
#include "app/objects/cut/scut.h"
#include "app/actions/sactionregistry.h"
#include <QDomElement>

using namespace strackpath;

SRemoveAssetAction::SRemoveAssetAction( const QString &assetName )
    : assetName_( assetName )
{
}

SApplyResult SRemoveAssetAction::apply( SProject *project )
{
    if( !project || assetName_.isEmpty() ) {
        return { false, nullptr };
    }

    SObject *body = project->asset( assetName_ );
    if( !body ) {
        return { false, nullptr };
    }

    // Capture the asset's defining state so the inverse rebuilds an identical
    // one (the body is derived: a container reference + a window).
    SAction *inverse = nullptr;
    SCut *cut = dynamic_cast<SCut *>( body );
    if( cut ) {
        SObject *root = project->getRootComponent();
        QList<int> containerPath = pathOf( root, &cut->getContent() );
        inverse = new SCreateAssetAction( containerPath,
                                          (offset_t) cut->getStartOffset().frames(),
                                          cut->getDuration(), assetName_ );
    }

    project->unregisterAsset( assetName_ );

    return { true, inverse };
}

void SRemoveAssetAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "assetName", assetName_ );
}

bool SRemoveAssetAction::readXml( const QDomElement &elem, int /*version*/ )
{
    assetName_ = elem.attribute( "assetName" );
    return true;
}

static const bool s_reg_removeasset = (
    SActionRegistry::instance().registerType(
        QStringLiteral("remove-asset"),
        []{ return new SRemoveAssetAction; }
    ), true
);
