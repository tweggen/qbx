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

#include <QList>

class QString;
class SApplication;
class SProject;
class SPluginSlot;
class SSamplerProgram;
class STrack;

namespace ssamplerwiring {

void install();

// QBX-274: A BOUNCE OVER A LIVE-OWNED TRACK WAITS FOR THE DISARM. Installs
// ssampler's live-owner hook (an asset body -> the spec of a track the live
// lane owns that the body reaches through child and content links, the
// monitor's liveOwnedTracks(): the closure, plus what a running render's
// suspension holds) and re-checks every waiting layer on
// SApplication::liveLanesMayHaveChanged (a superset: the re-check decides).
// Called from the SApplication constructor once the live monitor exists.
void installLiveOwnership( SApplication *app );

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

// RECORDING INTO THE INSTRUMENT (proposal 56 Q6, Q-1 answered 2026-11-01).
// The first NassauDigital slot on `track` whose program has a RECORD INPUT,
// with its slot index; null when there is none. An armed track holding one
// records INTO that instance from the instance's own input (SAudioRecorder),
// whatever the track's own input is.
SSamplerProgram *recordTargetOf( STrack *track, int *slotIndex );

// Every NassauDigital program on `track`'s chain (the key taps' walk).
QList<SSamplerProgram *> programsOf( STrack *track );

// THE EDITOR'S HALF (proposal 56 Q7, QBX-279). samplerui sees objects/track
// and objects/sampler, but the uid test and the program lookup are this
// file's, and a second copy of either is how two readers come to disagree.
//
// Does `slot` host NassauDigital (format tw, uid tw.native.nassaudigital)?
bool slotHostsNassauDigital( const SPluginSlot *slot );
// The uid the pluginui editor registry is keyed by.
QString nassauDigitalUid();
// `slot`'s program object, or null when it has none yet (or is not one).
SSamplerProgram *programOf( const SPluginSlot *slot );
// THE VIEW A NEW INSTANCE STARTS IN (Q7.1; DESIGN §7): the per-user option
// SOpt::SamplerDefaultView, read here and nowhere else. The resolver gives a
// program object it CREATES this view (a program read from a file keeps the
// one it was saved with), and the editor shows it for an instance that has no
// program object yet.
int newInstanceView();   // an SSamplerProgram::View, as int

}  // namespace ssamplerwiring

#endif
