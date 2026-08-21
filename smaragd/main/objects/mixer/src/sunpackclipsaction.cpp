#include "app/objects/mixer/sunpackclipsaction.h"
#include "app/objects/mixer/spackclipsaction.h"
#include "app/objects/fragment/slanefragment.h"
#include "app/objects/cut/scut.h"
#include "app/objects/track/strackpath.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/model/slink.h"
#include "app/actions/sactionregistry.h"
#include "tw/core/twlog.h"
#include <QDomElement>

using namespace strackpath;

SUnpackClipsAction::SUnpackClipsAction( const QList<int> &clipPath )
    : clipPath_( clipPath )
{
}

SApplyResult SUnpackClipsAction::apply( SProject *project )
{
    if( !project || clipPath_.isEmpty() ) {
        return { false, nullptr };
    }

    SObject *root = splacements::rootNamed( project, pathRoot_ );
    if( !root ) {
        return { false, nullptr };
    }

    QList<int> lanePath = clipPath_;
    lanePath.removeLast();
    SObject *lane = splacements::laneAt( root, lanePath );
    SLink *placement = splacements::placementAt( root, clipPath_ );
    if( !lane || !placement ) {
        TW_LOGW( "cut", "unpack-clips: refused, no clip at '%s'",
                 pathToString( clipPath_ ).toUtf8().constData() );
        return { false, nullptr };
    }

    SCut *cut = dynamic_cast<SCut *>( &placement->getSObject() );
    SLaneFragment *fragment = cut
        ? dynamic_cast<SLaneFragment *>( &cut->getContent() ) : nullptr;
    if( !cut || !fragment ) {
        TW_LOGW( "cut", "unpack-clips: refused, '%s' is not a packed "
                        "fragment asset",
                 pathToString( clipPath_ ).toUtf8().constData() );
        return { false, nullptr };
    }

    const QString assetName = cut->getSName();
    if( project->asset( assetName ) != cut ) {
        TW_LOGW( "cut", "unpack-clips: refused, '%s' is not the registered "
                        "asset '%s'",
                 pathToString( clipPath_ ).toUtf8().constData(),
                 assetName.toUtf8().constData() );
        return { false, nullptr };
    }

    // AC2.4b: unpacking moves EVERY child out of the fragment, which would
    // leave any OTHER placement of this asset windowing an emptied container
    // -- breaking the sharing invariant D2 never permits. refCount() ==
    // registry pin (1) + one per placement; exactly 2 means this is the
    // ONLY one.
    if( cut->refCount() != 2 ) {
        TW_LOGW( "cut", "unpack-clips: refused, asset '%s' has other "
                        "placements; unpacking would break sharing",
                 assetName.toUtf8().constData() );
        return { false, nullptr };
    }

    const offset_t placementStart = placement->getStartTime();

    // Snapshot BEFORE mutating -- childLinks() is a live view over the
    // fragment's own order, and reparenting one child would otherwise mutate
    // the very list a range-for is walking.
    QList<SLink *> children;
    for( SLink *lk : fragment->childLinks() ) children.append( lk );

    for( SLink *child : children ) {
        const offset_t abs = placementStart + child->getStartTime();
        child->setParent( lane );
        child->setStartTime( abs );
    }

    // Indices are read only once every child has landed on `lane` -- moving
    // them one at a time could otherwise shift an already-restored sibling's
    // index before it is read.
    QList<QList<int>> restoredClipPaths;
    for( SLink *child : children ) {
        QList<int> p = lanePath;
        p.append( lane->indexOfChild( child ) );
        restoredClipPaths.append( p );
    }

    delete placement;                        // drops this placement's ref
    project->unregisterAsset( assetName );    // drops the registry pin

    return { true, new SPackClipsAction( restoredClipPaths, assetName ) };
}

void SUnpackClipsAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "clip", qualifiedToString( pathRoot_, clipPath_ ) );
}

bool SUnpackClipsAction::readXml( const QDomElement &elem, int /*version*/ )
{
    clipPath_ = parseInto( pathRoot_, elem.attribute( "clip", "" ) );
    return true;
}

static const bool s_reg_unpackclips = (
    SActionRegistry::instance().registerType(
        QStringLiteral("unpack-clips"),
        []{ return new SUnpackClipsAction; }
    ), true
);
