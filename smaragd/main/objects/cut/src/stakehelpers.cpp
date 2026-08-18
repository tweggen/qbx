#include "app/objects/cut/stakehelpers.h"
#include "app/objects/cut/stakestack.h"
#include "app/model/sclipwindow.h"
#include "app/model/slink.h"
#include "app/model/sproject.h"
#include "app/model/sclipwindow.h"

namespace stakes {

SLink *wrapCutLinkIntoStack( SProject *project, SObject *lane,
                             SLink *cutLink )
{
    if( !project || !lane || !cutLink ) return nullptr;
    SClipWindow *win = SClipWindow::of( &cutLink->getSObject() );
    if( !win ) return nullptr;
    SObject &winObj = win->asObject();

    const offset_t startTime = cutLink->getStartTime();

    STakeStack *stack = new STakeStack( project );
    if( !stack->insertTake( *win ) ) {  // refs it BEFORE the old link dies
        delete stack;                   // cannot be refused into an empty stack
        return nullptr;
    }
    stack->setActiveTake( 0 );        // the wrapped material stays audible

    // The lane connected the window's durationChanged when its link was added;
    // deleting the link does not sever an object-level connection. From now
    // on the window reports to the stack, the stack to the lane.
    QObject::disconnect( &winObj, SIGNAL( durationChanged( length_t ) ),
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
    SObject *take = stack->takeObjectAt( 0 );
    if( !take ) return nullptr;

    const offset_t startTime = stackLink->getStartTime();

    // Sever the stack's per-take forwarding before it is orphaned.
    QObject::disconnect( take, SIGNAL( durationChanged( length_t ) ),
                         stack, nullptr );

    SLink *cutLink = new SLink( *take, nullptr );  // ref before the stack dies
    cutLink->setStartTime( startTime );
    const int origIndex = lane->indexOfChild( stackLink );
    delete stackLink;                 // stack unreferenced → deleteLater
    cutLink->setParent( lane );
    // Keep the column at its original child index (path stability, as above).
    lane->moveChildToIndex( lane->indexOfChild( cutLink ), origIndex );
    return cutLink;
}

}  // namespace stakes

// Register the pair with the model's generic take-COLUMN factory, so a slice
// that may not depend on objects/cut (objects/midi, at the same rank) can
// still turn a plain placement into a column and collapse it back. A static
// initializer, exactly like SClipWindow::registerWrapFactory in scut.cpp -
// and it works for the same reason: `smaragd_app` is an OBJECT library, so
// nothing strips a translation unit whose only reference is a static ctor.
static const bool s_reg_take_column = (
    SClipWindow::registerTakeColumnFactory( &stakes::wrapCutLinkIntoStack,
                                            &stakes::collapseSingleTakeStack ),
    true );
