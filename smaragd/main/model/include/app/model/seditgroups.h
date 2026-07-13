#ifndef SEDITGROUPS_H
#define SEDITGROUPS_H

#include "app/model/sobject.h"
#include "app/model/sobjectpath.h"
#include "app/model/splacements.h"
#include "app/model/slink.h"
#include "app/model/sproject.h"
#include <QList>

/**
 * Edit groups (proposal 17 phase 4, decision 4): tracks sharing a nonzero
 * `SObject::editGroup()` id form one ARBITRARY set — membership is not tied
 * to the tree. A clip edit on one member is carried out on every member's
 * CORRESPONDING clip; correspondence is positional (same startTime +
 * duration), which is sound because the group lock itself keeps windows
 * aligned: every length/placement edit while grouped is broadcast, and a
 * multi-mic recording pass creates identical columns on all members.
 *
 * Model-level on purpose: the clip verbs live in objects/cut AND
 * objects/track, which may not include each other (check_layering.py) —
 * both reach the group logic through here.
 */
namespace seditgroups {

// All path-container lanes in `root`'s subtree with editGroup == id.
inline void membersOf( SObject *root, int id, QList<SObject *> &out )
{
    if( !root || id == 0 ) return;
    for( SLink *lk : root->childLinks() ) {
        SObject &o = lk->getSObject();
        if( !o.isPathContainer() ) continue;
        if( o.getEditGroup() == id ) out.append( &o );
        membersOf( &o, id, out );
    }
}

// The lane itself plus every descendant lane (the folder-track "G" shortcut).
inline void collectSubtreeLanes( SObject *lane, QList<SObject *> &out )
{
    if( !lane ) return;
    out.append( lane );
    for( SLink *lk : lane->childLinks() ) {
        if( lk->getSObject().isPathContainer() )
            collectSubtreeLanes( &lk->getSObject(), out );
    }
}

// Highest edit-group id in use (fresh id = this + 1).
inline int maxEditGroupId( SObject *root )
{
    int maxId = 0;
    if( !root ) return 0;
    for( SLink *lk : root->childLinks() ) {
        SObject &o = lk->getSObject();
        if( o.isPathContainer() ) {
            if( o.getEditGroup() > maxId ) maxId = o.getEditGroup();
            int sub = maxEditGroupId( &o );
            if( sub > maxId ) maxId = sub;
        }
    }
    return maxId;
}

// The member lane's clip corresponding to the anchor window, or null.
inline SLink *correspondingClip( SObject *lane, offset_t startTime,
                                 length_t duration )
{
    for( SLink *lk : lane->childLinks() ) {
        if( lk->getSObject().isPathContainer() ) continue;
        if( !lk->getSObject().hasDuration() ) continue;
        if( lk->getStartTime() == startTime
            && lk->getSObject().getDuration() == duration )
            return lk;
    }
    return nullptr;
}

/**
 * Expand a clip path to its edit-group counterparts: the anchor path FIRST,
 * then one path per other member lane that has a corresponding clip
 * (members without one are skipped — "as applicable"). A clip outside any
 * group expands to just itself.
 */
inline QList<QList<int>> expandClipPaths( SProject *project,
                                          const QList<int> &clipPath )
{
    QList<QList<int>> out;
    out.append( clipPath );
    SObject *root = splacements::rootContainer( project );
    if( !root || clipPath.isEmpty() ) return out;
    QList<int> lanePath = clipPath;
    int idx = lanePath.takeLast();
    SObject *lane = splacements::laneAt( root, lanePath );
    SLink *anchor = lane ? lane->childAt( idx ) : nullptr;
    if( !lane || !anchor || anchor->getSObject().isPathContainer() ) return out;
    const int group = lane->getEditGroup();
    if( group == 0 ) return out;

    const offset_t start = anchor->getStartTime();
    const length_t dur = anchor->getSObject().hasDuration()
        ? anchor->getSObject().getDuration() : 0;

    QList<SObject *> members;
    membersOf( root, group, members );
    for( SObject *member : members ) {
        if( member == lane ) continue;
        SLink *clip = correspondingClip( member, start, dur );
        if( !clip ) continue;
        QList<int> p = strackpath::pathOf( root, member );
        if( p.isEmpty() && member != root ) continue;   // dangling
        p.append( member->indexOfChild( clip ) );
        out.append( p );
    }
    return out;
}

}  // namespace seditgroups

#endif // SEDITGROUPS_H
