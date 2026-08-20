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
        SObject &content = cut->getContent();
        // WHICH ROOT the content lives under (proposal 09 D17). This used to
        // be `pathOf( project->getRootComponent(), &content )` unconditionally
        // — and for a content object under a detached ARRANGEMENT root that
        // walk finds nothing and returns the EMPTY path, which does not mean
        // "not found": it means THE ROOT ITSELF. So the inverse of removing an
        // arrangement's asset was an asset windowing the ENTIRE MASTER, built
        // silently, on undo.
        //
        // Ask the registry first. A registered root addresses itself as
        // "<name>:" with an empty index path, which is exactly the spelling
        // create-asset now parses.
        QString containerRoot = project->arrangementNameOf( &content );
        QList<int> containerPath;
        if( containerRoot.isEmpty() ) {
            SObject *master = project->getRootComponent();
            if( &content != master ) {
                containerPath = pathOf( master, &content );
                // Still empty => genuinely unreachable. REFUSE to build an
                // inverse rather than build one that means the master.
                if( containerPath.isEmpty() ) {
                    return { false, nullptr };
                }
            }
        } else {
            // Registered arrangement root: the empty path IS the address.
        }
        inverse = new SCreateAssetAction( containerPath,
                                          (offset_t) cut->getStartOffset().frames(),
                                          cut->getDurationBlocking(),   // edit path (P19)
                                          assetName_,
                                          containerRoot );
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
