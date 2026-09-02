# Proposal 45 — System lanes: the master track, its inserts, and the shape send tracks will take

> **Status: M0 and M1 EXECUTED (2026-09-01); M2 onward proposed.** The milestones below are
> ordered so that the two genuinely dangerous changes (M2's shape check and M3's
> Closure wiring) land before any UI can reach the situation they cover.
>
> **Revision 2** after an adversarial review against the tree. Three claims in
> revision 1 were WRONG and are corrected in place rather than quietly dropped,
> because each of them was the reason for a design decision:
>
> - **D4/T1 said the first master insert would silently turn monitoring OFF.**
>   It would not. `twlive::checkMasterShape` inspects only the `twMixer`'s input
>   levels and the `twRewire`'s channel map, so a plugin chain interposed
>   *between* them is **invisible to it** — monitoring stays on in LinearSplit,
>   the live rung bypasses the master chain, and the user hears a wet/dry
>   mismatch with no log line. Silently wrong is worse than off, and the fix
>   moves into **M2** (D4a).
> - **D6 said the clip policy could be enforced at one existing seam,
>   `splacements::laneAt()`.** It cannot: `laneAt(` has **71 call sites across
>   45 files** and is the general lane resolver for `set-track-volume`,
>   `arm-track`, the automation verbs and the meter test verbs — a refusal
>   inside it would break the master fader this proposal exists to ship. A
>   distinct placement seam is needed (D6).
> - **D9 said "the parse gains one branch".** Paths are `QList<int>` end to end
>   (`sobjectpath.h:45-51`), `"$master".toInt()` is **0** — it silently resolves
>   to the first user track — and `pathOf()` walks `childLinks()`, so it returns
>   `{}` for the master lane, which is the address of **the root mixer**. Every
>   head control derives its commit address that way. D9 is redesigned and
>   promoted into M1.

Prerequisite reading, in this order — the first two are not optional, because
each contains a sentence that invalidates the obvious design:

1. `smaragd/main/shell/CONTRACT.md` **inv. 18a** — "The `Closure` master mode is
   REFUSED, not approximated … whoever adds a master insert chain lands here
   first." That is this proposal.
2. `smaragd/tw303a/playback/include/tw/playback/twliveplan.h:180-209` and
   `smaragd/tw303a/playback/src/twliveplan.cc:76-124` — the algebra the live
   lane rests on, **and the exact two components the check looks at**.
3. `smaragd/main/objects/mixer/CONTRACT.md` (inv. 1, 5, 6),
   `smaragd/main/objects/track/CONTRACT.md`,
   `smaragd/main/timeline/CONTRACT.md` (inv. 1, 10, 28),
   `plan/proposed/41_LANE_FRAGMENTS.md` §D3 (why `isPathContainer()` and
   `isLane()` are two questions),
   `plan/proposed/21_REALTIME_DATAFLOW_INTEGRATION.md` D2/D3.

---

## Why

**1. There is no master track, and the object standing in for it cannot grow
one.** The root of an arrangement is an `SStdMixer`: one `twMixer` summing every
top-level track, feeding one `twRewire`
(`main/objects/mixer/src/sstdmixer.cpp:489-520`, and `setNBusses` wiring
`cpRewire_->setInput( i, mix->linkOutput( 0 ) )`). That chain has **no plugin
chain and no gain stage**. There is nowhere to put a master limiter, a master EQ
or a master fader, and no amount of UI work creates one — the components are not
there.

**2. The master already has a fader field, and it is dead.** `SStdMixer`
inherits `SObject::volume_` and answers `isLane()` true (proposal 41 D3: "the
root mixer is itself a lane"). Nothing applies it — `set-track-volume`'s own
source says so in a comment: *"Volume is an STrack property (the root mixer is a
lane but has no fader)"* (`ssettrackvolumeaction.cpp:48-49`). **Dragging a master
fader today would move a number nobody reads.** Any design that leaves the field
in place ships two master faders.

**3. There are no send tracks, and no shape for one.** `grep -rni
"sendtrack\|auxsend\|send bus"` over `main/` returns nothing. A send lane needs
exactly what the master needs — a lane with inserts and a fader that is *not* a
place clips go — so building the master's answer twice is the failure mode to
avoid.

**4. Nothing that is not a top-level `STrack` can be a row in the arranger.**
`SStdMixerView::appendRowsFor` (`sstdmixerview.cpp:4358-4381`) iterates
`childLinks()` and `continue`s on anything that is not an `STrack`;
`STrackRow::track` is typed `STrack *` (`sstdmixerview.h:66`); the head widget is
`SSMVMixerControl( …, STrack & )`. So "represent the master in the arrangement"
is, concretely, "make the master something those three already accept".

---

## The rule this proposal adopts

> **A system lane is an ordinary `STrack` that answers a `systemRole()` other
> than `None`. It is not a new kind of object; it is a track the project owns
> rather than the user, with a policy attached.**

The reason is measured rather than aesthetic: **`dynamic_cast<STrack *>` appears
98 times across exactly 40 files** in `main/`. Every one is a site a new lane
*type* would have to be audited at, and this repo's record says what happens
when parallel implementations are introduced and only one half is later changed
(proposal 41 M7: paint and hit-test disagreed on z-order, green for two
milestones; proposal 39 M2: the fader multiply; proposal 37 P6: the `slots:`
access specifier). A system lane that IS an `STrack` inherits, already gated:
the plugin chain and all five plugin verbs, the gain stage and its automation,
level metering, the automation lane UI, the FX strip, the Track Detail dock, the
arranger row, the head, and serialization.

The rule has **two** costs, and the proposal is honest that the second one is
structural:

- a **policy** surface — a track that must refuse things ordinary tracks accept
  (D6);
- a **default-open blacklist**. Every track-addressed verb written after this
  proposal ships *accepting* system lanes unless someone remembers to refuse
  them. `fix/take-lane-click-and-slip` found ~10 verbs that had silently missed
  an analogous case for years. AC5.6 is the mitigation: an enumerated
  accept/refuse row for *every* registered track-addressed verb, checked by
  `action_roundtrip_test`, so a new verb with no row is a failing test rather
  than a latent hole.

---

## Part A — the model

### D1. `SObject::systemRole()`, on the base class, never a `dynamic_cast`

```c++
enum class SSystemRole { None = 0, Master, Send, Conductor };
virtual SSystemRole systemRole() const;   // default None
```

On `SObject` for the reason `contentKind()`, `resolveEventClip()`, `isMissing()`
and `isLaneFragment()` are: **`app/model` must ask it** — the placement service
(`splacements`) and the path resolver (`sobjectpath.h`) both sit below
`objects/track` and cannot see an `STrack` policy header.

(Revision 1 also justified this by claiming `app/timeline` cannot include
`objects/track`. That is false — the edge is declared in
`tools/check_layering.py:194-197` and `sstdmixerview.h:14` includes `strack.h`
today. The `app/model` leg is the real one and is sufficient.)

Stored as a field on `STrack`, set once at construction and **immutable** — a
lane does not become the master. Serialized as `systemRole='master'` only when
it is not `None`, the same non-default-only discipline `editGroup`,
`midiRouting`, `collapsed` and `colorIndex` follow, and what keeps every
existing project file and both goldens byte-unchanged.

