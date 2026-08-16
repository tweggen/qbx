#ifndef SINSTRUMENTTRACKS_H
#define SINSTRUMENTTRACKS_H

#include <vector>

class SObject;
class STrack;

/**
 * The RUN BARRIER's registry (proposal 37 D4 / 4.4, built in P3c).
 *
 * A run — an offline render, or a playback start — must begin with every
 * INSTRUMENT in the project having forgotten where the previous run left off,
 * and with the path from every instrument track up to the root re-rendered from
 * the run's start position onward. `SApplication::beginRun()` is the one caller;
 * this header is only the WALK, so the shell does not have to know how a track
 * tree is shaped or what makes a slot an instrument.
 *
 * A WALK, not a registration list, deliberately. A list would have to be kept in
 * step with insert-plugin, remove-plugin, reorder-plugin, undo of each of those,
 * track add/remove/reparent and project load — nine places, every one of them a
 * chance to leak a stale pointer into a structure the barrier then dereferences.
 * The walk is O(lanes) once per transport start (it never runs per page, per
 * block or per edit), and `STrack::instrumentSlot()` is two pointer hops, so the
 * cost is nothing measurable next to opening an audio device.
 *
 * Only LANES are descended into (`SObject::isPathContainer()`), exactly as
 * ssolo's walks do: a clip is never a track and a folder's instrument is as much
 * an instrument as a leaf's (3.2.1 — the folder is where a drum kit fed by
 * bubbled-up children lives).
 *
 * MAIN THREAD ONLY. It reads the model tree, which is the UI thread's.
 */
namespace sinstruments {

// Append every track at or below `root` whose slot 0 is an instrument, in
// depth-first order. `root` itself is the project's root container (an
// SStdMixer) and is not a track, so it is never tested.
void collectInstrumentTracks( SObject *root, std::vector<STrack *> &out );

}  // namespace sinstruments

#endif  // SINSTRUMENTTRACKS_H
