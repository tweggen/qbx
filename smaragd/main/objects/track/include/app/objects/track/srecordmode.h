#ifndef _SRECORDMODE_H_
#define _SRECORDMODE_H_

#include <QString>

class SObject;
class SProject;

/**
 * WHICH RECORD MODE APPLIES TO A LANE (plan 53 §5.2, QBX-156 M0).
 *
 * Four modes decide how a recording lands on a lane that already has material:
 *
 *   takes        union comping -- the recorded span joins the existing column
 *                (plan 53 mode A, the eventual default)
 *   takes-split  split-and-stack (mode B)
 *   overdub      a plain clip on top; both sound
 *   replace      the recording wins; what it covers is trimmed or removed
 *
 * WHY THIS LIVES IN objects/track AND NOT BESIDE place-recording. Resolution
 * needs STrack::hasChildTracks() to walk the folder chain, and
 * tools/check_layering.py gives objects/cut the edges
 * {actions, model, objects/wave, persistence} -- NOT objects/track. So
 * `place-recording` cannot resolve anything; it only PARSES the mode it is
 * handed. That matches plan 53 §5.3, where the RECORDER resolves per armed lane
 * and submits the verb with `mode=` already set, and it is why §5.2's table
 * should not be read as putting the resolver next to the verb.
 */
namespace srecmode {

/**
 * The mode in force for `lane`: its own attribute, else the nearest ancestor
 * folder track's (innermost wins), else the project's property, else
 * SProjectProps::recordModeDefault().
 *
 * `root` is the project's root container (SStdMixer), because the ancestor
 * chain is found by DESCENDING from it: SObject has no parent pointer -- the
 * tree is SObject -> SLink -> SObject and only the links point down. This is
 * the same shape, and the same reason, as ssolo::underSoloedAncestor.
 *
 * A null root or a lane that is not under it resolves to the project's mode
 * rather than guessing; a null project resolves to the default.
 */
QString resolveMode( const SObject *root, const SObject *lane,
                     const SProject *project );

/** The project-wide mode, ignoring every track attribute. */
QString projectMode( const SProject *project );

} // namespace srecmode

#endif // _SRECORDMODE_H_
