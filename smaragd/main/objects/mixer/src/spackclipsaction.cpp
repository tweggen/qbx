#include "app/objects/mixer/spackclipsaction.h"
#include "app/objects/mixer/sunpackclipsaction.h"
#include "app/objects/fragment/slanefragment.h"
#include "app/objects/cut/scut.h"
#include "app/objects/track/strackpath.h"
#include "app/model/splacements.h"
#include "app/model/sarrangements.h"
#include "app/model/sproject.h"
#include "app/model/slink.h"
#include "app/actions/sactionregistry.h"
#include "tw/core/twfraction.h"
#include "tw/core/twlog.h"
#include <QDomElement>
#include <algorithm>

using namespace strackpath;

namespace {

// First-unused "<lane name> N", mirroring create-asset's generateAssetName
// (screateassetaction.cpp) -- D9's fallback order ("user-supplied -> first
// child's source basename -> <track name> N") stops at the track name here;
// the source-basename tier is a nicety left for a later pass.
QString generatePackName( SProject *project, const QString &laneName )
{
    const QString base = laneName.isEmpty() ? QStringLiteral( "Fragment" )
                                             : laneName;
    for( int n = 1; ; ++n ) {
        const QString candidate = QString( "%1 %2" ).arg( base ).arg( n );
        if( !project->hasAsset( candidate ) ) return candidate;
    }
}

}  // namespace

SPackClipsAction::SPackClipsAction( const QList<QList<int>> &clipPaths,
                                    const QString &assetName )
    : clipPaths_( clipPaths ), assetName_( assetName )
{
}

