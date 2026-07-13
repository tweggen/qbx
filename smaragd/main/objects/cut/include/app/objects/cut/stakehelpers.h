#ifndef _STAKEHELPERS_H_
#define _STAKEHELPERS_H_

class SProject;
class SObject;
class SLink;

/**
 * Tree canonicalization for take stacks (proposal 17, invariant 3): a stack
 * exists only while it holds ≥2 takes. Both helpers replace a lane child
 * link IN PLACE (delete old link, parent new link — the standard wrap
 * pattern from split-clip), so the lane's engine sync (removeClip/
 * insertClip) runs through the normal childObjectAdded/Removed path.
 *
 * Reference order is load-bearing: the new holder links the content BEFORE
 * the old link is deleted, so the refcount never touches zero.
 */
namespace stakes {

/**
 * Wrap a plain-cut placement into a single-take stack (take 0 = the cut,
 * active). Returns the new stack link on the lane, or null if cutLink's
 * object is not an SCut.
 */
SLink *wrapCutLinkIntoStack( SProject *project, SObject *lane,
                             SLink *cutLink );

/**
 * Replace a single-take stack placement by a plain placement of its
 * remaining take cut. Returns the new cut link, or null if stackLink's
 * object is not a one-take STakeStack.
 */
SLink *collapseSingleTakeStack( SObject *lane, SLink *stackLink );

}  // namespace stakes

#endif // _STAKEHELPERS_H_
