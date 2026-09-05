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