SApplyResult SPackClipsAction::apply( SProject *project )
{
    if( !project || clipPaths_.isEmpty() ) {
        return { false, nullptr };
    }

    SObject *root = splacements::rootNamed( project, pathRoot_ );
    if( !root ) {
        return { false, nullptr };
    }

    struct Sel { SObject *lane; SLink *link; };
    QList<Sel> sels;
    QList<int> commonLanePath;
    QString    commonLaneName;
    bool       haveCommon = false;

    for( const QList<int> &cp : clipPaths_ ) {
        if( cp.isEmpty() ) {
            TW_LOGW( "cut", "pack-clips: refused, empty clip path" );
            return { false, nullptr };
        }
        QList<int> lanePath = cp;
        lanePath.removeLast();
        SObject *lane = splacements::laneAt( root, lanePath );
        SLink *link = splacements::placementAt( root, cp );
        if( !lane || !link ) {
            TW_LOGW( "cut", "pack-clips: refused, no clip at '%s'",
                     pathToString( cp ).toUtf8().constData() );
            return { false, nullptr };
        }
        if( !haveCommon ) {
            commonLanePath = lanePath;
            commonLaneName = lane->getSName();
            haveCommon = true;
        } else if( lanePath != commonLanePath ) {
            // AC2.5: a same-lane selection is the whole point (D8 -- a
            // fragment is single-lane by construction). Name BOTH lanes so
            // the refusal is actionable, not just "no".
            TW_LOGW( "cut", "pack-clips: refused, selection spans two lanes "
                            "('%s' and '%s')",
                     commonLaneName.toUtf8().constData(),
                     lane->getSName().toUtf8().constData() );
            return { false, nullptr };
        }
        sels.append( { lane, link } );
    }

    SObject *lane = sels.first().lane;

    // Order by current start time: the fragment's children keep the
    // selection's temporal order, and the FIRST one (earliest start) anchors
    // the group at fragment-relative 0.
    std::sort( sels.begin(), sels.end(), []( const Sel &a, const Sel &b ) {
        return a.link->getStartTime() < b.link->getStartTime();
    } );

    const offset_t groupStart = sels.first().link->getStartTime();
    offset_t groupEnd = groupStart;
    for( const Sel &s : sels ) {
        const offset_t st = s.link->getStartTime();
        const length_t dur = s.link->getSObject().hasDuration()
                            ? s.link->getSObject().getDurationBlocking() : 0;
        groupEnd = std::max( groupEnd, st + (offset_t) dur );
    }
    if( groupEnd <= groupStart ) {
        TW_LOGW( "cut", "pack-clips: refused, empty selection extent" );
        return { false, nullptr };
    }

    // AC2.6, reusing sarrangements::reaches verbatim (no second guard):
    // would re-placing this material's fragment back on `lane` close a
    // reference cycle? Checked against each selected clip's CONTENT, BEFORE
    // anything moves, so a refusal here has mutated nothing. A clip that is
    // itself a plain wave/sequence never reaches anywhere; a clip that is
    // itself an ASSET PLACEMENT (windowing a container, or another fragment)
    // can -- that is the shape proposal 41 newly makes reachable, since a
    // fragment may now hold arbitrary clip content including asset
    // placements (see fragment_pack_multilane_refused.qxa's second scenario).
    for( const Sel &s : sels ) {
        if( sarrangements::reaches( &s.link->getSObject(), lane ) ) {
            TW_LOGW( "cut", "pack-clips: refused, packing '%s' here would "
                            "close a reference cycle",
                     s.link->getSObject().getSName().toUtf8().constData() );
            return { false, nullptr };
        }
    }

    // --- move the selected clips into a new fragment, relative to groupStart
    SLaneFragment *fragment = new SLaneFragment( project );
    for( Sel &s : sels ) {
        const offset_t abs = s.link->getStartTime();
        s.link->setParent( fragment );        // the link persists; identity kept
        s.link->setStartTime( abs - groupStart );
    }

    const QString assetName = assetName_.isEmpty()
                             ? generatePackName( project, commonLaneName )
                             : assetName_;

    SCut *cut = new SCut( project, *fragment );
    cut->setWindow( Fraction( 0 ), ClipLen( (length_t)( groupEnd - groupStart ) ),
                    WarpedLen( 0 ), Fraction( 1 ) );
    cut->setSName( assetName );
    project->registerAsset( assetName, cut );

    // --- one placement, exactly where the material was ---------------------
    SLink *placement = new SLink( *cut, nullptr );
    placement->setStartTime( groupStart );
    placement->setParent( lane );

    const int placementIdx = lane->indexOfChild( placement );
    if( placementIdx < 0 ) {
        return { false, nullptr };   // never landed; do not fabricate an inverse
    }
    QList<int> placementPath = commonLanePath;
    placementPath.append( placementIdx );

    return { true, new SUnpackClipsAction( placementPath ) };
}

void SPackClipsAction::writeXml( QDomElement &elem ) const
{
    QStringList parts;
    for( const QList<int> &p : clipPaths_ ) parts << pathToString( p );
    const QString joined = parts.join( ";" );
    elem.setAttribute( "clips",
                       pathRoot_.isEmpty() ? joined
                                          : ( pathRoot_ + QLatin1Char(':') + joined ) );
    elem.setAttribute( "name", assetName_ );
}

bool SPackClipsAction::readXml( const QDomElement &elem, int /*version*/ )
{
    const QString spec = elem.attribute( "clips", "" );
    QString root, rest = spec;
    const int colon = spec.indexOf( QLatin1Char(':') );
    if( colon >= 0 ) {
        root = spec.left( colon );
        rest = spec.mid( colon + 1 );
    }
    if( !root.isEmpty() ) pathRoot_ = root;

    clipPaths_.clear();
    for( const QString &tok : rest.split( QLatin1Char(';'), Qt::SkipEmptyParts ) )
        clipPaths_.append( stringToPath( tok.trimmed() ) );

    assetName_ = elem.attribute( "name", "" );
    return true;
}

static const bool s_reg_packclips = (
    SActionRegistry::instance().registerType(
        QStringLiteral("pack-clips"),
        []{ return new SPackClipsAction; }
    ), true
);
