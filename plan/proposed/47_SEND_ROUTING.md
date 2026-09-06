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

### D9. THE LIVE-MONITORING HOLE, and it is the hardest thing here

**An armed, monitored track's send is SILENT, and that falls out of the design
rather than being chosen.** The live plan excludes a live-owned track from the
frozen sum — the mixer nulls its input plug — and the pump renders it
block-wise straight into the ring. Its send tap, however, is on the FROZEN path:
the send bus reads the track's `twGainStage`, whose pages are the pages that are
no longer being produced. So while a guitarist monitors through their chain, the
reverb they hear on playback drops out.

Three readings, and this proposal does not pretend the choice is obvious:

| Option | Cost |
|---|---|
| **(a) Accept and announce** | The monitored signal is dry. Cheap, honest, and wrong for the one workflow sends exist for (singing to a reverb). |
| **(b) Put send lanes in the live CLOSURE** | The pump must render the tap, the send bus, the send lane's chain and fader, per block — that is the pump growing a second graph, which 21 D1 spent a milestone arguing against. |
| **(c) Feed the ring from the tap** | The RT sums the ring onto the frozen root page; a send contribution would have to be summed at the send bus instead, which the RT does not reach. |

**M4 decides between them with a measurement, not in this document.** (a) is the
default and is what M1-M3 ship; anything else is a milestone of its own.

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

**M1 — the send bus, audible.** `wireAsSendLane`, `rewireSendBuses()` inside the
pass (D4), pre/post tap points (D3), level as the bus input level (D7),
audibility (D5). Gated by RMS through a render against a closed form.

**M2 — invalidation.** T3 and T4. Gated by an edit-then-render case that is
watched failing with the invalidation removed.

**M3 — the cycle refusal.** Measure the hang FIRST (a deliberately constructed
cycle behind a test-only knob, so the measurement is real), then refuse it.

**M4 — the live-monitoring decision (D9).** Measure the dropout, choose between
(a)/(b)/(c), and record the reasoning.

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
- **The monitored-send dropout** until M4 decides it (D9). M1-M3 ship option
  (a) and say so.
- **Pixels** anywhere — a send strip is `SSMVMixerControl` unchanged until M5,
  and M5 will assert a geometry relation, never a palette.
- **A send lane inside a nested arrangement asset**, and **more than one
  arrangement's send lanes at once** — 45 left both unmeasured and this
  proposal does not widen them.
- **Sends from a MIDI or event lane** — a tap is an AUDIO tap; a MIDI track
  whose notes reach no instrument has no audio to send.
