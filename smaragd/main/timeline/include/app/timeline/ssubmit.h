#ifndef SSUBMIT_H
#define SSUBMIT_H

class SAction;
class SStdMixerView;

/**
 * Submitting an action FROM THE ARRANGER, rooted at the arrangement that
 * arranger edits (proposal 09 D21).
 *
 * WHY THIS IS NOT THE AMBIENT ROOT D21 RETIRED. The objection to an ambient
 * root was that `apply()` read it long after the action was built -- possibly
 * after a tab switch -- so the action could resolve in a tree it was never
 * about. Here the root is read at SUBMIT time, from the view doing the
 * submitting, and then travels IN the action. A gesture handler builds and
 * submits in one synchronous call, so the active tab cannot change in between.
 *
 * WHY IT IS NOT FOLDED INTO SApplication::submitAction(). Not every submit
 * comes from a view: the recorders, the MIDI pump and the media drop all submit
 * master-rooted actions, and they would be mis-rooted the moment an arrangement
 * tab happened to be active. The funnel has to be the one the ARRANGER uses,
 * not the one everything uses.
 */
namespace stimeline {

/** Root `a` at the ACTIVE arranger's arrangement, then submit it. The active
 *  view is the one that owns any gesture in progress, and it is also what the
 *  docks (track detail, clip properties) act through. */
void submitActive( SAction *a );

/** Root `a` at `v`'s arrangement, then submit it. For the rare caller that
 *  holds a specific view rather than acting through the active one. */
void submitFor( SStdMixerView *v, SAction *a );

/** The arrangement name `v` edits, or empty for the master. */
const char *rootNameDoc();   // documentation anchor only; see SStdMixerView::rootName()

}  // namespace stimeline

#endif  // SSUBMIT_H
