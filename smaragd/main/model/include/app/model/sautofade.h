#ifndef _SAUTOFADE_H_
#define _SAUTOFADE_H_

#include "tw/core/twfraction.h"
#include "tw/events/twfade.h"
#include "tw/core/twtypes.h"

#include <QHash>

class SClipWindow;
class SLink;
class SObject;
class SProject;

/**
 * THE PLAYBACK AUTO-FADE (plan 53 Q9/P5, decisions D3a and D3b).
 *
 * A hard clip edge is a step in the waveform, and a step is a click. Before
 * this, the only way to not hear one was to draw a fade by hand at every edge
 * — which nobody does, which is why every DAW grew this feature. It is a
 * PROJECT property (`SProjectProps::AutoFadeMs`), applied in the mix at every
 * clip edge and at every comp boundary that has no crossfade of its own, and
 * never stored per clip: a clip stores only its VETO (`twClipFade::autoIn` /
 * `autoOut`) and any explicit fade it has, which wins at its own edge.
 *
 * WHY THIS LIVES IN `model` AND NOT BESIDE EITHER CONSUMER. There are two,
 * and they are in slices that may not see each other (tools/check_layering.py):
 * `objects/track` applies it at CLIP edges, in
 * `STrack::refreshClipGainCurves`, and `objects/cut` applies it at COMP
 * boundaries, in `STakeStack::ensureCompColumn`. `objects/track` has no edge
 * to `objects/cut` and never will — that is the rule that put `clipFade()` and
 * `contentKind()` on `SObject` as base-class seams in the first place. So the
 * ms-to-frames conversion, the per-edge policy and the seamless test are here,
 * once, where both can reach them and neither owns them.
 *
 * AND WHY IT NEEDS NOTHING FROM `objects/cut` TO DO IT. The seamless test
 * reads five things — the content, the stretch, the loop state, the window's
 * length and its source anchor — and `SClipWindow` publishes all five, exactly
 * because they are the arithmetic every windowed verb needs and nothing
 * audio-specific. Pitch, formants and the warp anchors are not here, and are
 * not needed; see `isSeamless`.
 */
