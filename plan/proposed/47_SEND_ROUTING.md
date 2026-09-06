# Proposal 47 — Send routing: the tap, the send bus, and the cycle refusal

> **Status: PROPOSED.** Nothing here is executed. It closes proposal 45's one
> deliberate open end: **a send lane exists, is named, carries a chain and a
> fader, sums into the master and round-trips — and nothing can feed it**
> (45 D10, AC7.3). This proposal builds the thing that feeds it.
>
> 45 D10 sized this at "at least the size of this proposal" and listed three
> pieces: the tap itself, feedback prevention for A → B → A, and PDC across a
> send. **PDC stays out of scope** — plugin delay compensation is unimplemented
> project-wide (37 P9) and a send is not the place to introduce it. The other
> two are here.

## Reading list, before any milestone

1. `plan/proposed/45_SYSTEM_LANES.md` **D10** (what was deliberately not built)
   and **T3** (`reconnectTracksToMixer` clobbers anything wired beside it) —
   T3 is the trap this proposal is most likely to re-pay.
2. `smaragd/main/objects/mixer/src/sstdmixer.cpp`, `reconnectTracksToMixer()`
   and `adoptSendLane()` — the pass that owns ALL mixer wiring today.
3. `smaragd/main/objects/track/src/strack.cpp`, `wireAsMasterLane()` — the
   shape D1 below reuses, and the `getRootComponent()` override beside it.
4. `smaragd/tw303a/mix/include/tw/mix/twmixer.h` — `setNInputs` /
   `setInputLevel` / `inputLevel`, i.e. the send bus already exists as a type.
5. `smaragd/tw303a/schedule/src/capture_revalidator.cc`, `expandNode_` — why a
   cycle hangs rather than crashes (D6).
6. `smaragd/main/objects/mixer/include/app/objects/mixer/ssolorules.h` — the
   audibility rule a send tap must ask rather than re-spell (D5).

---

## What exists after 45 M7

```
track:   twTrackMix -> twPluginChain -> twGainStage -> twRewire --.
                                                                  |
send lane (STrack, systemRole()==Send):                           |
         twTrackMix -> twPluginChain -> twGainStage -> twRewire --+--> twMixer (master sum)
              ^                                                   |        |
              |  NOTHING REACHES HERE                             |        v
              +-- no clips, no instrument, no input          ... -'   master lane chain -> mixer rewire
```

The send lane's `twTrackMix` is clip-driven and it has no clips, so it freezes
silence; its chain and fader process that silence; its rewire contributes it to
the master at unity. Every part of the path is real and wired. **The only thing
missing is a source.**

---

## Design

### D1. A send lane SUMS a `twMixer` of its own — the master lane's shape, reused

The master lane already demonstrates a lane whose input is a sum rather than
clips: `wireAsMasterLane( sum, rewire )` takes the mixer's `twMixer` straight
into the lane's plugin chain and leaves the lane's own `twTrackMix` inert
(45 AC2.8 / T5). A send lane wants exactly that, one level down:

```
twMixer (the SEND BUS) -> send lane's twPluginChain -> twGainStage -> twRewire -> master sum
```

so `STrack::wireAsSendLane( sum )` is `wireAsMasterLane` minus the rewire
argument — a send lane keeps its OWN `cpRewire_`, because unlike the master it
is an ordinary contributor to the master sum rather than the sum's output.

**`getRootComponent()` therefore needs NO new override.** The master's override
exists because the master lane's real output is the mixer's rewire; a send
lane's real output is its own. Getting this backwards would point every meter,
preview and live-plan channel map at the wrong component — the failure 45 T5
records, which reads as a broken meter rather than as a wiring bug.

**The send lane's `twTrackMix` must be DISCONNECTED, not merely empty.** 45 T5
measured what happens when an inert component is left connected: it carried the
identical signal for two milestones and made a deletion-shaped gate pass.

### D2. The tap lives on the SOURCE object and names its destination

```xml
<sends>
  <send dest='Reverb' level='-6.0' pre='false' enabled='true'/>
</sends>
```

A `QList<SSendTap>` on **`SObject`**, not on `STrack`. Same argument as
`contentKind()`, `resolveEventClip()` and the automation lane vector: the
serializer, the verbs and the testkit must reach a tap without knowing which
object slice owns it. WHICH objects may legally carry one is the verbs'
business (D6).

