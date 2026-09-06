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

### D6. A cycle is REFUSED AT THE VERB, and the reason is that the scheduler hangs rather than fails

Read, not measured (M3 measures it): `CaptureRevalidator::expandNode_` dedups
nodes by `(component, pageStart)` and guards recursion at depth 32, so a cycle
does **not** blow the stack. It produces two nodes each holding the other as an
unsatisfied dependency — `pendingDeps` never reaches 0 for either, neither is
ever enqueued, and the `GraphDemand` never completes. A render then sits until
`SMARAGD_RENDER_TIMEOUT_MS` kills it; playback's readahead never reaches its
priming frontier and the transport never starts.

`FreezeContext::isComponentInStack` breaks cycles at RENDER, which is why this
is a hang rather than an infinite recursion — and why it cannot be relied on to
save the scheduler, which never gets as far as rendering.

So the refusal is structural and lives where it can be announced:

- **self** — a lane may not send to itself;
- **non-send destination** — the destination must be a lane with
  `systemRole() == Send`;
- **unresolved name** — fails CLOSED and is announced (D6's own rule from 45:
  a bound is ANNOUNCED, never silent);
- **cycle** — a reachability walk over the tap graph from the proposed
  destination back to the source. The graph is at most (tracks × sends) edges
  and is walked on an edit, never per page.

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

**M3 — the cycle refusal.** Measure the hang FIRST (a deliberately constructed
cycle behind a test-only knob, so the measurement is real), then refuse it.

**M4 — gate D9.** The decision is made (option (a)); this milestone MEASURES the
dropout and pins it, so "a live lane does not feed a send" cannot rot into
"a live lane sometimes feeds a send".

**M5 — UI.** Send controls on the track head / detail pane, and the send lane's
own strip. Depends on nothing above except M1.

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
