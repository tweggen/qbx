#ifndef _STAKEHELPERS_H_
#define _STAKEHELPERS_H_

class SProject;
class SObject;
class SLink;
class STakeStack;

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
 * THE take column a placement carries — on BOTH shapes.
 *
 * A take column reaches a lane two ways, and the difference is not cosmetic:
 *
 *   DIRECT    SLink -> STakeStack                (what add-take builds)
 *   WRAPPED   SLink -> SCut/SMidiCut -> STakeStack
 *
 * The wrapped one is what a SHARED or PLACED column is (each placement its
 * own window into one stack), and a real saved project carries it. Every
 * take-lane consumer must resolve BOTH or it silently serves one shape only:
 * the take-lane UI already unwrapped (SMVActualView's takeStackOfLink, which
 * now calls this), while `select-take` and the take-addressed half of
 * `resize-clip` did not — so a click on a take lane was REFUSED and a
 * take-lane slip wrote the TAKE's window onto the WRAPPER. One question
 * therefore gets one spelling, here, where both sides can reach it.
 *
 * It unwraps exactly ONE window level, through the generic `SClipWindow`
 * interface rather than through `SCut`: a stack is homogeneous by
 * contentKind, so an EVENT column wrapped by an `SMidiCut` is the same
 * question and deserves the same answer.
 *
 * Null when this placement is not a take column at all.
 */
STakeStack *columnOfLink( SLink *lk );

/**
 * PUBLISH a change of a take column's CONTENT IDENTITY through its placement.
 *
 * A take switch, or a slip of one take, changes which material the column
 * produces WITHOUT touching the placement's timeline extent. On the DIRECT
 * shape the track is connected to the stack's own `durationChanged` and does
 * the rest (`twTrackMix::updateClip` -- a content-epoch bump and a state-chain
 * reset -- plus the range invalidation). On the WRAPPED shape the link's
 * object is the WINDOW, and a window does not listen to its content's
 * `durationChanged`: that emission has NO listener at all, so the frozen
 * track / chain / mixer pages keep serving the old material and the edit is
 * INAUDIBLE. Measured, on `select-take` over a wrapped column: the take-lane
 * highlight flipped and the next render was byte-identical.
 *
 * So on the wrapped shape the WINDOW republishes its own, unchanged duration.
 * That is not a no-op and not a trick: `setDurationFromTimeline` has no
 * unchanged-value early-out, and what the track's slot then does is exactly
 * what the direct shape already gets. Dropping the wrapper's capture and
 * staling its render path are NOT substitutes -- measured, with both of those
 * and no republish the next render still served the old take.
 *
 * A no-op on the direct shape, where the track is already wired.
 */
void publishColumnChange( SLink *link, STakeStack *column );

/**
 * A DEEP COPY of a take column: a new stack holding a clone of every take's
 * window (the clones share the same CONTENT, exactly as `cloneWindowOver`
 * does everywhere else) and the same active index.
 *
 * A COLUMN BELONGS TO EXACTLY ONE PLACEMENT (proposal 42). A second placement
 * of the same stack object shares its `activeTake_`, so comping either one
 * comps both — which is what `duplicate-clip` and "add link" silently produced
 * before this existed, and is indistinguishable to the user from the comping
 * gesture not working. Whoever wants the SHARE semantics wants an asset
 * (proposal 41's fragment), where sharing is the stated invariant.
 *
 * Null if `column` is null or holds no take.
 */
STakeStack *cloneColumn( SProject *project, STakeStack &column );

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
