#include "app/model/splacements.h"

#include "tw/core/twlog.h"

namespace splacements {

bool refuseSystemLane( SObject *obj, const char *verb )
{
    if( !obj || !obj->isSystemLane() ) return false;
    TW_LOGW( "model",
             "%s: refused on the %s lane '%s' (a system lane is not a user "
             "track: proposal 45 D6)",
             verb,
             systemRoleToString( obj->systemRole() ),
             obj->getSName().toUtf8().constData() );
    return true;
}


SObject *laneBySpec( SProject *project, const QString &spec, QString &rootInOut,
                     SObject **rootOut )
{
    if( rootOut ) *rootOut = nullptr;
    if( !project ) return nullptr;

    // THE SEND-NAME REWRITE (proposal 45 M7 / AC7.2), and it has to happen
    // HERE rather than in stringToPath() because a name maps to a sentinel
    // only against a particular root, and stringToPath is pure -- it is called
    // from readXml(), where there is no project. So the order is: peel the
    // root qualifier, resolve the root, THEN rewrite "$send:<name>" to the
    // "$sendN" the sentinel machinery already understands.
    //
    // The qualifier is peeled by parseQualified, which since M7 leaves a
    // leading '$'-token alone -- so "$send:Reverb" is a path and
    // "Drums:$send:Reverb" is that path in the Drums arrangement.
    QString work = spec.trimmed();
    const int colon = work.indexOf( QLatin1Char( ':' ) );
    if( colon > 0 && !work.startsWith( QLatin1Char( '$' ) ) ) {
        if( rootInOut.isEmpty() ) rootInOut = work.left( colon );
        work = work.mid( colon + 1 );
    }

    SObject *root = rootNamed( project, rootInOut );
    if( !root ) return nullptr;
    if( rootOut ) *rootOut = root;

    if( work.startsWith( QLatin1String( "$send:" ) ) ) {
        const QString name = work.mid( 6 ).section( QLatin1Char( ',' ), 0, 0 );
        const QString rest = work.section( QLatin1Char( ',' ), 1 );
        const int k = root->systemLaneIndexNamed( name );
        if( k < 0 ) {
            TW_LOGW( "model",
                     "no send lane named '%s' in this arrangement",
                     name.toUtf8().constData() );
            return nullptr;
        }
        work = QStringLiteral( "$send%1" ).arg( k );
        if( !rest.isEmpty() ) work += QLatin1Char( ',' ) + rest;
    }

    bool ok = false;
    const QList<int> path = strackpath::stringToPath( work, &ok );
    if( !ok ) return nullptr;
    return strackpath::resolveByPath( root, path );
}

SObject *placementLaneAt( SObject *root, const QList<int> &path,
                          const char *verb )
{
    SObject *lane = laneAt( root, path );
    if( !lane ) return nullptr;
    if( !lane->acceptsClips() ) {
        TW_LOGW( "model",
                 "%s: refused - the %s lane '%s' does not carry clips "
                 "(proposal 45 D6). Place the material on a user track and "
                 "route it here.",
                 verb,
                 systemRoleToString( lane->systemRole() ),
                 lane->getSName().toUtf8().constData() );
        return nullptr;
    }
    return lane;
}

}  // namespace splacements
