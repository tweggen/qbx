#include "app/objects/mixer/sduplicateassethereaction.h"
#include "app/objects/mixer/splaceassetaction.h"
#include "app/objects/mixer/sremoveassetplacementaction.h"
#include "app/objects/fragment/slanefragment.h"
#include "app/objects/cut/scut.h"
#include "app/objects/cut/sduplicateclipaction.h"
#include "app/objects/track/strackpath.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/model/slink.h"
#include "app/actions/sactionregistry.h"
#include "app/actions/scompositeaction.h"
#include "tw/core/twlog.h"
#include <QDomElement>

using namespace strackpath;

namespace {

// Live-only pair, following SCompositeAction's own convention (not
// registered as a verb; scripts never construct one directly). They exist so
// "mint a new asset over a deep-copied fragment" is one undoable STEP inside
// duplicate-asset-here's composite, symmetrical with the other two steps
// (SRemoveAssetPlacementAction / SPlaceAssetAction) it composes with.
class SMintFragmentAssetAction : public SAction {
public:
    SMintFragmentAssetAction( const QString &sourceAssetName,
                              const QString &newAssetName )
        : sourceAssetName_( sourceAssetName ), newAssetName_( newAssetName )
    {}

    QString name() const override
    { return QStringLiteral( "mint-fragment-asset" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement & ) const override {}
    bool readXml( const QDomElement &, int ) override { return false; }

private:
    QString sourceAssetName_;
    QString newAssetName_;
};

class SUnregisterFragmentAssetAction : public SAction {
public:
    SUnregisterFragmentAssetAction( const QString &sourceAssetName,
                                    const QString &mintedAssetName )
        : sourceAssetName_( sourceAssetName ), mintedAssetName_( mintedAssetName )
    {}

