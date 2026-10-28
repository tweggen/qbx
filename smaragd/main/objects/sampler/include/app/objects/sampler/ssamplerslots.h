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

#include <QString>

#include <functional>

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

}  // namespace ssampler

#endif
