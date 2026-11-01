#ifndef _SSAMPLERSLOTS_H_
#define _SSAMPLERSLOTS_H_

// ssamplerslots.h — how objects/sampler finds the program of a slot it may not
// name (proposal 56 Q1, the Q-6 answer of 2026-10-27).
//
// A sampler verb is addressed by trackPath + slotIndex (+ a zone or layer id),
// as the plugin verbs are (Q-5). Turning trackPath + slotIndex into an
// SPluginSlot needs objects/track, and objects/sampler keeps the RANK OF
// objects/midi: no edge to objects/track. So the resolution is a HOOK the
// shell installs — the smediadrop::setPlacementHook shape — and the shell, the
// one module that sees both slices, does the walk:
//
//   trackPath (root-qualified, or bare + the action's pathRoot) -> STrack ->
//   its chain -> slot `slotIndex` -> refused unless it hosts NassauDigital ->
//   its SSamplerProgram child, created (and bound) on first use when `create`.
//
// Without a resolver (a unit test that never installed one) every lookup fails
// with a reason that says so.

#include <QList>
#include <QString>

#include <functional>

class SObject;
class SProject;
class SSamplerProgram;

namespace ssampler {

// `pathRoot` is the action's SAction::pathRoot() (empty = the master); a
// qualified trackPath ("Drums:0") names its own root and wins, the
// splacements::laneBySpec rule. Null + `why` when there is no such track, slot
// or NassauDigital instance, or (create == false) when the slot has no program
// object yet.
using Resolver = std::function<SSamplerProgram *( SProject *project, const QString &pathRoot,
                                                  const QString &trackPath, int slotIndex,
                                                  bool create, QString *why )>;

void setResolver( Resolver resolver );
bool hasResolver();

SSamplerProgram *programAt( SProject *project, const QString &pathRoot, const QString &trackPath,
                            int slotIndex, bool create, QString *why );

// --- the asset-link cycle walk (proposal 56 §4.7, Q3; DESIGN §6.2) ----------------
//
// A layer may not link an asset that reaches the sampler's own track: the
// bounce would render the instrument into its own sample. sarrangements::
// reaches() follows childLinks() only, and a plugin chain is deliberately NOT
// a child link, so it cannot see a cycle that runs THROUGH ANOTHER SAMPLER (A's
// layer links asset X, X holds track B, B's sampler links asset Y, Y holds A).
// The walk below therefore also follows, for every LANE it passes, the asset
// links of the NassauDigital programs on that lane's slots. Which programs sit
// on a lane is objects/track's knowledge, so it is another shell hook
// (ssamplerwiring installs it); without one the walk follows child links only.
using LanePrograms = std::function<QList<SSamplerProgram *>( SObject *lane )>;
void setLaneProgramsHook( LanePrograms hook );

// True when `track` is reachable from `body` through child links, content
// links and sampler asset links. `via`, when given, names the sampler hops the
// path took ("" when it took none), for the refusal's message. Main thread.
bool assetReachesTrack( SObject *body, const SObject *track, QString *via = nullptr );

}  // namespace ssampler

#endif
