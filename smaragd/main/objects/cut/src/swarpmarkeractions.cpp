#include "app/objects/cut/swarpmarkeractions.h"

#include <QDebug>
#include <QDomElement>

#include "app/actions/sactionregistry.h"
#include "app/model/sobjectpath.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/model/slink.h"
#include "app/objects/cut/scut.h"

using namespace strackpath;

namespace {

// Resolve the addressed SCut (nullptr on failure, logged).
SCut *resolveCut( SProject *project, const QString &pathRoot_,
                  const QList<int> &clipPath,
                  const char *verb )
{
    if( !project || clipPath.isEmpty() ) return nullptr;
    SObject *mixer = splacements::rootNamed( project, pathRoot_ );
    SLink *link = splacements::placementAt( mixer, clipPath );
    if( !link ) {
        qWarning() << verb << ": no clip at path" << pathToString( clipPath );
        return nullptr;
    }
    SCut *cut = dynamic_cast<SCut *>( &link->getSObject() );
    if( !cut )
        qWarning() << verb << ": target is not an SCut at path"
                   << pathToString( clipPath );
    return cut;
}

// The monotonicity gate: an edited list is legal iff sanitize() would keep
// it verbatim — anything else means the edit broke the strictly-increasing
// invariant against a neighbor and must be REJECTED, not repaired.
bool isLegal( const std::vector<twWarpAnchor> &edited )
{
    return twWarpMap::sanitize( edited ) == edited;
}

} // namespace

// ---------------------------------------------------------------- add

SAddWarpMarkerAction::SAddWarpMarkerAction( const QList<int> &clipPath,
                                            int64_t src, int64_t warped )
    : clipPath_( clipPath ), src_( src ), warped_( warped )
{
}

SApplyResult SAddWarpMarkerAction::apply( SProject *project )
{
    SCut *cut = resolveCut( project, pathRoot_, clipPath_, "add-warp-marker" );
    if( !cut ) return { false, nullptr };

    std::vector<twWarpAnchor> anchors = cut->getGrainParams().warpAnchors;
    for( const twWarpAnchor &a : anchors )
        if( a.src == src_ ) {
            qWarning() << "add-warp-marker: anchor at src" << (qlonglong) src_
                       << "already exists";
            return { false, nullptr };
        }
    anchors.push_back( { src_, warped_ } );
    std::sort( anchors.begin(), anchors.end(),
               []( const twWarpAnchor &a, const twWarpAnchor &b ) {
                   return a.src < b.src;
               } );
    if( !isLegal( anchors ) ) {
        qWarning() << "add-warp-marker: (" << (qlonglong) src_ << ":"
                   << (qlonglong) warped_ << ") breaks monotonicity — rejected";
        return { false, nullptr };
    }
    cut->setWarpAnchors( anchors );
    return { true, new SDeleteWarpMarkerAction( clipPath_, src_ ) };
}

void SAddWarpMarkerAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "clip", qualifiedToString( pathRoot_, clipPath_ ) );
    elem.setAttribute( "src", QString::number( src_ ) );
    elem.setAttribute( "warped", QString::number( warped_ ) );
}

bool SAddWarpMarkerAction::readXml( const QDomElement &elem, int )
{
    clipPath_ = parseInto( pathRoot_, elem.attribute( "clip" ) );
    src_      = elem.attribute( "src", "0" ).toLongLong();
    warped_   = elem.attribute( "warped", "0" ).toLongLong();
    return true;
}

static const bool s_reg_add_warp_marker = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "add-warp-marker" ),
        [] { return new SAddWarpMarkerAction; } ),
    true );

// ---------------------------------------------------------------- move

SMoveWarpMarkerAction::SMoveWarpMarkerAction( const QList<int> &clipPath,
                                              int64_t src, int64_t newWarped )
    : clipPath_( clipPath ), src_( src ), newWarped_( newWarped )
{
}

SApplyResult SMoveWarpMarkerAction::apply( SProject *project )
{
    SCut *cut = resolveCut( project, pathRoot_, clipPath_, "move-warp-marker" );
    if( !cut ) return { false, nullptr };

    std::vector<twWarpAnchor> anchors = cut->getGrainParams().warpAnchors;
    int64_t oldWarped = -1;
    for( twWarpAnchor &a : anchors )
        if( a.src == src_ ) {
            oldWarped = a.warped;
            a.warped  = newWarped_;
            break;
        }
    if( oldWarped < 0 ) {
        qWarning() << "move-warp-marker: no anchor at src" << (qlonglong) src_;
        return { false, nullptr };
    }
    if( !isLegal( anchors ) ) {
        qWarning() << "move-warp-marker: warped" << (qlonglong) newWarped_
                   << "breaks monotonicity — rejected";
        return { false, nullptr };
    }
    cut->setWarpAnchors( anchors );
    return { true, new SMoveWarpMarkerAction( clipPath_, src_, oldWarped ) };
}

void SMoveWarpMarkerAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "clip", qualifiedToString( pathRoot_, clipPath_ ) );
    elem.setAttribute( "src", QString::number( src_ ) );
    elem.setAttribute( "newWarped", QString::number( newWarped_ ) );
}

bool SMoveWarpMarkerAction::readXml( const QDomElement &elem, int )
{
    clipPath_  = parseInto( pathRoot_, elem.attribute( "clip" ) );
    src_       = elem.attribute( "src", "0" ).toLongLong();
    newWarped_ = elem.attribute( "newWarped", "0" ).toLongLong();
    return true;
}

static const bool s_reg_move_warp_marker = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "move-warp-marker" ),
        [] { return new SMoveWarpMarkerAction; } ),
    true );

// ---------------------------------------------------------------- delete

SDeleteWarpMarkerAction::SDeleteWarpMarkerAction( const QList<int> &clipPath,
                                                  int64_t src )
    : clipPath_( clipPath ), src_( src )
{
}

SApplyResult SDeleteWarpMarkerAction::apply( SProject *project )
{
    SCut *cut = resolveCut( project, pathRoot_, clipPath_, "delete-warp-marker" );
    if( !cut ) return { false, nullptr };

    std::vector<twWarpAnchor> anchors = cut->getGrainParams().warpAnchors;
    int64_t oldWarped = -1;
    for( size_t i = 0; i < anchors.size(); i++ )
        if( anchors[i].src == src_ ) {
            oldWarped = anchors[i].warped;
            anchors.erase( anchors.begin() + i );
            break;
        }
    if( oldWarped < 0 ) {
        qWarning() << "delete-warp-marker: no anchor at src"
                   << (qlonglong) src_;
        return { false, nullptr };
    }
    // Removal can never break monotonicity.
    cut->setWarpAnchors( anchors );
    return { true, new SAddWarpMarkerAction( clipPath_, src_, oldWarped ) };
}

void SDeleteWarpMarkerAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "clip", qualifiedToString( pathRoot_, clipPath_ ) );
    elem.setAttribute( "src", QString::number( src_ ) );
}

bool SDeleteWarpMarkerAction::readXml( const QDomElement &elem, int )
{
    clipPath_ = parseInto( pathRoot_, elem.attribute( "clip" ) );
    src_      = elem.attribute( "src", "0" ).toLongLong();
    return true;
}

static const bool s_reg_delete_warp_marker = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "delete-warp-marker" ),
        [] { return new SDeleteWarpMarkerAction; } ),
    true );
