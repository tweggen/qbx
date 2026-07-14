#include "app/objects/mixer/screateassetaction.h"
#include "app/model/splacements.h"
#include "app/objects/mixer/sremoveassetaction.h"
#include "app/objects/track/strackpath.h"
#include "app/model/sproject.h"
#include "app/objects/mixer/sstdmixer.h"
#include "app/objects/cut/scut.h"
#include "app/actions/sactionregistry.h"
#include "tw/core/twfraction.h"
#include <QDomElement>

using namespace strackpath;

// First-unused "Asset N" name, so creating assets in a row doesn't collide and
// undo/redo can reuse an explicit name.
static QString generateAssetName( SProject *project )
{
    for( int n = 1; ; ++n ) {
        QString candidate = QString( "Asset %1" ).arg( n );
        if( !project->hasAsset( candidate ) ) return candidate;
    }
}

SCreateAssetAction::SCreateAssetAction( const QList<int> &containerPath,
                                        offset_t startOffset, length_t duration,
                                        const QString &assetName )
    : containerPath_( containerPath ),
      startOffset_( startOffset ),
      duration_( duration ),
      assetName_( assetName )
{
}

SApplyResult SCreateAssetAction::apply( SProject *project )
{
    if( !project || duration_ <= 0 ) {
        return { false, nullptr };
    }

    SObject *root = splacements::rootContainer( project );
    if( !root || !root->isPathContainer() ) {
        return { false, nullptr };
    }

    // The vertical scope is an existing container: the root mixer ([]) or a
    // folder track. Both are valid asset bodies (their getRootComponent() sums
    // their children live).
    SObject *container = resolveByPath( root, containerPath_ );
    if( !container ) {
        return { false, nullptr };
    }
    if( !container->isPathContainer() ) {
        return { false, nullptr };          // only containers can be windowed
    }

    QString assetName = assetName_.isEmpty() ? generateAssetName( project )
                                             : assetName_;

    // The asset is a live window over the container: a cut starting at
    // startOffset_ for duration_. No copy — placements read the container live.
    SCut *cut = new SCut( project, *container );
    cut->setWindow( WarpedPos( (int64_t)startOffset_ ), ClipLen( duration_ ),
                    WarpedLen( 0 ), Fraction(1) );
    cut->setSName( assetName );

    project->registerAsset( assetName, cut );

    return { true, new SRemoveAssetAction( assetName ) };
}

void SCreateAssetAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "container", pathToString( containerPath_ ) );
    elem.setAttribute( "startOffset", QString::fromStdString( Fraction(startOffset_, 1).toString() ) );
    elem.setAttribute( "duration", QString::fromStdString( Fraction(duration_, 1).toString() ) );
    elem.setAttribute( "assetName", assetName_ );
}

bool SCreateAssetAction::readXml( const QDomElement &elem, int /*version*/ )
{
    containerPath_ = stringToPath( elem.attribute( "container" ) );
    startOffset_   = (offset_t) parseFractionOrDouble( elem.attribute( "startOffset", "0" ).toStdString() ).toDouble();
    duration_      = (length_t) parseFractionOrDouble( elem.attribute( "duration", "0" ).toStdString() ).toDouble();
    assetName_     = elem.attribute( "assetName" );
    return true;
}

static const bool s_reg_createasset = (
    SActionRegistry::instance().registerType(
        QStringLiteral("create-asset"),
        []{ return new SCreateAssetAction; }
    ), true
);