**Serialized inline, and written only when the list is non-empty** — the rule
that keeps every existing `.qxp` and both committed goldens byte-unchanged, and
that 45, 41 and 37 P5 all already follow.

**The destination is a NAME, never the `-2-k` sentinel.** 45 already restricts
`remove-send-lane` to the LAST lane precisely because the sentinel IS the
address and removing from the middle re-points every path naming a later lane.
A stored index would have the same defect with none of the protection, and the
name is already the user-facing address (`$send:Reverb`). `midiOutPort` is the
precedent: a portable NAME in the file, the machine-local id looked up.

### D3. PRE is post-FX / pre-fader; POST is post-fader. Pre-FX is NOT offered

| Mode | Tap point | Why |
|---|---|---|
| post (default) | `cpGainStage_->linkOutput( 0 )` | the fader scales the send with the dry signal — what "post-fader" means everywhere |
| pre | `cpDspChain_->linkOutput( 0 )` | after the inserts, before the fader — the standard "pre-fader send" of every reference DAW |

A **pre-FX** tap would need a third tap point (`twTrackMix`'s own output) and
no reference DAW defaults to it. Named here so the absence is a decision rather
than an oversight.

### D4. ALL send-bus wiring happens inside the existing rewire pass

45 T3, re-paid. `reconnectTracksToMixer()` sets the master sum's input count
from the track count and rewires **every** input on every audibility, solo,
mute and arm change. A send bus wired from `adoptSendLane`, from a verb, or at
load is clobbered by the next unrelated button press.

So the pass grows a second half — `rewireSendBuses()`, called from
`reconnectTracksToMixer()` and from nowhere else — which for each send lane
sets its bus's input count from the tap count and fills every input. Idempotent
under repetition is the property, and "survives a rewire" is how it is gated
(45 AC7.4's shape, which is the case that actually bit).

### D5. A tap contributes NOTHING when its source is INAUDIBLE, and the rule is asked, never re-spelled

`ssolo::isLaneAudible( root, source, solo )`, resolved once per pass, exactly as
the master sum resolves it. `main/timeline/CONTRACT.md` inv. 10 records that two
meter call sites' LOCAL COPIES of the direct-children-only rule are how the
meter and the ear came to disagree about a nested lane; a third copy here would
make the send disagree with both.

**Mute kills a PRE-fader send too**, which is not derivable from the tap point
and is therefore a decision: Cubase, Logic and REAPER all silence sends on mute,
and "muted but still feeding the reverb" is a bug report waiting to happen.

`isLiveOwnedLane()` is a SEPARATE term and is NOT folded in — see D9.

### D6. A cycle is REFUSED at the verb and BROKEN at the wiring pass — and the reason is NOT that it hangs

**CORRECTED 2026-09-06 by M3's measurement. The first version of this section
was wrong, and it is left described rather than quietly rewritten.**

It read: `CaptureRevalidator::expandNode_` dedups nodes by
`(component, pageStart)` and guards recursion at depth 32, so a cycle produces
two nodes each holding the other as an unsatisfied dependency, `pendingDeps`
never reaches zero, the `GraphDemand` never completes, and a render sits until
the watchdog kills it.

**It does not.** Measured against `tests/send_cycle.qxp` before any cycle break
existed: a cyclic project rendered in **about one second**, completed normally,
and was **byte-identical across `SMARAGD_REVAL_WORKERS` 1 / 4 / 8 over six
runs**. `FreezeContext::isComponentInStack` breaks the recursion at render
time and the scheduler never reaches the state described. The reading of
`expandNode_` was accurate; the conclusion drawn from it was not, and it was
asserted as fact in D6, in M0's commit message and in a case header before
anything measured it.

**So why refuse a cycle?** Because the audio a broken cycle produces is
whatever the render-time break happens to yield — a number defined by accident
rather than a mix anyone asked for. That is a weaker reason than the original
one and it is the true one.

**And the verb alone is NOT enough**, which is the substance of M3. `add-send`
refuses a cycle, but **the loader does not go through the verb**: `<sends>` is
read by `SObject::readSends()`, so a hand-edited or foreign `.qxp` carries
whatever it likes. So there are two layers, and they answer different
questions:

| Layer | Job |
|---|---|
| `add-send` / `set-send` (M0) | REFUSE, with a message naming both ends. This is the one a user meets. |
| `rewireSendBuses()` (M3) | BREAK whatever reached the model anyway, deterministically, and ANNOUNCE it. This is what guarantees the graph handed to the scheduler is acyclic however the taps got there. |

The break accepts an edge only when it does not close a cycle **among the
edges already accepted**, walking lanes in index order (= file order). That
drops exactly ONE edge of a cycle; asking the FULL tap graph instead finds both
edges cyclic and drops BOTH, silencing a lane that has a good reason to sound.

The other refusals are unchanged and are the verb's alone: self, a
non-send destination, an unresolved name (fails closed), and the MASTER as a
source — the master's output is the sum that already contains every send lane,
so that edge is the mixer's own wiring and is not in the tap graph a walk can
see.

### D7. The level is the send bus's own per-input level. No new DSP

`twMixer::setInputLevel( channel, dB )` already exists and is already how the
master sum would express a non-unity input. A send level is that number. There
is no new gain component, no new ramp, and nothing new on the render path.

**And a non-unity send level does NOT disturb live monitoring**, which is worth
stating because 45 found the opposite for the MASTER sum: `checkMasterShape`
refuses the LINEAR split on any non-unity input of the **master** mixer, and a
send bus is a different `twMixer` that it never inspects. The master's own
inputs — the tracks and the send lanes — stay at unity exactly as they are now.

### D8. PDC across a send is OUT OF SCOPE, and every surface that shows a latency says so

Unchanged from 37 P9 and 45. A latency-reporting insert on a send lane makes the
wet path late by exactly its reported amount. This proposal adds no delay line
and does not pretend to.

### D9. A LIVE LANE DOES NOT FEED A SEND — DECIDED, requester call 2026-09-06

**An armed, monitored track's send is SILENT, and that is the behaviour, not a
defect to be worked around later.**

It falls out of the existing design rather than being invented here. The live
plan excludes a live-owned track from the frozen sum — the mixer nulls its
input plug — and the pump renders it block-wise straight into the ring. Its
send tap, however, reads the track's `twGainStage`, whose pages are exactly the
ones that are no longer being produced. So while a performer monitors through
their chain, the send contributes nothing.

Three readings were put up; the requester chose the first:

| Option | Verdict |
|---|---|
| **(a) A live lane does not feed a send. ACCEPT and ANNOUNCE.** | **CHOSEN.** The monitored signal is dry. |
| (b) Put send lanes in the live CLOSURE | Rejected. The pump would have to render the tap, the send bus, the send lane's chain and its fader per block — the pump growing a second component graph, which proposal 21 D1 spent a milestone arguing against. |
| (c) Feed the ring from the tap | Rejected. The RT sums a ring entry onto the frozen ROOT page; a send contribution would have to be summed at the send BUS instead, which the RT does not reach. |

**What "announce" means, and it is the whole cost of the decision.** A silent
send is indistinguishable from a broken one, so the rule is stated where
somebody hits it rather than only here:

- a live-owned source contributes **nothing** to a send bus, decided by the
  same `isLiveOwnedLane()` term the master sum already uses — and it is a
  SECOND term beside `ssolo::isLaneAudible`, never folded into it (proposal 21
  L1b's rule: a live-owned track is still audible in every other sense);
- the wiring pass logs it **once per arm**, not per pass and never per page;
- `main/objects/mixer/CONTRACT.md` and `CLAUDE.md` carry it as a stated
  limitation beside the monitor-priming lag, which is the other "ours, not the
  driver's" entry.

**M4 is therefore no longer a decision milestone.** It becomes the gate for this
behaviour: a case that arms a track with a send and asserts the send bus is
silent, so the behaviour cannot drift into a half-working state that neither
matches this document nor a reference DAW.

---

## Traps

| # | Trap |
|---|---|
| T1 | **`reconnectTracksToMixer` clobbers anything wired beside it** — 45 T3, re-paid in full. All send-bus wiring inside the pass (D4). |
| T2 | **An inert component left CONNECTED carries the identical signal** (45 T5, measured over two milestones). The send lane's `twTrackMix` must be disconnected. |
| T3 | **`bumpRenderChainEpoch()` is NOT enough for an edit the fader sees** — 45 measured a muted master render coming back BYTE-IDENTICAL to the unmuted one. A tap edit needs `invalidateRenderPath()`. |
| T4 | **A tap edit reaches the SEND LANE's pages, not the source track's.** The source's own output is unchanged; what changed is a downstream sum. Invalidating the source alone leaves the send lane serving stale pages. |
| T5 | **Node count is no longer flat.** 36 B9 measured `nodesExecuted` flat in channel WIDTH; it says nothing about routing depth. A send adds a node per (send lane, page) plus the bus. Measure, do not assume. |
| T6 | **A destination name that does not resolve** (lane renamed, lane removed, project from another machine) must fail CLOSED and be announced — never silently drop the send. |
| T7 | **Removing a send lane that still has taps** pointing at it. The taps must not become dangling addresses that re-attach if a lane of that name reappears. |
| T8 | **Golden byte-identity**: with no taps, no bus is wired and no component is added, so both committed goldens and every `.qxp` must be byte-unchanged. This is the negative control for the whole proposal. |
| T9 | **A send lane's own tap** — a send feeding another send is the legitimate shape D6's cycle check exists for, not something to refuse outright. Refusing send→send entirely would be simpler and is NOT what is proposed. |

---

## Milestones

**M0 — the tap model.** `SSendTap` + the `QList` on `SObject`, inline
serialization, the four verbs (`add-send`, `remove-send`, `set-send-level`,
`set-send-mode`), every refusal from D6, and `assert-sends`. **No audio.** The
negative control is T8: every golden byte-unchanged, because nothing is wired.

### M0 as executed (2026-09-06)

Shipped as committed. `SSendTap` on `SObject`, inline `<sends>`, the three
verbs, `assert-sends`, and `qxa.send_tap_model`. The negative control holds:
two send lanes and three taps between them render **byte-identically** to the
same project with none (`assert-file-identical`), and both committed goldens
are unchanged.

**Watched failing under six sabotages, and five of the six bite disjointly:**

| Sabotage | What failed |
|---|---|
| the CYCLE refusal deleted | only the cycle `expectReject` (action #34) |
| the SELF refusal deleted | only its `assert-log` (#28) — see the finding below |
| the MASTER-as-source refusal deleted | only its `expectReject` (#29) |
| `serializeSends` writes nothing | only the four post-load assertions (#41-44) |
| `setSendTap` APPENDS instead of replacing | only the five in-flight assertions (#10-18) |
| `STrack::serialize` forgets `serializeSends` | the same four post-load assertions (#41-44) |

The last two sharing a symptom is not a disjointness failure: they are two
independent ways to lose the same data, and the fifth proves the in-flight
assertions are a separate statement from the round-trip ones.

**FINDING — THE SELF REFUSAL IS ALREADY DONE BY THE CYCLE WALK, and this is
proposal 45's own lesson found a second time.** Deleting the explicit
`dest == source` check did NOT let a self-send through: the reachability walk
starts at the destination and returns true on its first iteration when the
destination IS the source, so the verb still refused — with the CYCLE message
instead. What the accident does not do is say the right thing, which is exactly
why the gate asserts the ANNOUNCEMENT (`assert-log`) rather than the refusal.
45's table records the identical shape for `remove-track` / `move-track` /
`reparent-track`, where deleting each explicit check moved zero assertions. The
explicit check stays for the same reason theirs did: it is the one that still
reads correctly when the walk changes.

**NOT gated in M0**, and each is a milestone below rather than an omission: the
audio (M1 — there is no bus), invalidation (M2 — there is nothing to
invalidate), the scheduler HANG the cycle refusal exists to prevent (M3
measures it; M0 only refuses it), the monitored-send dropout (M4/D9), and any
UI (M5 — these verbs are reachable only from a `.qxa` script, exactly as
proposal 41's fragment verbs were before its menu items landed).

**Suite:** 365 registered / 360 run / 5 disabled, reconciled both ways.
`playback_test` failed once in the `-j4` run and is the documented wall-clock
reposition case on a **4-core** box, where `-j4` is full saturation: 3/3 passes
idle, and this branch changes no `tw303a/` file at all.

**M1 — the send bus, audible.** `wireAsSendLane`, `rewireSendBuses()` inside the
pass (D4), pre/post tap points (D3), level as the bus input level (D7),
audibility (D5). Gated by RMS through a render against a closed form.

### M1 + M2 as executed (2026-09-06)

Shipped together, because M1's gate cannot pass without M2: a send that is
wired correctly and never re-freezes is inaudible, which is M2's whole subject.

`STrack::wireAsSendLane`, one `twMixer` per lane owned by `SStdMixer`,
`rewireSendBuses()` called from `reconnectTracksToMixer()` and nowhere else
(D4/T1), and `SSendTap`'s level as the bus's own per-input level (D7).
`qxa.send_bus_audible` measures six states, **every one against a closed form**
rather than a bound fitted to what the code produced:

| State | Closed form | Measured |
|---|---|---|
| dry only (tap disabled) | A = 0.230956 | 0.230956 |
| send 0 dB, post | 2A = 0.461913 | 0.461913 |
| send −6 dB, post | 1.501187A = 0.346686 | 0.346710 |
| fader −6 dB, send 0 dB, **post** | 1.002374·0.501187A = 0.115752×2.0024 → 0.231504 | 0.231505 |
| fader −6 dB, send 0 dB, **pre** | 1.501187·0.501187A… = 0.346686 | 0.346710 |
| source muted | 0 | 0 |
| tap removed (fader −6 dB) | 0.501187A = 0.115752 | 0.115754 |

**THE PRE/POST PAIR IS THE ONLY THING THAT DISCRIMINATES THE TAP POINT**, and
it is why the case pulls the source fader to −6 dB in the middle. At unity gain
pre and post read IDENTICALLY, so a case that never moved the fader would gate
nothing at all — proposal 39 M2 and 41 M5 both paid for that shape.

**FOUR FINDINGS, three of them defects in this milestone's own code.**

1. **`twMixer::setNInputs` REFUSES ZERO by contract** (`if( n<=0 ) return -2`),
   and the refusal was unhandled. A bus that lost its last tap kept the plug it
   already had and went on summing forever: a render read 0.34671 where
   0.115752 was due. "No taps" is now ONE UNWIRED input, never a request for
   none. The refusal is right — a mixer with no inputs has no output to define.

2. **Removing a send lane and UNDOING it SEGFAULTED**, in
   `twComponent::setInput` three frames under `SRestoreSendLaneAction`. A
   component holds an input PLUG into its producer's latch, and dropping the
   bus first left the lane's plugin chain holding a plug into a destroyed
   `twMixer`; the next `setInput()` dereferenced that dead latch to detach it.
   `STrack::unwireSendLane()` now runs while the bus is still alive, and
   `detachSendLane` drops the bus at the SAME index rather than leaving
   `rewireSendBuses()` to trim the tail — the two lists would otherwise stop
   being index-parallel for a removal from the middle, which the verb forbids
   today and which this code no longer leans on. **Caught by `qxa.send_lane_
   remove_undo`, a proposal 45 case, not by anything written here.**

3. **ONLY `invalidateRenderPath()` IS LOAD-BEARING, and that was established by
   ABLATION rather than by reasoning.** The first version also bumped every
   send lane's `bumpRenderChainEpoch()` and every bus's `bumpContentEpoch()`.
   Each was removed separately and the gate still passed, so both were deleted:
   code no sabotage can bite is code with no justification. This is proposal
   45's own rule arrived at independently — *invalidate, never bump*.

4. **A "finding" that was retracted, recorded because the retraction is the
   lesson.** An early measurement appeared to show that a source-track FADER
   edit never reached the send lane, and an `invalidateRenderChainsContaining`
   override on `SStdMixer` was written for it — including making that walk
   `virtual` on `SObject`. The measurement was wrong: the case had written
   `volumeDb=` where `set-track-volume` takes `volume=`, so the fader had never
   moved. With the attribute fixed the override changed nothing and was
   reverted, `virtual` included. **A number measured through a broken harness
   is not a measurement**, and a hot base-class walk very nearly went virtual
   for it.

**Sabotages — six, each biting one assertion and no others:**

| Sabotage | Bites |
|---|---|
| pre and post tap the same point | only `send_pre` (#19) |
| the send level ignored (all taps unity) | only `send_minus6` (#12) |
| audibility ignored (a muted source still feeds) | only `send_muted` (#22) |
| the `enabled` flag ignored | only `send_dry` (#6) |
| `rewireSendBuses()` never called from the pass (T1/D4) | every audible assertion (#9, #12, #16, #19) |
| `setNInputs( 0 )` again | only `send_gone` (#26) |

`qxa.send_tap_model`'s T8 was RESTATED rather than deleted: "a tap changes
nothing" was true only while no bus existed. It now reads **a send lane whose
taps are all DISABLED is byte-identical to no send at all** — a narrower claim,
a harder one (bytes, not an RMS bound), and the one that actually protects both
committed goldens, which are unchanged.

**Suite:** 366 registered / 361 run / 5 disabled, reconciled both ways, all
green. One unreproduced flake to name rather than bury:
`qxa.master_closure_linear_ring` failed once in an earlier `-j4` run and passed
4/4 alone and in the clean full run afterwards. It is `RUN_SERIAL` and is one
of the live-monitoring wall-clock cases on a **4-core** box; this branch adds
nothing to any path it exercises (`rewireSendBuses` returns immediately when a
project has no send lanes, and that case has none). Not reproduced, not
explained.

### M3 as executed (2026-09-06) — and D6 was WRONG

**The milestone's first act falsified its own design section.** D6 predicted a
cyclic send graph would hang the render; `tests/send_cycle.qxp` (a project
saved by this app, then hand-edited to add the closing edge — the shape a
user's file would actually have) **rendered in about one second, completed
normally, and was byte-identical across `SMARAGD_REVAL_WORKERS` 1 / 4 / 8 over
six runs.** `FreezeContext::isComponentInStack` breaks the recursion at render
time. The reading of `expandNode_` behind D6 was accurate; the conclusion drawn
from it was not, and it had been asserted as fact in D6, in M0's commit message
and in a case header before anything measured it. All three are corrected in
place rather than quietly rewritten.

**What M3 therefore built is the half the verb cannot reach.** `add-send`
refuses a cycle, but the LOADER does not go through the verb — `<sends>` is
read by `SObject::readSends()`. `rewireSendBuses()` now breaks any cycle that
reached the model anyway: an edge is accepted only when it does not close a
cycle among the edges ALREADY accepted, walking lanes in index (= file) order.
That drops exactly ONE edge; asking the full tap graph instead finds both edges
cyclic and drops BOTH, silencing a lane with a good reason to sound.

The gate's level is a closed form: with Alpha → Beta dropped and Beta → Alpha
kept, Beta is silent, Alpha = A, and the master is **2A = 0.461913** — where
dropping the other edge gives 3A (clipped ≈ 0.6666), dropping both gives A, and
dropping neither gives the undefined cyclic value.

**Sabotages:** no break at all → the announcement AND the level (#2, #8); break
over the full graph so both edges go → only the "exactly one edge" assertion
(#3); break silently → only the announcement (#2), the level still 2A.

**The `assert-log` window trap was re-paid here before it was fixed.** The
break happens while the project is being ADOPTED, not during the render, so
assertions placed after the `<render>` — or after the `assert-sends` calls,
which advance the window just as well — read an empty window and report
"OK — 0 records". Proposal 45 recorded this exact trap; it still caught this
case out. Also learned: `maxCount` alone still asserts a floor of one, so a
negative assertion needs `minCount="0" maxCount="0"`.

### M4 as executed (2026-09-06) — D9 pinned

**A wiring gate, and it had to be.** The decision is that a live lane does not
feed a send; that cannot be measured through a render, because `startRender()`
SUSPENDS every live lane (21 L1b) — by the time there is audio, the condition
under test is gone. So `assert-send-inputs` reads the send bus's own inputs off
the live `twMixer`, the send-side twin of `assert-master-inputs` and for a
sharper version of the same reason.

The case asserts the tap wired at −6 dB; then armed with `monitor == on`, the
input REMAINS (the tap is untouched in the model) and is **UNWIRED**; then on
hand-back it is wired again at its original level and audible at
1.501187A.

**A real input backend is load-bearing.** Without `SMARAGD_AUDIO_INPUT_BACKEND`
an armed track never becomes a live SOURCE, and the first version of this case
PASSED over a condition that never happened. It now takes the paced `file:`
input the proposal-21 monitor cases use, and is `RUN_SERIAL` with them.

**Sabotages:** drop the D9 term → only the "wired=0 while live" assertion (#10);
make the exclusion one-way (never re-wire) → only the hand-back assertions
(#14, #15, #17), which is what stops "does not feed" degrading into "never
feeds again".

**NOT gated, and it is the decision's own cost:** what the user HEARS while
monitoring. That dropout is what option (a) accepts; measuring it needs a
wall-clock RUN_SERIAL case of the proposal-21 monitor shape. What is gated is
the mechanism the dropout follows from.

**Suite:** 368 registered / 363 run / 5 disabled, reconciled both ways, all
green. Goldens byte-identical.

### M5 as executed (2026-09-06)

A **Sends** section in the Track Detail dock: one row per send lane, with an
enable box, the lane's name, a level in dB and a pre/post choice, each
committing through M0's ordinary verbs. It is the first surface that makes
those verbs reachable by hand — until now they existed only in a `.qxa`
script, exactly as proposal 41's fragment verbs did before its menu items
landed.

**A ROW EXISTS PER SEND LANE, NOT PER TAP.** Ticking one CREATES the tap, so a
user does not first have to discover a separate "add" gesture, and a lane with
no tap still shows — "there is a Reverb bus and this track does not feed it" is
visible rather than inferred from an absence. **Unticking DISABLES; it does not
remove**, which is the whole reason `SSendTap::enabled` exists: a level and a
pre/post choice must survive being switched off, or a user toggling a send off
and on finds it back at 0 dB post.

A lane's own strip offers every OTHER lane and no row for itself. The verb
refuses a self-send anyway; a control that exists only to be rejected is worse
than one that is not offered.

**THE WIRING IS GATED, AND THAT IS NEW FOR THIS REPO.** `send-strip-set` moves
the REAL control and lets Qt deliver the signal, so **a missing `connect()`
fails**. Every context menu this project has shipped — proposal 41's Pack /
Unpack items, 45's Show-system-lanes item, the metronome button's right-click
menu — is hand-verified only, because there is no testkit verb for a context
menu. A WIDGET is different: it can be built off screen and driven, which
`assert-track-head` and `assert-track-detail-layout` already do for geometry.
Sabotage S1 removes the checkbox's `connect()` and five assertions fail.

**Sabotages, four, each biting its own assertions:** the checkbox connected to
nothing (#9, #10, #14, #16, #18); unticking REMOVING instead of disabling
(#22, #24 — the two that assert the level and mode survive); a self row offered
(#25, #26); the level control editing the model but never reaching the bus
(#16, #18, **#20 the audio**, #22, #24).

The case also ends on `assert-track-detail-layout` at 260 px and 600 px with
`maxCrushed=0 maxOverlap=0`: the rows are fixed-height and mount at stretch 0,
so a short dock scrolls rather than laying the FX chain and the sends on top of
each other — the defect `fix/detail-pane-layout` fixed and this section could
have reintroduced.

**A defect this found in the new seam:** `describeSendStrip` first resolved its
track with `splacements::laneAt`, which cannot resolve a system-lane sentinel
at all — so `$send:Reverb` returned nothing and the assertions read an EMPTY
description rather than failing on the lane. Through `laneBySpec` now, which is
what every other system-lane-aware verb uses.

**NOT gated, hand-verified only:** that the section appears where a user
expects it and what it looks like. `screenshot` grabs a root window that is
blank under `QT_QPA_PLATFORM=offscreen`, so pixels stay out of reach — the
standing gap this repo works around by building one widget and measuring
geometry. Also not gated: the strip's behaviour when a send lane is added or
removed while the dock is open (it rebuilds on track switch, not on a lane
change), and the double-click-to-0 dB reset on the level spin box (wired
through `sdefaultreset`, no case).

**Suite:** 369 registered / 364 run / 5 disabled, reconciled both ways, all
green. Goldens byte-identical.

**M6 — contracts and docs.** `main/objects/mixer/CONTRACT.md`,
`main/objects/track/CONTRACT.md`, `docs/ACTIONS.md`, `CLAUDE.md`, and this
file's "as executed" sections.

---

## What this proposal will NOT gate

- **PDC across a send** (D8) — not implemented.
- **Real device latency** under a send-heavy project. Every measurement here is
  against the capture backend, as everywhere else in this repo.
- **The monitored-send dropout** until M4 pins it (D9). M1-M3 implement option
  (a) and say so; nothing asserts it until M4.
- **Pixels** anywhere — a send strip is `SSMVMixerControl` unchanged until M5,
  and M5 will assert a geometry relation, never a palette.
- **A send lane inside a nested arrangement asset**, and **more than one
  arrangement's send lanes at once** — 45 left both unmeasured and this
  proposal does not widen them.
- **Sends from a MIDI or event lane** — a tap is an AUDIO tap; a MIDI track
  whose notes reach no instrument has no audio to send.
