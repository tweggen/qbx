#include "app/objects/cut/stakehelpers.h"
#include "app/objects/cut/stakestack.h"
#include "app/objects/cut/scut.h"
#include "app/model/slink.h"
#include "app/model/sproject.h"

namespace stakes {

SLink *wrapCutLinkIntoStack( SProject *project, SObject *lane,
                             SLink *cutLink )
{
    if( !project || !lane || !cutLink ) return nullptr;
    SCut *cut = dynamic_cast<SCut *>( &cutLink->getSObject() );
    if( !cut ) return nullptr;

    const offset_t startTime = cutLink->getStartTime();

    STakeStack *stack = new STakeStack( project );
    stack->insertTake( *cut );        // refs the cut BEFORE the old link dies
    stack->setActiveTake( 0 );        // the wrapped material stays audible

    // The lane connected the cut's durationChanged when its link was added;
    // deleting the link does not sever an object-level connection. From now
    // on the cut reports to the stack, the stack to the lane.
    QObject::disconnect( cut, SIGNAL( durationChanged( length_t ) ),
                         lane, nullptr );

    SLink *stackLink = new SLink( *stack, nullptr );
    stackLink->setStartTime( startTime );
    const int origIndex = lane->indexOfChild( cutLink );
    delete cutLink;                   // lane removeClip fires
    stackLink->setParent( lane );     // lane insertClip fires (keyed anew)
    // setParent appends; restore the original index so clip paths recorded
    // in actions/inverses stay valid across the wrap (undo determinism).
    lane->moveChildToIndex( lane->indexOfChild( stackLink ), origIndex );
    return stackLink;
}

SLink *collapseSingleTakeStack( SObject *lane, SLink *stackLink )
{
    if( !lane || !stackLink ) return nullptr;
    STakeStack *stack = dynamic_cast<STakeStack *>( &stackLink->getSObject() );
    if( !stack || stack->nTakes() != 1 ) return nullptr;
    SCut *cut = stack->takeCutAt( 0 );
    if( !cut ) return nullptr;

    const offset_t startTime = stackLink->getStartTime();

    // Sever the stack's per-take forwarding before it is orphaned.
    QObject::disconnect( cut, SIGNAL( durationChanged( length_t ) ),
                         stack, nullptr );

    SLink *cutLink = new SLink( *cut, nullptr );   // ref before the stack dies
    cutLink->setStartTime( startTime );
    const int origIndex = lane->indexOfChild( stackLink );
    delete stackLink;                 // stack unreferenced → deleteLater
    cutLink->setParent( lane );
    // Keep the column at its original child index (path stability, as above).
    lane->moveChildToIndex( lane->indexOfChild( cutLink ), origIndex );
    return cutLink;
}

}  // namespace stakes
