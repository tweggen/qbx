#ifndef _SSAMPLERWIRING_H_
#define _SSAMPLERWIRING_H_

// ssamplerwiring.h — the shell's half of NassauDigital's model (proposal 56
// Q1, the Q-6 answer of 2026-10-27: objects/sampler keeps objects/midi's rank
// and has NO edge to objects/track; "the shell installs the slot's instance
// initializer as a hook").
//
// The shell is the composition root and the one module that sees both
// objects/track (SPluginSlot) and objects/sampler (SSamplerProgram). install()
// connects them with two hooks and nothing else:
//
//   * ssampler::setResolver — a sampler verb's trackPath + slotIndex becomes
//     the slot's SSamplerProgram: laneBySpec (honouring the action's
//     pathRoot), the track's chain, the slot, refused unless the slot hosts
//     tw.native.nassaudigital, its program child -- created on first use with
//     SPluginSlot::adoptChild when the verb asks for it.
//
//   * SPluginSlot::setChildAdoptedHook — every SSamplerProgram that arrives
//     under a slot (adopted at run time, or read back from a project file) is
//     BOUND: its installer sets the slot's instance initializer to the
//     program's (so a rebuilt instance is born current, invariant 4) and
//     applies it to the instances that exist (applyToInstances: under the
//     processor's mutex, then the path above the slot is staled, §4.3).
//
// Called once from SApplication::initPluginRegistry(), beside
// registerNassauDigital(); a test that builds slots without an SApplication
// calls it itself. Idempotent.

class QString;
class SProject;

namespace ssamplerwiring {

void install();

// THE OFFLINE RENDER'S SETTLE (proposal 56 B8, §4.6; DESIGN §6.1 rule 3).
// Called by SApplication::startRender() on the MAIN thread, before the run
// barrier and before the render worker exists. For every NassauDigital
// instrument the barrier's walk finds, it waits SYNCHRONOUSLY for each
// outstanding generation job and installs the results — the render verb
// sleeps on the main thread, so a wait that needed the event loop would
// deadlock — and logs, per track that had any,
//
//     run barrier: waited <ms> ms for <n> sampler generation(s) on track <spec>
//
// One bound covers the whole render: SMARAGD_SAMPLER_SETTLE_MS (default
// 30000), or the test override below. False with `why` when a job did not
// finish inside it: the render must not start (invariant 7, QBX-145's
// discipline). Playback does NOT call this: a playback run plays whatever
// generation is installed.
bool settleBeforeRender( SProject *project, QString *why );
int  settleBoundMs();
// TEST SEAM (set-sampler-render-gate settleBoundMs=): < 0 restores the default.
void setSettleBoundOverrideMs( int ms );

}  // namespace ssamplerwiring

#endif
