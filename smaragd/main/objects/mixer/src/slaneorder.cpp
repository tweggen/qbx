#include "app/objects/mixer/slaneorder.h"

#include "app/model/slink.h"
#include "app/objects/mixer/sstdmixer.h"
#include "app/objects/track/strack.h"

namespace slaneorder {

namespace {

void appendSubtree( SObject *container, int depth, const Options &opt,
                    QVector<Lane> &out )
{
    if( !container ) return;
    for( SLink *lk : container->childLinks() ) {
        if( !lk ) continue;
        STrack *tk = dynamic_cast<STrack *>( &lk->getSObject() );
        if( !tk ) continue;      // clips render inside their track's own lane

        // HIDING IS ONE MECHANISM and it takes the subtree with it, which is
        // the same thing a collapsed lane does and the only reading under
        // which "hidden" means hidden.  A no-op for user lanes in practice
        // (`set-lane-hidden` refuses them outright); what it actually governs
        // is a conductor lane staying hidden until somebody asks for it.
        if( opt.hidden == Hidden::Honour && tk->laneHidden() ) continue;

        const bool kids = tk->hasChildTracks();
        const bool col  = tk->isCollapsed();

        Lane lane;
        lane.track       = tk;
        lane.link        = lk;
        lane.parent      = container;
        lane.depth       = depth;
        lane.hasChildren = kids;
        lane.collapsed   = col;
        lane.role        = tk->systemRole();
        out.append( lane );

        // The fold decision is the ONLY thing that differs between the two
        // mounts' walks, and it is one `if`.  That is the measurement behind
        // D1: the arranger and the mixer really do want the same list.
        const bool descend = kids && ( opt.fold == Fold::Ignore || !col );
        if( descend ) appendSubtree( tk, depth + 1, opt, out );
    }
}

void appendSystemLane( STrack *lane, SObject *parent, const Options &opt,
                       bool exemptFromHidden, QVector<Lane> &out )
{
    if( !lane ) return;
    const bool hiddenHere =
        opt.hidden == Hidden::Honour && lane->laneHidden() && !exemptFromHidden;
    if( hiddenHere ) return;

    Lane l;
    l.track       = lane;
    l.link        = nullptr;     // not a child of anything (proposal 45 D2)
    l.parent      = parent;
    l.depth       = 0;
    l.hasChildren = lane->hasChildTracks();
    l.collapsed   = lane->isCollapsed();
    l.role        = lane->systemRole();
    out.append( l );

    // ...and its own child lanes, which is where a conductor lane lives
    // (proposal 45 M6 / AC6.3).  Through the SAME recursion every user lane's
    // children go through, so a conductor lane is an ORDINARY lane: it carries
    // a real SLink and a real container, and a gesture on it therefore derives
    // its commit address by the ordinary route.  The exemption above is the
    // master lane's alone and is deliberately not passed down.
    const bool descend =
        l.hasChildren && ( opt.fold == Fold::Ignore || !l.collapsed );
    if( descend ) appendSubtree( lane, 1, opt, out );
}

}  // namespace

QVector<Lane> flattenTrackLanes( SObject *root, const Options &opt )
{
    QVector<Lane> out;
    if( !root ) return out;

    appendSubtree( root, 0, opt, out );

    // THE SYSTEM TAIL IS APPENDED, NOT WALKED TO.  `childLinks()` deliberately
    // does not contain the master lane -- it is not a child of the mixer, it
    // is the mixer's output stage -- so there is nothing to find, and
    // appending it once at the end is also exactly what "pinned below every
    // user lane" means (proposal 45 AC4.1).
    SStdMixer *mix = dynamic_cast<SStdMixer *>( root );
    if( !mix || opt.system == SystemLanes::None ) return out;

    // Sends first, master last (proposal 45 D11).  See the header: nothing
    // asks for `All` today, because a send lane has no arranger row.
    if( opt.system == SystemLanes::All )
        for( STrack *send : mix->sendLanes() )
            appendSystemLane( send, mix, opt, false, out );

    appendSystemLane( mix->masterLane(), mix, opt, opt.alwaysShowMaster, out );
    return out;
}

int indexOfTrack( const QVector<Lane> &lanes, const STrack *t )
{
    if( !t ) return -1;
    for( int i = 0; i < lanes.size(); ++i )
        if( lanes[i].track == t ) return i;
    return -1;
}

}  // namespace slaneorder
