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

#include "app/model/sarrangements.h"

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

// ROUTING EDGES ARE EDGES TOO (QBX-269). A lane's output can depend on a lane
// it does not contain: a return track renders its source's aux bus, a keyed
// slot reads its key lane, and a send lane sums every lane that sends to it.
// An asset over a folder holding such a return or keyed track therefore
// bounces audio from OUTSIDE its container (measured: the bounce of a return
// of a sampler's aux 1 carries that sampler's kick), and a sampler that links
// it re-bounces itself forever. For every LANE it passes the walk also follows
// the lanes that lane's output is routed FROM. Which lanes those are is
// objects/track's and objects/mixer's knowledge, so it is a shell hook as well
// (ssamplerwiring installs it); each source comes with the hop's wording for a
// refusal ("the return \"R\" taps \"S\"").
struct RoutingSource {
    SObject *source = nullptr;
    QString  hop;
};
using LaneRoutingSources = std::function<QList<RoutingSource>( SObject *lane )>;
void setLaneRoutingSourcesHook( LaneRoutingSources hook );

// True when `track` is reachable from `body` through child links, content
// links, sampler asset links and (QBX-269) routing edges. `via`, when given,
// names the sampler and routing hops the path took, in path order, for the
// refusal's message ("" when it took neither). `samplerHop`, when given, says
// whether the path took a SAMPLER hop: a path through routing edges alone is a
// plain reference cycle, which the containment verbs word differently.
// Main thread.
//
// It is ALSO the walk every containment and placement verb asks (QBX-267):
// the shell installs it as sarrangements' reach walk, so a cycle a later edit
// would close (reparent-track, move-clip, place-asset, ...) is refused by the
// same check as a link that would close one. For that use `body` and `track`
// are any two objects ("is `track` reachable from `body`"), and `pending`
// lists edges the edit is about to add (dissolve-arrangement's several moves).
bool assetReachesTrack( SObject *body, const SObject *track, QString *via = nullptr,
                        const sarrangements::PendingEdges *pending = nullptr,
                        bool *samplerHop = nullptr );

// --- a bounce over a LIVE-OWNED track waits (QBX-274) -------------------------------
//
// A bounce freezes the asset's content on a worker. When that content holds a
// track the live lane OWNS (armed, proposal 21 D4), its processor refuses the
// freeze and answers silence, so the bounce would be silent there. The
// author's answer (2026-11-04): the bounce is DEFERRED until the track is
// disarmed; the layer keeps its previous generation and is marked waiting.
// Which tracks the live lane owns is the shell's knowledge (SLiveMonitor), so
// it is a hook: given an asset BODY, the spec of a live-owned track the body
// reaches through child and content links ("" when none, or no hook). A
// render's suspension releases ownership, so during an offline render's
// settle nothing is owned and the bounce runs fresh (the author, 2026-11-04).
using LiveOwner = std::function<QString( SObject *body )>;
void    setLiveOwnerHook( LiveOwner hook );
QString liveOwnerOf( SObject *body );
// What the live lane owns MAY have changed (the shell calls it on
// SApplication::liveLanesMayHaveChanged): every
// program re-asks the hook for each waiting layer and bounces the ones no
// longer blocked. A superset signal is fine: the re-check is the decision.
void liveOwnersMayHaveChanged();

}  // namespace ssampler

#endif
