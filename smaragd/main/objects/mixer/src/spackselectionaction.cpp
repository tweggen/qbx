#include "app/objects/mixer/spackselectionaction.h"
#include "app/objects/mixer/spackclipsaction.h"
#include "app/objects/track/strackpath.h"
#include "app/actions/sactionregistry.h"
#include "app/actions/scompositeaction.h"
#include "tw/core/twlog.h"
#include <QDomElement>

using namespace strackpath;

namespace spackselection {

QList<QList<QList<int>>> groupByLane( const QList<QList<int>> &clipPaths )
{
    QList<QList<int>>        laneOrder;   // first-seen lane paths
    QList<QList<QList<int>>> groups;      // parallel to laneOrder

    for( const QList<int> &cp : clipPaths ) {
        if( cp.isEmpty() ) continue;      // not a clip path; the action refuses
        QList<int> lanePath = cp;
        lanePath.removeLast();
        const int at = laneOrder.indexOf( lanePath );
        if( at < 0 ) {
            laneOrder.append( lanePath );
            groups.append( QList<QList<int>>{ cp } );
        } else {
            groups[ at ].append( cp );
        }
    }
    return groups;
}

int packableLaneCount( const QList<QList<int>> &clipPaths )
{
    int n = 0;
    for( const QList<QList<int>> &g : groupByLane( clipPaths ) )
        if( g.size() >= 2 ) ++n;
    return n;
}

}  // namespace spackselection

SPackSelectionAction::SPackSelectionAction( const QList<QList<int>> &clipPaths )
    : clipPaths_( clipPaths )
{
}

SApplyResult SPackSelectionAction::apply( SProject *project )
{
    if( !project || clipPaths_.isEmpty() ) {
        return { false, nullptr };
    }
    for( const QList<int> &cp : clipPaths_ ) {
        if( cp.isEmpty() ) {
            TW_LOGW( "cut", "pack-selection: refused, empty clip path" );
            return { false, nullptr };
        }
    }

    // ONE undo step for the whole gesture, whatever it touched: a composite of
    // unmodified pack-clips, one per lane holding two or more selected clips.
    // A lane holding exactly one contributes NOTHING -- not a no-op member,
    // no member at all -- so a mixed selection leaves its singletons exactly
    // where they were and the undo stack carries one entry either way.
    //
    // The members are independent by construction: packing lane A removes
    // clips from A and adds a placement to A, which cannot shift any index in
    // lane B, and no track is added or removed so no LANE path moves either.
    // Order between members is therefore free; first-seen is the stable one.
    SCompositeAction composite;
    int packedLanes = 0;
    for( const QList<QList<int>> &group :
             spackselection::groupByLane( clipPaths_ ) ) {
        if( group.size() < 2 ) continue;
        // Empty name: pack-clips generates the first unused "<lane name> N"
        // itself, per lane, which is exactly the automatic naming this verb
        // owes its caller.
        SAction *pack = new SPackClipsAction( group );
        pack->setPathRoot( pathRoot_ );
        composite.append( pack );
        ++packedLanes;
    }

    if( packedLanes == 0 ) {
        TW_LOGW( "cut", "pack-selection: refused, no lane in the selection "
                        "holds two or more clips (%d clip(s) over %d lane(s))",
                 (int) clipPaths_.size(),
                 (int) spackselection::groupByLane( clipPaths_ ).size() );
        return { false, nullptr };
    }

    return composite.apply( project );
}

void SPackSelectionAction::writeXml( QDomElement &elem ) const
{
    // Same spelling pack-clips uses: a semicolon-separated path list behind at
    // most ONE leading root qualifier.
    QStringList parts;
    for( const QList<int> &p : clipPaths_ ) parts << pathToString( p );
    const QString joined = parts.join( ";" );
    elem.setAttribute( "clips",
                       pathRoot_.isEmpty()
                           ? joined
                           : ( pathRoot_ + QLatin1Char(':') + joined ) );
}

bool SPackSelectionAction::readXml( const QDomElement &elem, int /*version*/ )
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

    return true;
}

static const bool s_reg_packselection = (
    SActionRegistry::instance().registerType(
        QStringLiteral("pack-selection"),
        []{ return new SPackSelectionAction; }
    ), true
);
