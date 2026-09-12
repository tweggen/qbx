#ifndef STRACKGESTURES_H
#define STRACKGESTURES_H

#include <functional>

#include <QList>

class SAction;
class SStdMixer;
class STrack;

/**
 * THE TRACK-STRUCTURE GESTURES, ONCE (proposal 48 M4 / AC4.2).
 *
 * Remove, group and ungroup, as the arranger's track menu has always done
 * them -- extracted here so the MIXER PANE's strip menu can offer the same
 * three without a second copy of any of them. `SStdMixerView`'s `ct*` slots
 * are now thin wrappers over these; the strip calls them directly.
 *
 * **THE SUBMIT IS INJECTED, and that is the whole reason this is a namespace
 * with a callback rather than a method on the view.** D12: the mixer stamps
 * its OWN arrangement on every action it submits, because a pane showing one
 * arrangement must never commit into whichever editor TAB happens to be
 * active -- which is exactly what `stimeline::submitActive` does, and what the
 * arranger correctly wants. One body, two submitters.
 *
 * **WHAT IS DELIBERATELY NOT HERE, and why AC4.2's menu is three items rather
 * than the arranger's ten.** The rest of the head menu is not shareable and
 * says so by what it reads:
 *
 *   - `indentOne_` / `outdentOne_` resolve the PRECEDING SIBLING through
 *     `rowIndexOfTrack()` / `rowAt()` -- the arranger's visible ROW list. A
 *     mixer has no rows (D2), so indent and outdent would have to be
 *     re-expressed over `slaneorder::flattenTrackLanes` first. That is a
 *     rewrite of two working arranger gestures and it is not this milestone.
 *   - Take lanes, lane height and the automation-lane picker are ROW concepts
 *     outright; AC4.2 excludes them by name.
 *   - "Create asset from range" needs the ruler RANGE, and "Insert sample" a
 *     click POSITION on the timeline. Neither exists in a pane.
 *   - "New track" from the head menu aims BELOW the clicked lane, which is
 *     again a row position.
 *
 * The macro shape (begin/end on the app's undo stack) stays inside each
 * function: `app/timeline` can reach the undo stack, and the alternative --
 * injecting that too -- is a framework for one menu.
 */
namespace strackgestures {

/// The submitter. `SActionHistory` takes ownership of what it is handed, so
/// an implementation must submit rather than store.
using Submit = std::function<void( SAction * )>;

/// D3's target rule (`strackbroadcast::targetsFor`) with nested targets
/// pruned, which is what every STRUCTURAL verb below wants: removing a folder
/// takes its subtree, so a selected child inside a selected folder must not be
/// acted on twice.
QList<STrack *> structuralTargets( SStdMixer *mixer, STrack *clicked );

/// Remove each target and its subtree, bottom-up, as ONE macro when there is
/// more than one. Anything removed is dropped from the mixer's selection.
bool removeTracks( SStdMixer *mixer, const QList<STrack *> &targets,
                   const Submit &submit );

/// ONE new folder for the whole block, born in the first target's slot, then
/// every target reparented into it. One macro.
bool groupTracks( SStdMixer *mixer, const QList<STrack *> &targets,
                  const Submit &submit );

/// Promote each target folder's children into the folder's OWN parent, in
/// order, and delete the emptied folder. One macro.
bool ungroupTracks( SStdMixer *mixer, const QList<STrack *> &targets,
                    const Submit &submit );

}  // namespace strackgestures

#endif // STRACKGESTURES_H