namespace sautofade {

/**
 * The project's auto-fade in MILLISECONDS, or 0 when the project has none.
 *
 * ZERO IS THE READ DEFAULT AND THAT IS THE WHOLE SAFETY PROPERTY (D3a (a)):
 * no project file written before plan 53 carries the key, so every one of them
 * answers 0 here and renders exactly as it always did. The 3 ms the feature
 * promises arrives through `seedInto` at creation, never through this read.
 */
double msOf( const SProject *project );

/**
 * The same, in FRAMES at the project's sample rate; 0 when off.
 *
 * Rounded to the nearest frame, once, here — so the clip edges and the comp
 * boundaries of one project can never disagree about how long 3 ms is, which
 * is the kind of one-frame difference that makes a gate flake rather than
 * fail.
 */
length_t framesOf( const SProject *project );

/**
 * Write the 3 ms seed onto a BRAND-NEW project (D3a (a)).
 *
 * CALL IT ONLY FOR A FRESH PROJECT, NEVER AFTER A LOAD — the same rule, for
 * the same reason, as `srecmodeseed::seedInto` (see
 * app/shell/srecordmodeseed.h, which states it at length): `fileOpen`'s
 * `new SProject()` is the empty shell the loader fills, and property loading
 * MERGES over whatever is already there, so a seed written on that path would
 * survive for every file that LACKS the key — i.e. for every project saved
 * before plan 53.
 *
 * `auto_fade_seed.qxa` gates what a .qxa CAN gate about it: that a fresh
 * project through the real new-project path does fade, and that a file which
 * STATES a value wins over the seed. It cannot gate the fileOpen hazard
 * directly, because `SLoadProjectAction` loads into the CURRENT project and
 * clears the arrangements but not the properties — the same limitation
 * `record_mode_ini_seed.qxa` records for the record mode, and for the same
 * reason.
 *
 * It lives here rather than beside the record-mode seed because it reads no
 * INI and therefore needs nothing from the app layers; the shell calls it from
 * the same two places.
 */
void seedInto( SProject *project );

/**
 * IS THE JOIN BETWEEN THESE TWO ABUTTING WINDOWS SEAMLESS? (D3b (a).)
 *
 * The caller has already established that they ABUT on one lane — that
 * `left`'s placement ends exactly where `right`'s begins. What is left is
 * whether the MATERIAL is continuous across that join, and if it is, the
 * auto-fade must not touch it: a 3 ms dip in the middle of a sustained note is
 * an audible notch, and every `split-clip`, every mode-B seam and every
 * `replace` R2 split would make one. That is the notch D3b forbids, and
 * `auto_fade_seamless_exempt.qxa` is the gate that holds it.
 *
 * Continuous means all four of:
 *
 *   - THE SAME CONTENT OBJECT. Not an equal file path: two windows over two
 *     different `SSample`s of one file have independent readers, and nothing
 *     here can promise their sample positions line up.
 *   - NEITHER IS LOOPING. A looping window's last repetition ends at a loop
 *     seam, which is a discontinuity in the material whatever follows it; and
 *     `timelineToSourceExact` maps within ONE repetition, so the answer below
 *     would not even be the right question.
 *   - THE SAME TIME SCALING, exactly (`stretchOrRate`, a Fraction and never a
 *     double). Material that continues in POSITION but is read at a different
 *     rate steps in pitch and in phase at the join.
 *   - THE SOURCE POSITIONS MEET EXACTLY: `left`'s source position at its own
 *     end IS `right`'s source anchor. Exact, in the content's own units, with
 *     no rounding anywhere — which is what `timelineToSourceExact` exists for
 *     and why the anchor is stored content-authoritative (proposal 18 Phase 3).
 *
 * WHAT IS DELIBERATELY NOT TESTED: the warp anchors. They are audio-specific
 * and `SClipWindow` does not publish them (its contract lists them, with pitch
 * and the grain params, as absent on purpose), so this layer cannot read them.
 * It costs nothing that matters: `timelineToSourceExact` is map-aware on both
 * sides, so the POSITION test above is exact under any warp, and every edit
 * that produces a seam — `split-clip` above all — copies the anchor list
 * wholesale, so the two agree whenever this question is interesting.
 */
bool isSeamless( SClipWindow &left, SClipWindow &right );

/** Which of one placement's two edges are seamless. */
struct Seams {
    bool in  = false;
    bool out = false;
};

/**
 * THE SEAMLESS ANSWER FOR EVERY CLIP ON ONE LANE, computed once.
 *
 * Only `isSeamless` can say whether two windows are continuous; only a LANE
 * knows which clip abuts which. This is where the second half lives, and it
 * lives here rather than in `objects/track` so that THE MIX AND THE EDIT
 * SURFACE READ THE SAME ANSWER. The Clip Properties panel shows the user
 * which of a clip's edges the auto-fade skips, and a second implementation of
 * that question is a second thing to be wrong — it would be wrong silently,
 * too, because a display that disagreed with the mix looks exactly like a
 * display that is right.
 *
 * It returns a map rather than answering per clip because the answer needs the
 * lane's clips SORTED BY START TIME: `childLinks()` is the container's own
 * order and is not sorted by time (SObject::windowStep's contract says so in
 * as many words, and plan 53 §P5's claim that
 * `STrack::refreshClipGainCurves` "already walks the lane's links in order"
 * is simply wrong). Asking per clip would re-sort per clip.
 *
 * TWO THINGS ARE CHECKED HERE, both of them about the PLACEMENT rather than
 * the window, which is why they are not in `isSeamless`:
 *
 *   - THE CLIPS MUST ABUT EXACTLY. One frame of gap is a gap, and the edges
 *     either side of it are ordinary outer edges that want their fade. One
 *     frame of OVERLAP is not a join either: the two are sounding together
 *     there, and whatever that is, it is not a continuation.
 *   - THE WINDOW MUST SPAN THE WHOLE PLACEMENT. `SClipWindow::parametersOf`
 *     answers with the ACTIVE TAKE for a directly placed column, and since
 *     plan 53 A0a a take may cover LESS than its column; its source position
 *     at its own end is then not the source position at the placement's end,
 *     so the exact test would be answering a different question than the one
 *     asked. Refusing here means such an edge gets its auto-fade, which is the
 *     safe direction: a fade that was not needed is 3 ms of ramp over material
 *     that is continuous anyway, while a missing fade is a click.
 *
 * Clips the lane holds that are not clips (a nested lane) and placements with
 * no start time are absent from the result, and a `value()` on a link that is
 * not in it answers "neither edge is seamless" — the right default.
 */
QHash<SLink *, Seams> laneSeams( SObject &lane );

/**
 * THE FADE THE MIX SHOULD APPLY: a clip's stored fade with the project's
 * auto-fade filled in at the edges that have none and do not refuse it.
 *
 * PER EDGE, and not per clip. Plan 53 §P5 says "every clip edge whose
 * `twClipFade` is `none()`", which reads one way as a whole-clip test; it is
 * applied per edge here, because the other reading says that declaring a
 * fade-IN silently turns the auto-fade off at the clip's OUT edge — a user who
 * drew one fade did not ask for a click at the other end. Per edge is a
 * superset of what the plan asked for and `auto_fade_override.qxa` pins both
 * halves of it.
 *
 * An edge is left alone when any of three things is true: it has an explicit
 * fade (the user's own length wins), its `auto` flag is false (the user said
 * hard edge), or the caller found it SEAMLESS. Otherwise it gets
 * `min( autoFrames, clipLen / 2 )` — halved so a clip shorter than two
 * auto-fades still reaches unity in the middle instead of being a triangle.
 *
 * Pure, so it is the same function for a clip and for anything later that
 * needs the policy: it reads nothing and changes nothing.
 */
twClipFade effectiveFade( const twClipFade &stored, length_t clipLen,
                          length_t autoFrames, bool seamlessAtStart,
                          bool seamlessAtEnd );

/**
 * Re-apply the auto-fade everywhere and invalidate what it changed.
 *
 * The auto-fade is a project property, so the only signal it raises is
 * `SProject::propertyChanged` — no clip changed, no arrangement changed, and
 * nothing in the ordinary edit paths will notice. This is the function that
 * turns that signal into audible effect, exactly as the metronome property's
 * one connection in `SApplication` turns its own into a plan rebuild.
 *
 * It walks the arrangement in MODEL terms only — every lane is an `SObject`
 * and both calls it makes are `SObject`'s — so it needs no track type and
 * stays here with the rest of the policy.
 */
void reapplyAll( SProject *project );

} // namespace sautofade

#endif // _SAUTOFADE_H_