It must NOT be spelled as a conjunction of existing predicates. "A lane that is
not in `childLinks()`" happens to describe the master today and nothing else —
an accidental agreement, which is exactly what proposal 41 M0 exists to stop
relying on.

### D2. The master lane is NOT a `childLinks()` member

The load-bearing structural decision, and it is forced rather than chosen.

Tracks are addressed by an **index path from the arrangement root**
(`main/model/include/app/model/sobjectpath.h`; "`[]` is the root, `{2}` its 3rd
child"). Every `.qxa` case, every `trackPath=` in `docs/ACTIONS.md`, every
committed `.qxp` fixture and both goldens are written against the current
indices. A master lane inserted as a child of `SStdMixer` shifts all of them, and
`main/objects/mixer/CONTRACT.md` **inv. 1** ("`getNTracks()` counts TOP-LEVEL
children only — assertions in tests rely on this") becomes false in the same
edit.

It also breaks the audio twice: `reconnectTracksToMixer()` wires **every** child
into the sum (`sstdmixer.cpp:192-238`), so a master lane there would be summed
alongside the tracks it is meant to process; and `ssolo::anySoloInTree` walks the
root's `childLinks()` for lanes, so the master would join the solo set.

So the master lane is an **owned reference link**, the shape `STrack` already
uses for its plugin chain:

- `SStdMixer` holds `SLink *masterLaneRef_`, published through `ownedRefLinks()`
  and **not** through `childLinks()` (`strack.cpp:1297`; the header comment at
  `strack.h:73-80` explains why publishing matters for `~SProject`'s survivor
  ordering — a trap that already killed a build once).
- The lane object is a QObject child of `SProject`, so `SProject::serialize()`'s
  child loop writes it as a top-level element (`sproject.cpp:59-65`) — the
  mechanism `SPluginChain` already uses.
- `<SStdMixer masterLaneId='…'>` references it, resolved in
  `SProjectLoader::deferResolve`, because a plain attribute is invisible to the
  loader's `<SLink>`-based dependency ordering (`strack.cpp:1492-1503` says this
  about `pluginChainId`).

**Consequence, and it is not small: index paths cannot reach the master.** D9 is
what answers that, and it is a prerequisite of M1 rather than a footnote.

### D3. The audio topology, and why the goldens do not move

Today:

```
tracks → twMixer (sum) → twRewire → [project root component]
```

After M2:

```
tracks → twMixer (sum) → twPluginChain → twGainStage → twRewire → [root]
                          \____________________________/
                            owned by the master lane
```

- **The root component is still the `twRewire`.** The master meter
  (`sapplication.cpp:492-506`, `masterLevel` tapping `rootComponent()`),
  `RenderSession`, `AudioEngine` and `twSpeaker` all reach the graph through
  `SProject::getRootComponent()->getRootComponent()`. None changes, and the
  master meter becomes post-master-FX by construction rather than by a second
  tap.
- **An empty chain and 0 dB are arithmetically identical to today.**
  `twPluginChain::freezePage` forwards to its last insert and, with no inserts,
  forwards its input page verbatim; `twGainStage` at 0 dB unmuted does no
  arithmetic (proposal 37 P5). That is the same argument P5 used to keep every
  golden byte-unchanged. Note `twGainStage::renderPageWide` does render its
  **own** page rather than forwarding one (`twgainstage.cc:317-355`), so
  **AC2.1's `cmp` is a real measurement, not a formality** — keep it.
- **The master lane's own `twTrackMix` and `twRewire` are inert.** An `STrack`
  builds `twTrackMix → twPluginChain → twGainStage → twRewire`
  (`strack.cpp:1076-1097`). The master lane's `twTrackMix` has no clips (D6) and
  its `twRewire` is redundant with the mixer's. M2 wires the *mixer's* sum into
  the *lane's* chain and takes the lane's gain-stage output back into the
  *mixer's* rewire. Making the master lane's rewire the project root is rejected:
  it moves what every tap above resolves to, for nothing.

### D3a. Invalidation must travel THROUGH the master chain, and today it cannot

`SStdMixer::bumpRenderChainEpoch()` bumps `cpMixers_` and `cpRewire_` and
nothing else (`sstdmixer.cpp:156-177`); `bumpRenderChainEpochRange` likewise.
After D3 the master lane's `twPluginChain` and `twGainStage` sit **between**
them, each with its own page cache, and each bumped by **nobody** on an ordinary
clip or track edit. The rewire re-freezes, `fetchInputPage` serves the gain
stage's still-valid stale page, and **the edit is inaudible**.

This is the commonest path in the app — every user has an empty master chain and
edits clips — and it is silent. `SStdMixer::bumpRenderChainEpoch[Range]` must
extend to the master lane's components, and the gate for it is an *ordinary*
edit with the master wired and its chain **empty** (AC2.5). It is a different
edge from D11's "invalidation *from* the master" and neither subsumes the other.

### D4. Live monitoring: the algebra, and what actually breaks

Live monitoring rests on one identity:

```
master(unarmed ∪ live) == master(unarmed) + master(live)
```

true sample-for-sample **iff the master is a unity sum followed by an identity
map**. `twlive::checkMasterShape` is asked on every plan build, and today a
non-linear answer turns monitoring **off** with one log line, because the
`Closure` alternative is half-built: the plan builder can express it
(`sliveplanbuilder.cpp:329-349` assembles the master node with the unarmed
tracks as `frozenInputs`), but **nothing outside those two files reads
`twLivePlan::masterLinear`** — `twSpeaker` adds the frozen root page whenever the
frozen lane is playing, so a Closure plan would be summed on top of a root page
that already contains those tracks and the arrangement would be heard
**doubled**.

### D4a. **The shape check is BLIND to a master chain, and that is the real bug**

`checkMasterShape( const twMixer *, const twRewire *, idx_t width )` reads
exactly three things (`twliveplan.cc:76-124`): the two components' widths, every
`twMixer` input level, and the `twRewire` channel map. **A plugin chain and a
gain stage interposed between them are not arguments and are not inspected.**

So after D3's re-wiring, a user who drops a limiter on the master gets
`LinearSplit` — monitoring stays **on**, the RT adds the frozen root page (now
processed by the limiter) to a live ring that **bypassed the master chain
entirely**, and the two halves no longer belong to the same signal. A master
fader at −6 dB does the same thing to the split. There is no log line, because
nothing detected anything.

Revision 1 got this backwards and it changes the milestone plan:

- **M2 must extend the check** so that a non-empty master chain, a non-unity
  master gain, or a master mute reports `Closure`. That restores today's honest
  refusal (inv. 18a) for the whole window between M2 and M3, and it is a gate
  in M2 rather than a note in M3.
- **M3 then wires Closure**, at which point the refusal becomes audible
  monitoring.

### D4b. **Closure's hard part is not the summing — it is who owns the
processors**

"`twSpeaker` stops adding the frozen root page" is necessary and **not
sufficient**. The AudioEngine readahead demands **root** pages
(`audio_engine.cc:861,971`; `synthOutput_` is the mixer's rewire), and so does
`twSpeaker::warmFrozenLane` (`twspeaker.cc:353`, the count-in priming). Under
D3, freezing a root page runs the master lane's `twPluginSlotProcessor`s — and
under Closure the *pump* must render those same instances, on its own thread, to
produce the master's audio at all. Two threads, one plugin instance: exactly
what proposal 21's ownership protocol exists to prevent, and the master has **no
null-able input plug** for the closure exclusion to work through.

A related gap: the plan builder's master node today carries **no processors** —
`sliveplanbuilder.cpp:334-349` sets only `liveChildren`, `frozenInputs` and
`channelMap`. Attaching the master lane's inserts, gain envelope and mute to it
is unwritten work that M3 owns.

Three options, and M3 must **pick one in the design**, not discover one:

1. **Re-root the frozen demand below the master chain.** While a Closure plan is
   live, the readahead and `warmFrozenLane` demand the `twMixer`'s pages rather
   than the rewire's; the pump reads those as `frozenInputs` and runs the master
   processors itself. One owner at a time, which is the protocol's own rule.
   Cost: `AudioEngine`'s buffering-ready check and `startPlayback`'s frontier
   wait are expressed against `synthOutput_` and must follow the re-rooting —
   the same class of change `fix/loop-behaviour` (h) had to make when the
   frontier stopped being linear.
2. **Live-own the master's processors** the way an armed track's are owned. This
   contradicts AC3.3 as revision 1 wrote it (`liveOwnedRefusals == 0`), because
   root freezes would then legitimately refuse.
3. **Give the pump second instances.** Rejected: two DSP streams over one patch
   diverge in state, and D2's own measured hand-back cost (one phase step) is
   the evidence.

**Recommendation: option 1**, with option 2's refusal counter kept as the
diagnostic. AC3.3 is rewritten accordingly.

### D5. The master fader is the gain stage, and `SStdMixer::volume_` is retired

`twGainStage` is already the fader for every track, already post-FX, already
automatable via `self:Volume`, already ramped, and already the one thing
`set-track-volume` writes. The master lane owning one means there is exactly one
master fader and it is the same object as every other fader.

**"Retired" must be behavioural, not a grep.** `getVolume()` is inherited, not
overridden — there is no `SStdMixer::getVolume` text to find, and
`SObject::serializeSelfAttributes` reads `getVolume()` on every object including
the mixer (`sobject.cpp:157`). So the retirement is defined as:

- `set-track-volume` with an **empty** path (which today resolves the root mixer
  — `laneAt(root,{})` returns the root, whose `isLane()` is true) is
  **redirected to the master lane**, and a case proves the redirect is *heard*;
- the mixer's `volume_` keeps serializing as it does today (changing that moves
  existing files), and a CONTRACT line records that it is inert.

### D6. Policy: a system lane refuses clips — at a NEW seam, not an existing one

The requester's rule — *a master track must not directly contain sample or MIDI
cuts, but may have child tracks* — becomes:

```c++
virtual bool acceptsClips() const;      // SObject, default true
```

false for every `systemRole() != None` lane.

Revision 1 placed the check inside `splacements::laneAt()` on the grounds that it
is "the placement destination resolver". **It is not only that.** `laneAt(` has
**71 call sites across 45 files**, and they include `set-track-volume`
(`ssettrackvolumeaction.cpp:43`), `set-track-mute`, `set-track-name`,
`arm-track` (`sliveinputactions.cpp:22`), the automation verbs, the MIDI record
verbs and the meter test verbs. A refusal there would break the master fader
this proposal exists to ship.

So the seam is **new and narrow**: `splacements::placementLaneAt()` — `laneAt()`
plus the `acceptsClips()` test — and M5's first job is to **enumerate the
placement callers** (`place-clip`, `move-clip`, `place-recording`,
`place-midi-recording`, `add-sample`, `place-asset`, `pack-clips`' lane check,
and the arranger's `dropEvent`) and convert exactly those. That enumeration is
an audit, and AC5.1 says so instead of claiming the callers are untouched.

Three more refusals, same milestone:

- **No arm, and no monitor.** `arm-track` **and `set-monitor-mode on`** are
  refused: the live set is `{armed && monitorEffective} ∪ {monitor == on}`, so
  refusing only the first leaves a second door to the same contradiction with
  D4. `set-track-input` likewise.
- **No instrument.** Slot 0 of a system lane's chain may not be an instrument.
- **No structural edit.** `remove-track`, `reparent-track`, `move-track` refuse.
  It is not the user's object.

Every refusal is **announced** — a message naming the lane and the reason,
reaching the status line and `TW_LOG` — never a silent no-op ("A bound is
ANNOUNCED, never silent", proposal 38).

Mute and solo differ:

- **Mute on the master is legitimate** and is what `twGainStage`'s audio mute
  already does; a `self:Muted` lane on the master is a usable feature.
- **Solo on the master is meaningless and must be refused** — not merely
  useless: the master is not in `childLinks()`, so `anySoloInTree` cannot see
  it, and a solo flag there would be state nothing reads. That is the
  `SStdMixer::volume_` defect again, in a new field.

### D7. Conductor lanes are the *framework* here, not the content

"Child tracks like time or time signature" become **conductor lanes**:
`systemRole() == Conductor`, children of the master lane, `acceptsClips()` false,
hidden by default.

This proposal builds their ability to *exist, nest, hide, show, be addressed and
round-trip*. It deliberately does **not** build their content:

> `twTempoMap` is THE tempo authority and `SProject::getBPMTempo()` a derived
> view — "tempo is stored as SMF's own unit, µs per quarter (an integer), so BPM
> and the map cannot disagree" (proposal 37 P1). `set-tempo` is the ONLY tempo
> write, and it is an action.

A tempo lane must be a **VIEW of `twTempoMap`**, never a second store, and its
edits must funnel into `set-tempo`. Doing that properly needs a curve model for
ramps and is a proposal of its own. M6 gates the container.

### D8. Hiding is ONE mechanism: `isHidden()`, serialized when true, undoable

```c++
bool isHidden() const;                  // SObject; false by default
```

Serialized `hidden='true'` only when true — the `collapsed` precedent
(`strack.cpp:125`, `1449`). A system lane is *constructed* hidden, so the
attribute is present for it and absent everywhere else.

**One departure from that precedent, stated rather than smuggled:** `collapsed`
is serialized but its toggle is *not* undoable (`collapse-track` drives
`toggleTrackCollapsed()` directly). Hiding **is** undoable here, because hiding a
lane that carries inserts and automation is closer to an arrangement edit than to
a fold. The menu item **Show system lanes** is one `SCompositeAction` over the
per-lane verb — the `pack-selection` shape (`objects/mixer` inv. 7).

Rejected alternative: a per-user `SSettings` toggle *alongside* the serialized
flag. Two authorities for one question; and proposal 33 D2's split does not apply
— whether the master lane is on screen belongs to the arrangement, not to the
monitor.

**Hidden is a VIEW property and never an audio one.** A hidden master lane is
fully in the signal path. That needs saying: `isLiveOwnedLane()`,
`ssolo::isLaneAudible` and `isCollapsed()` are three existing flags that look
like they might mean "not heard" and do not.

### D9. Addressing — redesigned, and it is a milestone, not a footnote

D2 puts the master outside `childLinks()`, so it is unaddressable. Revision 1
proposed a `"$master"` string token and claimed "the parse gains one branch".
Three measured facts kill that as written:

- **Paths are `QList<int>` end to end.** `stringToPath` does `p.toInt()`
  (`sobjectpath.h:45-51`), `resolveByPath` takes `const QList<int> &`, and many
  action classes store a `QList<int>` member. A string token has no
  representation in the type.
- **`"$master"` today resolves to track 0.** `toInt()` on a non-numeric string
  is 0 — the "resolved against the wrong root SUCCEEDS" silent-corruption class
  `sobjectpath.h`'s own comment warns about.
- **The WRITE side aliases the master to the root mixer.** `pathOf()` walks
  `childLinks()` and returns `{}` — which is *also* "the root itself"
  (`sobjectpath.h:128-135`). `SSMVMixerControl` derives every commit address that
  way (volume, mute, solo, edit-group, the automation recorder's `ownerPath`)
  and already carries a guard for that exact ambiguity. So D11's "the head works
  unchanged because a system lane IS an `STrack`" was **false**: the master
  head's fader would move and commit nothing, or commit to the mixer.

**The recommended mechanism keeps the type.** A **negative index sentinel** in
the existing `QList<int>`:

```
{-1}          the arrangement's master lane
{-1, 0}       its first conductor child   (ordinary indices below the master)
{-2 - k}      the k-th send lane
"$master", "$master,0", "$send:Reverb"     surface spellings only
```

- `resolveByPath` gains one branch **before** `childAt()`: a negative step at an
  `SStdMixer` resolves a system lane.
- `pathOf` gains the matching branch, so the master lane has a representable,
  non-aliasing address.
- `stringToPath`/`pathToString` map the surface spelling to and from the
  sentinel, and `stringToPath` **rejects** an unrecognised `$`-token rather than
  `toInt()`-ing it to 0.
- **Every stored `QList<int>` action member is unchanged**, which is the whole
  return.

The cost is an audit: every consumer that indexes with a path step must reject
negatives rather than passing them to `childAt()`. That audit is **AC1.7**, and
it is the reason D9 belongs to M1 — M1's own AC ("round-trips with its inserts")
cannot be written without it.

The alternative — carrying the system role as a separate field on
`strackpath::QualifiedPath` — is viable and is *cleaner in the abstract*, but
every action that stores a bare `QList<int>` would have to grow a second member.
Recorded here so the decision is not re-litigated silently.

**EXECUTED (M1), and two things came out of it that the design did not
anticipate:**

- **A send name may not contain a colon.** `parseQualified` splits the root
  qualifier on the first ':' (`"Drums:0"`), so `$send:Reverb` parses as root
  `$send`. The reserved spelling is index-based (`$send0`) until M7 picks the
  name form; `$master` is unaffected.
- **Failing closed immediately found three fossils.**
  `plugin_order_divergence`, `plugin_remove_and_undo` and
  `plugin_stereo_chain` addressed `trackPath="/mixer/0"` — a spelling that
  exists nowhere in the code and worked only because `toInt()` answers 0. They
  had meant "track 0" by coincidence for years, and would have gone on
  silently addressing track 0 whatever they meant. This is the read-side
  hazard the sentinel exists to close, found in production data on its first
  run rather than argued for.

### D10. Send lanes: the shape is fixed here, the routing is NOT built

A send lane is a system lane (`systemRole() == Send`) with a plugin chain, a gain
stage, `acceptsClips()` false, a **name** (there can be several; D9 addresses one
by `$send:<name>`), and an output reaching the master sum.

**Not built here, and each of these is real work:**

- the **send itself** — a per-track, per-destination tap with a level and a
  pre/post-fader choice: a new auxiliary output on the track side, a model
  object, verbs, and an invalidation edge. At least the size of this proposal.
- **feedback prevention.** Send A → B → A is a graph cycle, and the scheduler's
  dependency counting will deadlock or spin on it rather than fail cleanly.
- **latency compensation across a send** (PDC is unimplemented, proposal 37 P9).
- **`reconnectTracksToMixer` must learn about system inputs.** It sets the
  mixer's input count from the track count and rewires **all** inputs on every
  audibility, solo or arm change (`sstdmixer.cpp:196-238`), so a send lane wired
  into a spare input is **clobbered by the next pass**. And `checkMasterShape`
  iterates `getNInputs()` levels, so a send input must be unity or monitoring
  dies for a new reason.

M7 is therefore *shape only*: a send lane exists, carries inserts and a fader,
sums into the master, hides, refuses clips and round-trips — with **nothing able
to feed it**. Deliberately a lane that does nothing audible yet, said out loud.

### D11. Invalidation FROM the master, and the arranger

**Invalidation from a master edit.** `SObject::invalidateRenderPathRange()` walks
**down from the project root through `childLinks()`** — the pitfall
`objects/track` inv. 13 records for an `SPluginSlot`, whose fix was to have the
*track* do the walk. The master lane is not in `childLinks()`, so **`SStdMixer`
must issue its invalidation**. Untested this is silent: the fader moves and the
render does not change — the shape proposal 41's
`fragment_nested_edit_reaches_all_placements` found, which only a RENDERED-audio
assertion caught.

**The arranger.** `appendRowsFor` gains a second phase after the recursive walk:
the arrangement's system lanes, in a fixed order (sends above, master last,
conductor lanes as the master's children under the existing `depth+1`
recursion), each appended only when `!isHidden()`, each followed by
`appendAutomationRowsFor` exactly as a user track is.

Four properties, each with a specific way of going wrong here:

- **`STrackRow::track` stays non-null**, so `rowHeightOf`, `drawTakeLane`,
  `pruneUiState` and the control column are unchanged. The largest single piece
  of evidence for D1.
- **`pruneUiState` walks the MODEL** to decide which per-track UI keys survive
  (there was no pruning before proposal 37 P6, and a removed track left a
  dangling key a later track at the same address inherited). It walks
  `childLinks()`. **A master lane outside that walk would have its height scale,
  fold state and shown-automation set pruned on every rebuild.**
- **Reorder is refused in the MODEL, not in the head.** The grip drag resolves
  through `move-track`, which D6 refuses; the head additionally draws no grip,
  but the head is cosmetic and the verb is the gate.
- **The system rows are not a clip drop target**, and the drop is not *accepted*
  visually either — a cursor promising a gesture the model then refuses is
  precisely the defect `fix/loop-behaviour` (b) fixed for the loop drag.

### D12. Metering: the tap, decided

Both meter mounts call `track.getRootComponent()` uniformly
(`ssmvmixercontrol.cpp`, `strackdetailpanel.cpp`). After D3 the audible signal
leaves the master's **gain stage** and enters the **mixer's** rewire, so a probe
pointed at the master lane's own (inert) rewire reads silence forever — and
`twLevelProbe` answers a miss by **decaying**, so it looks like a broken meter
rather than a wiring bug (proposal 34: "A page miss must DECAY the meter").

**Decision: `STrack::getRootComponent()` is overridden for the master role** to
return the mixer's rewire, rather than special-casing the two mounts. One
answer, at the one place every consumer already asks. The consequences are named
so they are not discovered: that override is also what `resolveClip()`, preview
building and the live plan builder's `channelMap` read, and for the master lane
each of those is *correct* under D3 — the mixer's rewire genuinely is that
lane's output. **T5** records that the lane's own trackmix and rewire stay inert
and must not be "wired for consistency" later.

### D13. Arrangement lifecycle

Arrangement roots are `SStdMixer` instances created and destroyed at runtime —
`create-arrangement` (`screatearrangementaction.cpp:45`),
`extract-arrangement` (`sextractarrangementaction.cpp:195`),
`remove-arrangement`, `dissolve-arrangement`. "Every arrangement root owns
exactly one master lane" therefore means those verbs **mint and destroy master
lanes**, and D2 parents the lane to `SProject` rather than to the mixer — so
**deleting an arrangement root does not delete its master lane**: it survives as
an orphan that serializes forever.

M1 owns this: creation mints, removal destroys (or explicitly re-parents), and
**undo of `remove-arrangement` resurrects the lane with its chain, gain and
automation intact** — an inverse built from a path is not enough, because the
lane is not at a path in the removed tree.

---

## Non-goals

- Sends themselves (D10). A send lane with nothing feeding it is the deliverable.
- Tempo, time-signature and marker *content* (D7).
- Plugin delay compensation (proposal 37 P9).
- A master-lane **input**: not armable, not monitorable, not recordable (D6).
- Changing `SStdMixer::isLane()`. It stays true; the master *lane* is a different
  object with a different job.
- Multi-output routing / hardware output busses. One master per arrangement.
- Retiring `SStdMixer` in favour of a master `STrack` at the root — a much larger
  refactor with the same D2 path problem and no user-visible gain.

---

## Traps

**T1. The shape check is blind to a master chain (D4a).** Between M2's re-wiring
and an extended `checkMasterShape`, a master insert leaves monitoring **on and
wrong**, with no log line. This is the single most dangerous window in the plan.

**T2. Index paths shift if the master is a child link (D2).** Every `.qxa`, every
fixture, both goldens, and `objects/mixer` inv. 1.

**T3. `reconnectTracksToMixer` sums every child**, and rewires **all** inputs on
every audibility change (D10) — so it both mis-sums a child master and clobbers
a send wired into a spare input.

**T4. `SStdMixer::volume_` is a live field nothing reads (D5).** Ship a master
fader without retiring it behaviourally and there are two.

**T5. The master lane's own `twTrackMix` and `twRewire` are inert (D3, D12).** A
future reader wires one "for consistency" and either doubles the sum or moves the
root component. Note also that `STrack::bumpRenderChainEpoch` will bump those
inert components on every master edit — harmless, and a red herring for whoever
debugs D3a next.

**T6. Invalidation THROUGH the master chain (D3a)** — silent, and it hits the
*empty*-chain case that every user has.

**T7. `ownedRefLinks()` must publish the master link.** `strack.h:73-80` records
what happened when the plugin-chain link was not published: `~SProject`'s
survivor ordering deleted the chain first and the destructor `removeRef()`'d
freed memory.

**T8. An older build DROPS the master lane's identity on a round trip.**
Serialization is regenerated from live objects; there is **no unknown-attribute
passthrough** (`sobject.cpp:131ff` writes an enumerated list). An old build
instantiates the top-level `<STrack>` with no parent, then re-saves it **without**
`systemRole` and **without** `hidden`, and re-saves `<SStdMixer>` **without**
`masterLaneId`. On return to a new build the reference is severed and the role is
gone: either a fresh master lane is minted beside a role-less orphan track, or
`masterLaneId` points at a `systemRole() == None` track that accepts clips and
can be armed. **A defensive load rule is required** — a `masterLaneId` target
that does not answer `Master` is coerced or rejected, never adopted as-is — and
it needs a hand-crafted fixture.

**T9. `pruneUiState` walks `childLinks()` (D11).** A master lane outside it loses
its UI state on every rebuild.

**T10. `SMidiOutPump` cannot see the master — and that is the hazard.**
`collectTracks` walks `childLinks()` from the root
(`smidioutpump.cpp:311-321`), so a master lane with `midiOutPort` set would
**never emit**. The refusal is still right; the reason is the *inverse* of what
revision 1 wrote — it is the `volume_` defect shape, a settable field that
silently does nothing. `set-monitor-mode` and `set-track-input` are the same
shape (D6).

**T11. Invalidation FROM the master (D11)** — the root-down walk cannot reach it.

**T12. `render_while_armed` byte-identity.** Proposal 21's gate asserts an armed
render equals an unarmed one. M3 changes what `twSpeaker` does while a live plan
is up; renders suspend live lanes, so it should hold — a prediction, and
therefore something to check.

**T13. `getNTracks()` and `assert-arrangements`** must keep counting user tracks.

**T14. Goldens** are byte-identical only while the master chain is empty and its
gain is 0 dB (D3). Any milestone shipping a default master insert (none is
planned) re-freezes them under a recorded licence, as B4 and B5 did.

**T15. Arrangement lifecycle leaks (D13).** A `SProject`-parented lane does not
die with its mixer.

**T16. `pathOf` aliasing inside the master subtree.** A conductor lane at
`{-1,0}` is invisible to `findPathRec`, so a UI gesture on a conductor row that
derives its address the usual way gets `{}` → the mixer. D9's `pathOf` branch has
to descend into the master lane too.

**T17. The count-in and a record start under Closure.** `warmFrozenLane` demands
the **root** explicitly, and `startPlayback` waits on the readahead frontier of
the root; D4b option 1 re-roots both. A count-in with a master insert up is a
distinct case from plain playback and needs its own gate.

**T18. The mid-play flip has no trigger yet.** `pumpEdits`' plan signature covers
closure members' chains; a master insert or removal must reach
`SLiveMonitor::refresh` through some master-lane analogue of
`SAppContext::liveLanesChanged`.

---

## Milestones, acceptance criteria and gates

Every milestone ends green on: `./build.sh`, `python3 tools/check_layering.py`,
`python3 tools/check_logging.py`, `ctest --test-dir smaragd/build -j4
--output-on-failure`, a **reconciled** registered/run/skipped count (measure it;
never quote it), and **byte-identical goldens** (`smaragd/tests/goldens/`,
16-bit PCM, `cmp`) unless the milestone says otherwise.

**Every new gate must be watched FAILING on the pre-fix binary**, and the PR must
say which sabotage bit which assertion — a single sabotage that fails everything
proves nothing. This repo has shipped a green gate over a broken paint or a
broken audio path four times (proposal 39 M2, proposal 41 M5,
`fix/take-lane-domain`, `fix/take-lane-capture-align`).

### M0 — `systemRole`, `isHidden`, `acceptsClips` on `SObject` (pure refactor)

- **AC0.1** All three exist on `SObject` with defaults `None` / `false` / `true`;
  nothing overrides them yet.
- **AC0.2** No caller spells any of them as a conjunction of existing predicates
  (proposal 41 D3) and no new `dynamic_cast` is introduced. *This is a review
  item, not a grep-able gate, and is listed as one.*
- **AC0.3** Zero behaviour change.
- **Gate:** full `ctest`; `cmp` on both goldens; one definition each by grep.

### M1 — Addressing, the master lane, ownership, lifecycle (still silent)

D9 lands here because M1's own round-trip AC cannot be written without it.

- **AC1.1** Every arrangement root owns exactly one master lane: an `STrack` with
  `systemRole() == Master`, reachable through `SStdMixer::masterLane()`,
  published in `ownedRefLinks()` (T7) and **absent from `childLinks()`** (D2).
- **AC1.2** `getNTracks()`, every existing `trackPath`, `assert-arrangements` and
  both goldens are unchanged — the lane is in the file and in no signal path
  (T2, T13).
- **AC1.3** **Addressing (D9).** `resolveByPath` and `pathOf` handle the sentinel;
  `pathOf( root, masterLane )` returns `{-1}` and **not** `{}`; `stringToPath`
  maps `"$master"` to it and **rejects** an unknown `$`-token instead of
  resolving it to track 0.
- **AC1.4** It serializes as a top-level element with
  `<SStdMixer masterLaneId='…'>`, resolved in `deferResolve`, and round-trips
  with its inserts, gain, automation lanes and `hidden` flag.
- **AC1.5** **Defensive load (T8).** A hand-crafted fixture whose `masterLaneId`
  points at a track that does **not** answer `Master` is coerced or rejected,
  never adopted; and a pre-M1 project (no `masterLaneId`) is given a fresh master
  lane with nothing audible changed.
- **AC1.6** **Lifecycle (D13, T15).** `create-arrangement` and
  `extract-arrangement` mint a master lane; `remove-arrangement` and
  `dissolve-arrangement` destroy it; **undo of a removal restores the lane with
  its chain, gain and automation.**
- **AC1.7** **The negative-index audit.** Every consumer that indexes with a path
  step rejects a negative rather than passing it to `childAt()`; enumerated in
  the PR.
- **AC1.8** *(MOVED TO M4.)* The master lane has no arranger row until M4, so
  there is no per-track UI state for `pruneUiState` to keep or drop yet.
  Asserting it here would assert nothing.
- **AC1.9** *(RESOLVED WITHOUT A CODE CHANGE.)* The retirement of
  `SStdMixer::volume_` is behavioural, per D5 — and the behaviour was already
  correct: `set-track-volume` resolves its lane and then requires an `STrack`
  (`ssettrackvolumeaction.cpp:48-52`, whose own comment reads "the root mixer
  is a lane but has no fader"), so an EMPTY path is REFUSED rather than
  silently writing a number nobody reads. `$master` is therefore the one
  spelling of the master fader, no verb ever writes the mixer's `volume_`, and
  the field is documented inert in `objects/mixer/CONTRACT.md`. Redirecting
  the empty path was considered and rejected: it would give the master fader a
  second address for no gain.
- **Gate:** new `systemlane_test` (ownership, serialization, the sentinel's
  resolve/`pathOf` round trip, teardown order under ASAN where available);
  new qxa `master_lane_roundtrip`, `master_lane_lifecycle`,
  `master_lane_bad_reference`; `action_roundtrip_test`; `cmp` on both goldens.
- **Watched failing (DONE, three independent sabotages, each isolated):**
  (1) `pathOf`'s sentinel branch removed → exactly the three `expectPath`
  assertions fail (#9, #15, #18) and nothing else; (2) `"$master"` no longer
  parsed and unparsable text back to `toInt()` → `role` (#3) and
  `inChildLinks` (#7) fail, and the negative control (#8) reports *"applied
  but expectReject was set"* — the mistyped `$mastr` SUCCEEDS, resolving to
  track 0, which is the silent corruption D9 exists to prevent, demonstrated
  rather than argued; (3) the defensive role check deleted → `assert-log` and
  the `plugins`/`volume` assertions of `master_lane_bad_reference` fail while
  `master_lane_roundtrip` stays green.
- **Measured:** suite 330/330 green (335 registered, 5 disabled — the 3 macOS
  `au_*` plus the 2 Windows-only media cases), both goldens byte-identical.
  AC1.7 resolved by construction: every path consumer indexes through
  `SObject::childAt(int)`, which bounds-checks negatives, so a sentinel fails
  closed at one accessor and no call site needed changing.

### M2 — The master lane is in the signal path, and the shape check learns about it

- **AC2.1** With an empty chain at 0 dB, a render is **byte-identical** to the
  pre-M2 render — `cmp`, not a tolerance (D3).
- **AC2.2** A gain plugin (`tw.test.clap.gain`) on the master at 2.5× is heard:
  rendered RMS is 2.5× the pre-insert RMS to a closed form over
  `tests/test_autosaw.wav`, on **both** paths — the offline render and the
  playback capture backend.
- **AC2.3** The master fader is heard and it is `twGainStage`: −6 dB halves the
  amplitude, and a `self:Volume` ramp on the master is heard per second in closed
  form (the `automation_volume_ramp` shape).
- **AC2.4** A **master** edit invalidates (T11): render, insert a master plugin,
  render again *in the same process*, second render reflects it. A case that
  renders only afterwards cannot see this and is not the gate.
- **AC2.5** An **ordinary** edit invalidates **through** an empty master chain
  (D3a, T6): render, change a clip and a track fader, render again in-process,
  second render reflects both. This is the commonest path in the app and it is
  silent when wrong.
- **AC2.6** The master meter reads post-master-FX and is ONE probe (D12): after
  AC2.2's insert the transport meter and the master head meter agree, and neither
  decays.
- **AC2.7** **`checkMasterShape` learns the chain (D4a, T1).** A non-empty master
  chain, a non-unity master gain, or a master mute reports `Closure`, so
  `SLiveMonitor` refuses monitoring exactly as inv. 18a does today — an honest
  refusal for the whole M2→M3 window rather than silent wrongness.
- **AC2.8** The master lane's own `twTrackMix`/`twRewire` are unwired, asserted
  through accessors read by `systemlane_test` (T5) — not "asserted structurally".
- **Gate:** new qxa `master_insert_heard`, `master_fader_heard`,
  `master_edit_invalidates`, `track_edit_invalidates_through_master`,
  `master_meter_postfx`, `master_insert_refuses_monitoring`; `cmp` on both
  goldens for AC2.1; `metering_test`; `playback_test` for AC2.7's predicate.
- **Watched failing:** AC2.7 by reverting the check extension (monitoring stays
  on with an insert up); AC2.5 by reverting the epoch extension (the second
  render is byte-identical to the first when it must not be).

#### M2 PART 1 — **DONE 2026-09-02: the lane is in the path and every edge invalidates**

D3, D3a and D11 are executed; AC2.1-AC2.5 are met and gated. AC2.6, AC2.7 and
AC2.8 are **NOT** started — see the bottom of this block.

**D3, the wiring.** `SStdMixer::wireMasterChain()` puts the master lane's
`twPluginChain` and `twGainStage` between the bus sum and the rewire, called
from the constructor (after minting) and from `adoptMasterLane` (after
replacing — without it the sum keeps running through the RETIRED lane's chain,
which is about to be deleted). The lane's own `twTrackMix` and `twRewire` go
inert, as T5 requires.

**AC2.1 HOLDS AND IT IS A REAL MEASUREMENT.** `mc_stereo.wav` is byte-identical
to the committed golden with the chain in the path — an empty `twPluginChain`
forwards its input page verbatim and `twGainStage` at 0 dB unmuted does no
arithmetic, exactly as proposal 37 P5 argued. Both goldens `cmp` clean.

**D3a WAS FOUND BY AN EXISTING GATE, NOT BY READING.** With the wiring in and
the epoch extension missing, `mc_golden_stereo`'s own negative control flipped:
its `set-track-mute` plus re-render produced a file **byte-identical to the
unmuted golden**, so the assertion that exists to prove the comparison can fail
reported *"applied but expectReject was set"*. A mute had stopped being
audible. `bumpMasterChainEpoch[Range]()` fixes it.

**D11 NEEDED A NEW SEAM ON `SObject`, and the milestone plan did not say so.**
`SObject::invalidateRenderPath()` walks DOWN from the project root through
`childLinks()`; a system lane is deliberately not among them, so the walk does
not find it and only the lane's OWN caches are bumped. New virtual
`SObject::renderPathOwner()` (null for every ordinary object; the mixer for its
master lane, set at mint and at adopt) is consulted in the existing `!found`
branch of both invalidation entry points. Delegating there rather than at every
call site is what stops the next edit kind from being silently inaudible.

**Measured, all closed forms over `tests/test_autosaw.wav`** (480 Hz sawtooth,
period exactly 100 frames, so a whole-second window is an exact number of
periods and its RMS is `A/sqrt(3)` with no windowing error). Base over
[48000, 96000) channel 0: **0.230956**, i.e. A = 0.40003.

| | closed form | measured |
|---|---|---|
| master insert at 2.5x (AC2.2) | 0.577390 | **0.577391** |
| master fader at −6 dB (AC2.3) | 0.115747 | **0.115754** |
| master fader at −12 dB | 0.058013 | (banded) |

2.5x peaks at **0.99999**, inside full scale, so the gain this proposal names
is usable as written rather than needing headroom — checked rather than
assumed, because the render is 16-bit PCM and would have clipped silently.

**Watched failing, two sabotages, each isolated — neither edge subsumes the
other, demonstrated rather than argued:**

| Sabotage | Fails | Stays green |
|---|---|---|
| D3a reverted (the master chain is not bumped) | `track_edit_invalidates_through_master`, both goldens | `master_insert_heard`, `master_fader_heard` |
| D11 reverted (a master edit reaches nothing downstream) | `master_insert_heard`, `master_fader_heard` | `track_edit_invalidates_through_master`, both goldens |

**A TRAP THAT COST TWO WRONG DIAGNOSES and belongs in any new plugin case:**
`insert-plugin` without `path="twtestclap.clap"` finds no module, quietly builds
the **transparent placeholder**, and every level assertion then reads the base
RMS. That failure looks exactly like a broken signal path and is not one.

**NOT DONE in this part**, and each is a gate the milestone still owes:
AC2.6 (the master meter post-FX — D12's `getRootComponent()` override for the
master role is not written, so a probe pointed at the lane reads its inert
rewire and decays), AC2.7 (`checkMasterShape` still cannot see a master chain,
so a limiter on the master leaves monitoring ON and the two halves of the split
no longer belong to the same signal — the honest refusal inv. 18a used to give
is absent for the whole M2→M3 window), and AC2.8 (the lane's own trackmix and
rewire are inert by construction but nothing asserts it).

**Also settled here:** D5's body text (line ~327) still says `set-track-volume`
with an empty path "is **redirected to the master lane**". AC1.9 rejected that
redirect. `master_fader_heard.qxa` now asserts the refusal, so the two cannot
drift apart silently; D5's sentence is the stale one.

### M3 — **The `Closure` master mode, wired** (T1, D4b)

- **AC3.1** The processor-ownership question is **answered in the design**: the
  frozen demand is re-rooted below the master chain while a Closure plan is live
  (D4b option 1), `AudioEngine`'s buffering-ready check and frontier wait follow
  the re-rooting, and `twSpeaker` stops adding the frozen root page.
- **AC3.2** The plan builder's master node carries the master lane's **inserts,
  gain envelope and mute** (absent today, `sliveplanbuilder.cpp:334-349`).
- **AC3.3** The unarmed tracks are neither **doubled** nor **lost**, measured
  three ways because they are three different failures: a closed-form **RMS**
  level (a doubling is present from the first frame and produces no *step*, so
  `maxStep` alone would not bite), plus `assert-audio-continuity`'s `maxStep`
  for the flip transient and `maxGapFrames` for the dropout.
- **AC3.4** No two-thread access to one plugin instance: root freezes and pump
  renders never overlap on the master's processors, with `liveThreadRefusals`
  **0**. `liveOwnedRefusals` is **reported, not asserted zero** — under D4b that
  number is a diagnostic of the chosen option, and asserting 0 would push an
  implementer toward the racy one.
- **AC3.5** `SLiveMonitor` no longer refuses a master carrying an insert: the
  refusal message is absent and monitoring is **audible**, asserted.
- **AC3.6** Flipping linear ↔ Closure **while the transport runs** costs at most
  the house bound of **1024 frames** (proposal 21 L2) — anything tighter asserts
  what the design does not offer — and the flip has a **trigger** (T18).
- **AC3.7** **Count-in and record start under Closure** (T17): a count-in with a
  master insert up still primes (proposal 21 L5's "0 buffering polls" shape), and
  a record start places its take at the same frame as with a linear master.
- **AC3.8** `render_while_armed` still produces a byte-identical render (T12).
- **Gate:** `playback_test` gains a Closure case at widths 1/2/6 with no device
  opened (the `twmonitor::pullChannels` precedent); new qxa
  `monitor_master_insert`, `monitor_master_flip_midplay`,
  `record_count_in_master_insert` — all `RUN_SERIAL` at
  `SMARAGD_CAPTURE_SPEED=1` against the paced `file:` input;
  `render_while_armed` unchanged.
- **Watched failing:** with AC3.1 reverted, `monitor_master_insert`'s **RMS**
  assertion must show the doubling, with the measured numbers in the PR.

### M4 — The master lane on screen

- **AC4.1** A row in the arranger when shown, pinned below every user lane, with
  a head carrying its fader, meter, mute and FX access (D11).
- **AC4.2** Hidden by default; `set-lane-hidden` toggles as one undo step; **Show
  system lanes** is one composite (D8).
- **AC4.3** Hidden ≠ silent: with the lane hidden and a master insert up, AC2.2's
  level assertion still holds.
- **AC4.4** **The head's fader commit is HEARD**, through the real
  `applyVolumeDb`/`applyVolume_` handler (the `automation_write_pass` shape).
  This is the assertion that bites D9's write-side defect — a head whose slider
  moves and whose action never fires.
- **AC4.5** Existing lane geometry unchanged: `assert-lane-alignment` passes with
  a visible master, and user tracks' take/automation groups are unmoved.
- **AC4.6** The master's own automation sub-lanes appear and honour the
  shown-automation set keyed by `STrack *`.
- **Gate:** new qxa `master_lane_rows`, `master_lane_hidden_still_audible`,
  `master_head_fader_heard`; `assert-track-head`; `assert-lane-alignment`;
  `action_roundtrip_test`.

### M5 — The policies, each refused and each announced

- **AC5.1** A clip dropped on, moved to, or placed on a system lane is REFUSED
  with a message naming the lane. The check lives in the new
  `splacements::placementLaneAt()`, and the PR **enumerates the placement callers
  converted** (D6) rather than claiming none were edited.
- **AC5.2** `arm-track`, **`set-monitor-mode`**, **`set-track-input`**,
  `remove-track`, `reparent-track`, `move-track`, `set-track-solo` and
  `set-track-midi-output` are refused on a system lane, each with its own message
  (D6, T10).
- **AC5.3** An instrument in slot 0 of a system lane's chain is refused; an
  ordinary effect is accepted.
- **AC5.4** Mute IS accepted on the master and is audible, including as a
  `self:Muted` automation lane.
- **AC5.5** Every refusal leaves the model untouched and puts nothing on the undo
  stack.
- **AC5.6** **The enumerated verb table.** Every registered track-addressed verb
  has an explicit accept/refuse row, checked by `action_roundtrip_test`, so a
  verb added later with no row fails the suite rather than silently accepting a
  system lane (the default-open-blacklist mitigation).
- **Gate:** new qxa `master_refuses_clips`, `master_refuses_arm_and_structure`,
  `master_refuses_instrument`, `master_mute_audible`; each refusal watched failing
  by removing **its own** check and confirming which assertion bites.

### M6 — Conductor lanes: the container only

- **AC6.1** A conductor lane exists as a child of the master lane, addressable as
  `$master,0` (`{-1,0}`), hidden by default, refusing clips, contributing no
  audio, round-tripping.
- **AC6.2** It carries **no tempo state of any kind** — `twTempoMap` stays the one
  authority and `set-tempo` the one write (D7), asserted by grep the way the P1
  gate asserts `bpmTempo_`'s writers.
- **AC6.3** **A gesture on a conductor row commits to the conductor lane** (T16),
  not to the mixer — the `pathOf`-descent half of D9.
- **AC6.4** Nesting changes no `getNTracks()`, no user `trackPath`, no golden.
- **Gate:** new qxa `conductor_lane_container`, `conductor_lane_addressing`; the
  AC6.2 grep; `action_roundtrip_test`; `cmp` on both goldens.

### M7 — Send lanes: shape only, routing explicitly NOT built

- **AC7.1** A send lane can be created by verb, is named, carries a plugin chain
  and a gain stage, sums into the master, hides and shows, refuses clips, and
  round-trips.
- **AC7.2** Addressable as `$send:<name>`; the five plugin verbs reach it
  unchanged (D9).
- **AC7.3** **Nothing feeds it**: with no source it contributes silence and both
  goldens are byte-identical. The milestone asserts the absence of routing rather
  than implying its presence.
- **AC7.4** **The send survives a `reconnectTracksToMixer` pass** (T3, D10): the
  gate toggles a solo or an arm — which forces a full rewire — **before**
  rendering. Without that step this gate goes green over the clobbering defect.
- **AC7.5** Two send lanes coexist; a name collision is refused.
- **Gate:** new qxa `send_lane_shape`, `send_lane_survives_rewire`; `cmp` on both
  goldens; `action_roundtrip_test`.
- **Splittable.** If M7 becomes its own proposal, M0-M6 stand alone.

### M8 — Contracts, docs, and the CLAUDE.md section

- **AC8.1** `main/objects/mixer/CONTRACT.md` gains the ownership, lifecycle and
  epoch-extension invariants (D2, D3a, D13, T7, T8, T15);
  `main/objects/track/CONTRACT.md` the system-role and policy invariants (D6);
  `main/model/CONTRACT.md` the sentinel addressing (D9);
  `main/timeline/CONTRACT.md` the pinned-row and hidden≠silent invariants;
  `main/shell/CONTRACT.md` **inv. 18a is rewritten** — it currently names this
  proposal as the thing that lands there.
- **AC8.2** `docs/ACTIONS.md` gains `set-lane-hidden`, `create-send-lane`, the
  `$master` / `$send:` spellings, and every refusal above documented as a
  refusal.
- **AC8.3** `CLAUDE.md` gains a section in the house style — the "read this
  before touching X, the obvious design is wrong" table — whose first row is
  D4a/T1: *the shape check cannot see a master chain.*
- **AC8.4** The PR body says what was gated **and what was not**, names every
  unreproduced flake, and reports measured registered/run/skipped counts rather
  than quoting any figure from `CLAUDE.md`.
- **Gate:** documentation review; `docs/ACTIONS.md` rows verified against the
  registered verbs by `action_roundtrip_test`.

---

## What this proposal will NOT gate

- **Real device latency and jitter** under a Closure-mode live lane. Every
  proposal-21 measurement is against the capture backend and so is this one.
- **ASIO and WASAPI under load** with a master insert up. The `-j` gate's
  wall-clock caveats apply (`twlog_test`, `qxa.log_dock_scale`,
  `monitor_latency`): confirm the box is idle before reading a timing failure as
  a regression.
- **Sends** — nothing can feed a send lane (D10, AC7.3). Feedback cycles,
  pre/post-fader taps and send-path latency are out of scope.
- **Tempo, time-signature and marker content** (D7, AC6.2).
- **Plugin delay compensation** on the master (proposal 37 P9). A
  latency-reporting master insert is heard late by exactly its reported amount.
- **Pixel aesthetics** of the master head and row — a geometry relation and a
  luminance relation, never a palette.
- **The Show-system-lanes MENU ITEM.** There is no testkit verb for a context
  menu anywhere in this repo, so what is gated is the VERB it submits and the
  predicate its enabled state reads. The wiring is hand-verified.
- **An older build's round trip** (T8). The defensive *load* rule is gated; the
  old build itself is not — there is no second build in the gate.
- **A master lane inside a nested arrangement asset.** Arrangements can be
  windowed as assets (proposal 09); D3's topology answers what a placed
  arrangement's master does, and **no case exercises it.**
- **More than one arrangement's master lanes at once.** The design is per-root
  throughout; nothing measures two.

---

## Sub-agent assignment

**M3 must not be delegated without the reading list at the top**, and neither
must **M2's AC2.7**: those two are the only places where the failure mode is
audibly wrong output rather than a refused verb, and D4b's ownership question has
to be *decided* before code is written. **M1 is larger than it looks** — it
carries D9's addressing audit and D13's lifecycle work, either of which could be
its own branch. M0 and M8 are mechanical. M5 parallelises well once M0 has
landed, because its ACs are independent refusals.