    QString name() const override
    { return QStringLiteral( "unregister-fragment-asset" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement & ) const override {}
    bool readXml( const QDomElement &, int ) override { return false; }

private:
    QString sourceAssetName_;
    QString mintedAssetName_;
};

SApplyResult SMintFragmentAssetAction::apply( SProject *project )
{
    if( !project ) return { false, nullptr };
    SObject *body = project->asset( sourceAssetName_ );
    SCut *srcCut = dynamic_cast<SCut *>( body );
    SLaneFragment *srcFragment = srcCut
        ? dynamic_cast<SLaneFragment *>( &srcCut->getContent() ) : nullptr;
    if( !srcCut || !srcFragment ) return { false, nullptr };

    // The deep copy: a NEW fragment, and a NEW window object per child (the
    // duplicate-clip mechanism, sduplicateclipaction.h) -- content stays
    // shared, exactly as an ordinary clip duplicate's does, because every
    // per-clip edit this codebase has lives on the WINDOW, never on raw
    // content.
    SLaneFragment *newFragment = new SLaneFragment( project );
    for( SLink *child : srcFragment->childLinks() ) {
        makeDuplicateClip( project, child->getSObject(), newFragment,
                           child->getStartTime() );
    }

    SCut *newCut = new SCut( project, *newFragment );
    newCut->setWindow( srcCut->getSrcStart(),
                       ClipLen( srcCut->getDurationBlocking() ),
                       srcCut->getLoopLength(), srcCut->getStretchExact() );
    newCut->setGrainParams( srcCut->getGrainParams() );
    newCut->setSName( newAssetName_ );
    project->registerAsset( newAssetName_, newCut );

    return { true,
             new SUnregisterFragmentAssetAction( sourceAssetName_, newAssetName_ ) };
}

SApplyResult SUnregisterFragmentAssetAction::apply( SProject *project )
{
    if( !project ) return { false, nullptr };
    if( !project->asset( mintedAssetName_ ) ) return { false, nullptr };
    project->unregisterAsset( mintedAssetName_ );
    return { true,
             new SMintFragmentAssetAction( sourceAssetName_, mintedAssetName_ ) };
}

// First-unused "<asset> copy", "<asset> copy 2", ... — the same discipline
// create-asset's generateAssetName uses.
QString generateCopyName( SProject *project, const QString &base )
{
    QString candidate = base + QStringLiteral( " copy" );
    if( !project->hasAsset( candidate ) ) return candidate;
    for( int n = 2; ; ++n ) {
        candidate = QString( "%1 copy %2" ).arg( base ).arg( n );
        if( !project->hasAsset( candidate ) ) return candidate;
    }
}

}  // namespace

SDuplicateAssetHereAction::SDuplicateAssetHereAction(
        const QList<int> &clipPath, const QString &newAssetName )
    : clipPath_( clipPath ), newAssetName_( newAssetName )
{
}

SApplyResult SDuplicateAssetHereAction::apply( SProject *project )
{
    if( !project || clipPath_.isEmpty() ) {
        return { false, nullptr };
    }

    SObject *root = splacements::rootNamed( project, pathRoot_ );
    if( !root ) {
        return { false, nullptr };
    }

    QList<int> trackPath = clipPath_;
    const int clipIdx = trackPath.takeLast();
    SObject *lane = splacements::laneAt( root, trackPath );
    SLink *placement = lane ? lane->childAt( clipIdx ) : nullptr;
    if( !lane || !placement || placement->getSObject().isLane() ) {
        TW_LOGW( "cut", "duplicate-asset-here: refused, no clip at '%s'",
                 pathToString( clipPath_ ).toUtf8().constData() );
        return { false, nullptr };
    }

    SCut *oldCut = dynamic_cast<SCut *>( &placement->getSObject() );
    SLaneFragment *oldFragment = oldCut
        ? dynamic_cast<SLaneFragment *>( &oldCut->getContent() ) : nullptr;
    if( !oldCut || !oldFragment ) {
        TW_LOGW( "cut", "duplicate-asset-here: refused, '%s' is not a "
                        "fragment asset placement",
                 pathToString( clipPath_ ).toUtf8().constData() );
        return { false, nullptr };
    }

    const QString oldName = oldCut->getSName();
    if( project->asset( oldName ) != oldCut ) {
        TW_LOGW( "cut", "duplicate-asset-here: refused, '%s' is not the "
                        "registered asset '%s'",
                 pathToString( clipPath_ ).toUtf8().constData(),
                 oldName.toUtf8().constData() );
        return { false, nullptr };
    }

    const QString newName = newAssetName_.isEmpty()
                           ? generateCopyName( project, oldName )
                           : newAssetName_;
    if( project->hasAsset( newName ) ) {
        TW_LOGW( "cut", "duplicate-asset-here: refused, name '%s' is in use",
                 newName.toUtf8().constData() );
        return { false, nullptr };
    }

    const offset_t timePos = placement->getStartTime();

    // ONE undo step, following place-recording's precedent: a composite of
    // primitives. The mint step is custom (no such primitive pre-existed);
    // the other two are the SAME registered verbs place-asset/
    // remove-asset-placement already use for every other asset kind, which
    // is what gives AC2.6's cycle guard for free -- SPlaceAssetAction's
    // existing sarrangements::reaches check runs unmodified.
    SCompositeAction composite;
    SAction *mint = new SMintFragmentAssetAction( oldName, newName );
    mint->setPathRoot( pathRoot_ );
    composite.append( mint );
    SAction *unplace = new SRemoveAssetPlacementAction( oldName, trackPath,
                                                        clipIdx, timePos );
    unplace->setPathRoot( pathRoot_ );
    composite.append( unplace );
    SAction *place = new SPlaceAssetAction( newName, trackPath, timePos );
    place->setPathRoot( pathRoot_ );
    composite.append( place );

    return composite.apply( project );
}

void SDuplicateAssetHereAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "clip", qualifiedToString( pathRoot_, clipPath_ ) );
    if( !newAssetName_.isEmpty() ) elem.setAttribute( "name", newAssetName_ );
}

bool SDuplicateAssetHereAction::readXml( const QDomElement &elem, int /*version*/ )
{
    clipPath_ = parseInto( pathRoot_, elem.attribute( "clip", "" ) );
    newAssetName_ = elem.attribute( "name", "" );
    return true;
}

static const bool s_reg_duplicateassethere = (
    SActionRegistry::instance().registerType(
        QStringLiteral("duplicate-asset-here"),
        []{ return new SDuplicateAssetHereAction; }
    ), true
);
