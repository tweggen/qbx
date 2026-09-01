# Proposal 45 — System lanes: the master track, its inserts, and the shape send tracks will take

> **Status: PROPOSED.** Nothing here is executed. The milestones below are
> ordered so that the one genuinely dangerous change (M3) lands *before* any UI
> can reach the feature that would trigger it.

Prerequisite reading, in this order — the first two are not optional, because
each of them contains a sentence that invalidates the obvious design:

1. `smaragd/main/shell/CONTRACT.md` **inv. 18a** — "The `Closure` master mode is
   REFUSED, not approximated … whoever adds a master insert chain lands here
   first." That is this proposal.
2. `smaragd/tw303a/playback/include/tw/playback/twliveplan.h:180-209` — the
   algebra the live lane rests on, and why a master insert breaks it.
3. `smaragd/main/objects/mixer/CONTRACT.md` (inv. 1, 5, 6),
   `smaragd/main/objects/track/CONTRACT.md`,
   `smaragd/main/timeline/CONTRACT.md` (inv. 1, 10, 28),
   `plan/proposed/41_LANE_FRAGMENTS.md` §D3 (why `isPathContainer()` and
   `isLane()` are two questions), `plan/proposed/21_REALTIME_DATAFLOW_INTEGRATION.md`
   D3.

---

## Why

**1. There is no master track, and the object standing in for it cannot grow
one.** The root of an arrangement is an `SStdMixer`: one `twMixer` summing every
top-level track, feeding one `twRewire`
(`main/objects/mixer/src/sstdmixer.cpp:499-518`). That chain has **no plugin
chain and no gain stage**. There is nowhere to put a master limiter, a master EQ
or a master fader, and no amount of UI work creates one — the components are not
there.

**2. The master already has a fader field, and it is dead.** `SStdMixer`
inherits `SObject::volume_` and answers `isLane()` true (proposal 41 D3: "the
root mixer is itself a lane — it carries solo, mute and edit-group state exactly
as a track does"). Nothing applies that volume to anything: the only comment in
`sstdmixer.cpp` that mentions it says "Volume needs no connection: `twTrackMix`
reads `getVolume()` live each buffer" — which is true of a *track's* mix and
false of the master's, because the master's sum is a `twMixer`, not a
`twTrackMix`. **Dragging a master fader today would move a number nobody reads.**
Any design that leaves this field in place has two master faders in it.

**3. There are no send tracks, and no shape for one.** `grep -rni
"sendtrack\|auxsend\|send bus"` over `main/` returns nothing. A send lane needs
exactly what the master needs — a lane with inserts and a fader that is *not* a
place clips go — so building the master's answer twice is the failure mode to
avoid.

**4. Nothing that is not a top-level `STrack` can be a row in the arranger.**
`SStdMixerView::appendRowsFor` (`sstdmixerview.cpp:4358-4381`) iterates
`childLinks()` and `continue`s on anything that is not an `STrack`;
`STrackRow::track` is typed `STrack *`; the head widget is
`SSMVMixerControl( …, STrack & )`. So "represent the master in the arrangement"
is, concretely, "make the master something those three already accept".

---

## The rule this proposal adopts

> **A system lane is an ordinary `STrack` that answers a `systemRole()` other
> than `None`. It is not a new kind of object; it is a track the project owns
> rather than the user, with a policy attached.**

Everything below follows from that one decision, and the reason for it is
measured rather than aesthetic: **`dynamic_cast<STrack *>` appears 98 times
across 40 files** in `main/`. Every one of those sites is a place where a
new lane *type* would have to be audited, and the repo's own history says what
happens when a pair of parallel implementations is introduced and only one half
is later changed (proposal 41 M7: paint and hit-test disagreed on z-order, green
for two milestones; proposal 39 M2: the fader multiply; proposal 37 P6: the
`slots:` access specifier). A system lane that IS an `STrack` inherits, for
free and already gated: the plugin chain and all five plugin verbs, the gain
stage and its automation, level metering, the automation lane UI, the FX strip,
the Track Detail dock, the arranger row, the head, and serialization.

The cost of the rule is a *policy* surface — a track that must refuse things
ordinary tracks accept — and that surface is D6.

---

## Part A — the model

### D1. `SObject::systemRole()`, on the base class, never a `dynamic_cast`

```c++
enum class SSystemRole { None = 0, Master, Send, Conductor };
virtual SSystemRole systemRole() const;   // default None
```

On `SObject` for exactly the reason `contentKind()`, `resolveEventClip()`,
`isMissing()` and `isLaneFragment()` are: the *arranger* has to ask the question
(it pins system rows to the bottom and refuses them as drop targets) and
`app/timeline` may not include `app/objects/track`'s policy headers, and the
*placement service* in `app/model` has to ask it while sitting below
`objects/track` entirely.

Stored as a field on `STrack`, set once at construction and **immutable** — a
lane does not become the master. Serialized as `systemRole='master'` only when
it is not `None`, which is the same non-default-only discipline `editGroup`,
`midiRouting`, `collapsed` and `colorIndex` already follow and what keeps every
existing project file and both goldens byte-unchanged.

`isLaneFragment()`'s own doc comment is the warning to heed here: this must NOT
be spelled as a conjunction of existing predicates. "A lane that is not in
`childLinks()`" happens to describe the master today and would describe nothing
else — an accidental agreement, which is what proposal 41 M0 exists to stop
relying on.

### D2. The master lane is NOT a `childLinks()` member

This is the load-bearing structural decision and it is forced, not chosen.

Tracks are addressed by an **index path from the arrangement root**
(`app/model/sobjectpath.h`; `strackpath.h`: "`[]` is the root, `{2}` its 3rd
child"). Every `.qxa` case, every `trackPath=` attribute in `docs/ACTIONS.md`,
every committed `.qxp` fixture and both goldens are written against the current
indices. A master lane inserted as a child of `SStdMixer` shifts every one of
them by one, and `main/objects/mixer/CONTRACT.md` **inv. 1** ("`getNTracks()`
counts TOP-LEVEL children only — assertions in tests rely on this") would become
false in the same edit.

It also breaks the audio, twice: `SStdMixer::reconnectTracksToMixer()` wires
*every* child into the sum, so a master lane placed there would be summed
alongside the tracks it is supposed to process; and `ssolo::anySoloInTree`
walks the root's `childLinks()` for lanes, so the master would join the solo
set.

So the master lane is an **owned reference link**, exactly the shape
`STrack` already uses for its plugin chain:

- `SStdMixer` holds `SLink *masterLaneRef_`, published through
  `ownedRefLinks()` and **not** through `childLinks()`
  (`strack.cpp:1303`, and the header comment at `strack.h:70-77` explaining why
  publishing it matters for `~SProject`'s survivor ordering — the same trap
  applies here and killed a build once already).
- The lane object is a QObject child of `SProject`, so
  `SProject::serialize()`'s child loop writes it as a top-level element
  (`sproject.cpp:58-64`) — the mechanism `SPluginChain` already uses.
- `<SStdMixer masterLaneId='…'>` references it, and the reference is resolved in
  `SProjectLoader::deferResolve`, because a plain attribute is invisible to the
  loader's `<SLink>`-based dependency ordering (`strack.cpp:1492-1501` says this
  in as many words about `pluginChainId`).

**Consequence to accept rather than discover:** paths do not reach the master.
D9 gives it a name instead.

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

Three properties make this safe:

- **The root component is still the `twRewire`.** `SApplication`'s master meter
  (`sapplication.cpp:135`), `RenderSession`, `AudioEngine` and
  `twSpeaker` all reach the graph through
  `SProject::getRootComponent()->getRootComponent()`. None of them changes, and
  the master meter becomes post-master-FX by construction rather than by a
  second tap.
- **An empty chain and 0 dB are arithmetically identical to today.**
  `twPluginChain::freezePage` forwards to its last insert and, with no inserts,
  forwards its input page verbatim (CLAUDE.md, metering table); `twGainStage`
  "at 0 dB unmuted does no arithmetic at all" (proposal 37 P5). That is the
  same argument P5 used to keep every golden byte-unchanged, and it is why
  **AC2.1 is a `cmp`, not a tolerance.**
- **The master lane's own `twTrackMix` is not in the path.** An `STrack`
  builds `twTrackMix → twPluginChain → twGainStage → twRewire`
  (`strack.cpp:1070-1096`). The master lane's `twTrackMix` has no clips (D6
  forbids them) and its `twRewire` is redundant with the mixer's. M2 therefore
  wires the *mixer's* sum into the *lane's* chain and takes the lane's gain
  stage output back into the *mixer's* rewire, leaving the lane's own head and
  tail components unwired and inert. The alternative — making the master lane's
  rewire the project root — moves the root component and is rejected: it would
  change what every tap above resolves to, for no gain.

### D4. **THE LIVE-MONITORING PRECONDITION IS THE HARD PART OF THIS PROPOSAL**

Read `main/shell/src/slivemonitor.cpp:773-800` before designing anything here.

Live monitoring rests on one algebraic identity:

```
master(unarmed ∪ live) == master(unarmed) + master(live)
```

which holds sample-for-sample **iff the master is a unity sum followed by an
identity map**. `twlive::checkMasterShape` verifies exactly that on every plan
build. A master insert, or a master fader at anything but unity, makes it false
— and today the app's response is to **turn live monitoring off entirely**, with
one log line, because the `Closure` alternative is only half-built: the plan
builder can express it (`sliveplanbuilder.cpp:329-349` already assembles the
master node with the unarmed tracks as `frozenInputs`), but **nothing reads
`twLivePlan::masterLinear`** outside those two files — `twSpeaker` adds the
frozen root page whenever the frozen lane is playing, so a Closure plan would be
summed on top of a root page that already contains those tracks and the user
would hear the arrangement **doubled**.

So: **the first master insert a user drops silently kills their monitoring.**
That is a worse outcome than not shipping the feature, which is why M3 is a
milestone of its own, is sequenced before the UI that makes the situation
reachable (M4/M5), and carries the only gate in this proposal that measures
audio continuity rather than a model fact.

The fix is the one `shell/CONTRACT.md` inv. 18a names: `twSpeaker` must stop
adding the frozen root page while a non-linear plan is live, and the pump — which
already renders the master node when `masterLinear` is false — becomes the sole
producer of the master's audio for the duration. Two things follow that the
milestone must handle rather than assume:

- **The frozen lane must not merely be muted; the demands must keep running.**
  The readahead is what keeps the *unarmed* tracks' pages warm, and under
  Closure those pages are read by the pump as `frozenInputs`. A `twSpeaker` that
  stops the frozen lane rather than stopping its *summing* starves the pump.
- **The transition is a transport-edge and an arm-edge event.** `SLiveMonitor`
  already rebuilds on both (`transportAboutToChange` / `transportChanged`,
  shell inv. 12-18) and already pays a documented one-reposition cost at a
  stopped→playing edge. Flipping between linear and Closure *while playing* —
  which is what dropping an insert on the master during playback is — is a new
  edge and needs the same treatment: measured, not assumed.

### D5. The master fader is the gain stage, and `SStdMixer::volume_` is retired

`twGainStage` is already the fader for every track, already post-FX, already
automatable via `self:Volume`, already ramped at transitions, and already the
one thing `set-track-volume` writes. The master lane owning one means there is
exactly one master fader and it is the same object as every other fader.

`SStdMixer`'s inherited `volume_` is therefore **explicitly retired** as a
master level: M1 asserts (by grep, in the gate) that nothing reads
`SStdMixer::getVolume()`, and the Track Detail / head mounts for the root are
routed to the master lane. Leaving it in place is the "two spellings" defect
this codebase has paid for repeatedly; leaving it *and* adding a gain stage is
that defect with a fader on each.

### D6. Policy: a system lane refuses clips, at ONE seam, and says so

The requester's rule — *a master track must not directly contain sample or MIDI
cuts, but may have child tracks* — becomes:

```c++
virtual bool acceptsClips() const;      // SObject, default true
```

false for every `systemRole() != None` lane, and consulted in **one** place:
the placement destination resolver, `splacements::laneAt()`. That is already
the single seam every placement verb funnels through (`place-clip`,
`move-clip`, `pack-clips`' own lane check, the arranger's `dropEvent`) — proposal
41 M2b widened its *parent* resolution to `containerAt()` while deliberately
leaving `laneAt()`, "the placement DESTINATION resolver", strict. Adding the
check at each verb instead is how the tenth verb ships without it.

Three more refusals, same mechanism, same milestone (M5):

- **No arm.** `arm-track` on a system lane is refused. A master lane that could
  be armed would enter the live closure and be handed to the pump as a
  monitored track, which is a second, contradictory answer to D4.
- **No instrument.** Slot 0 of a system lane's chain may not be an instrument
  (`SPluginChain` already distinguishes them). An instrument on the master
  produces audio from an event feed the master does not have.
- **No structural edit.** `remove-track`, `reparent-track` and `move-track`
  refuse a system lane. It is not the user's object.

Every refusal is **announced** — a message naming the lane and the reason,
reaching the status bar and `TW_LOG` — never a silent no-op. The repo's own
standard: "A bound is ANNOUNCED, never silent" (proposal 38).

Mute and solo need a decision each, and they differ:

- **Mute on the master is legitimate and means "silence the output".** It is
  already exactly what `twGainStage`'s audio mute does, and a `self:Muted`
  automation lane on the master is a usable feature (a hard mute at the end of a
  mix).
- **Solo on the master is meaningless** — `ssolo::isLaneAudible`'s rule is about
  a lane relative to its siblings, and the master has none. Refused, with the
  same message discipline. Note it is not merely useless: the master lane is not
  in `childLinks()`, so `anySoloInTree` cannot see it, and a solo flag set there
  would be a state nothing reads — the `SStdMixer::volume_` defect again.

### D7. Conductor lanes are the *framework* here, not the content

The requester names "child tracks like time or time signature". Those are
**conductor lanes**: `systemRole() == Conductor`, children of the master lane,
`acceptsClips()` false, hidden by default like their parent.

What this proposal builds is the ability for them to *exist, nest, hide, show
and round-trip*. What it deliberately does **not** build is their content, and
the reason is a hard invariant this tree already holds:

> `twTempoMap` is THE tempo authority, and `SProject::getBPMTempo()` is a
> derived view — "tempo is stored as SMF's own unit, µs per quarter (an
> integer), so BPM and the map cannot disagree" (proposal 37 P1). `set-tempo` is
> the ONLY tempo write, and it is an action.

A tempo lane must therefore be a **VIEW of `twTempoMap`**, never a second store
of tempo events, and its edit verbs must funnel into `set-tempo`. Building that
correctly is a proposal of its own (it needs a curve model for ramps, and
`set-tempo`'s re-derivation of every `timebase=beats` link is already the
expensive part). M6 gates the *container*: a conductor lane exists under the
master, is addressable, hides and shows, holds no clips, and contributes no
audio. Anything that would store a tempo number on it is out of scope and the
milestone's ACs say so.

### D8. Hiding is ONE mechanism: `isHidden()`, serialized when true, undoable

```c++
bool isHidden() const;                  // SObject; false by default
```

Serialized `hidden='true'` only when true — the `collapsed` precedent
(`strack.cpp:125`, `1449`). A system lane is *constructed* hidden, so the
attribute is present in the file for it and absent everywhere else.

Toggled by one undoable verb, `set-lane-hidden`, and the menu item **Show system
lanes** is a `SCompositeAction` of that verb over the arrangement's system lanes
— one undo step, the `pack-selection` shape (`objects/mixer/CONTRACT.md` inv. 7).

The alternative considered and rejected: a per-user `SSettings` view toggle
("show system lanes") *alongside* a per-lane serialized flag. That is two
authorities for one question, and proposal 33 D2's split (project state vs.
machine-local state) does not apply — *whether the master lane is on screen* is
a property of the arrangement the user is editing, not of the monitor it is
displayed on, and it should travel with the project the way `collapsed` does.

**Hidden is a VIEW property and never an audio one.** A hidden master lane is
fully in the signal path. Stating that is not pedantry: `isLiveOwnedLane()`,
`ssolo::isLaneAudible` and `STrack::isCollapsed()` are three existing flags that
each look like they might mean "not heard" and do not, and the CONTRACT for this
one has to say which it is.

### D9. Addressing: a reserved path token, because index paths cannot reach it

D2 puts the master outside `childLinks()`, so `strackpath::resolveByPath` cannot
find it. `strackpath::parseQualified` already supports a qualified spelling
(`"Drums:0"` names an arrangement root, proposal 09 D21), so the extension is
additive:

```
"$master"            the master lane of the default arrangement
"Drums:$master"      the master lane of the "Drums" arrangement
"$master,0"          its first conductor child
"$send:Reverb"       a send lane by name (D10)
```

`$` is chosen because it cannot begin a decimal index and cannot begin an
arrangement name in any existing fixture — a bare `master` could collide with a
user's track named "master" in the qualified form. The parse gains one branch;
`spluginaction::chainFor`, `arm-track`, `set-track-volume` and every other
`trackPath=` verb then reach the master **with no change of their own**, which
is the whole return on D1.

### D10. Send lanes: the shape is fixed here, the routing is NOT built

A send lane is a system lane (`systemRole() == Send`) with:

- a plugin chain and a gain stage — identical to the master's, which is the
  point of doing them together;
- `acceptsClips()` false;
- a **name**, because unlike the master there can be several, and D9's
  `$send:<name>` addresses one;
- an output that reaches the master sum.

What this proposal does **not** build, and what the later one must:

- the **send itself** — a per-track, per-destination tap with a level and a
  pre/post-fader choice. That is a new component on the track side
  (`twTrackMix`/`twGainStage` grow an auxiliary output), a new model object, new
  verbs, and a new invalidation edge, and it is at least the size of this
  proposal.
- **feedback prevention.** Send A → send B → send A is a cycle in the audio
  graph and the scheduler's dependency counting will deadlock or spin on it
  rather than fail cleanly. The cycle guard `place-asset` already has
  (`objects/mixer` AC2.6) is the precedent for what is needed and is not
  transferable as written.
- **latency compensation across a send.** PDC is not implemented anywhere
  (proposal 37 P9), so a send through a latency-reporting plugin will be late by
  exactly that amount, as the live lane already is.

M7 is therefore written as *shape only*: a send lane can be created, carries
inserts and a fader, sums into the master, hides and shows, refuses clips, and
round-trips — with **no source able to feed it**. That is deliberately a lane
that does nothing audible yet, and the milestone's ACs say so out loud rather
than implying coverage. It may be executed with this proposal or split out; the
design above is what makes splitting it cheap.

### D11. The arranger: system rows are pinned, and they are not drop targets

`appendRowsFor` gains a second phase, after the recursive walk over
`childLinks()`: the arrangement's system lanes, in a fixed order (master last,
sends above it, conductor lanes as the master's children under the existing
`depth+1` recursion), each appended only when `!isHidden()`.

Four properties, each of which has a specific way of going wrong in this
codebase:

- **`STrackRow::track` stays non-null.** Every consumer of `rows_` assumes it
  (`rowHeightOf`, `drawTakeLane`, `pruneUiState`, the control column). Because a
  system lane IS an `STrack`, none of them change. This is the single largest
  piece of evidence for D1.
- **Reorder is refused, and the refusal is in the model, not the head.**
  `SSMVMixerControl`'s grip strip initiates a drag; the drop resolves through
  `move-track`, which D6 already refuses. The head *additionally* draws no grip
  for a system lane — but the head is cosmetic and the verb is the gate.
- **The system rows are not a drop target for clips.** The `dropEvent` path
  resolves its destination through `laneAt()` (D6), so the refusal is already
  there; what M4 adds is that the drop is not *accepted* visually either, since
  a cursor that promises a gesture the model then refuses is precisely the
  defect `fix/loop-behaviour` (b) fixed for the loop-drag hover.
- **A pinned row must not break `assert-lane-alignment`.** The head spans a
  track's whole lane group (`timeline/CONTRACT.md` inv. 5), and the system rows
  are appended after the user rows, so the group geometry is unchanged for every
  existing case. The gate for that is the existing verb run over a project that
  now has a visible master.

### D12. Colour, metering and automation come for free, and one of them needs a note

The master lane resolves a clip colour like any lane (`app/model/sclipcolors.h`),
meters like any lane (its tap is `getRootComponent()`, which for the master lane
is its own — *unwired* — rewire, **not** the mixer's).

That last clause is a trap, not a detail: after D3 the audio the user hears
leaves the master's **gain stage** and enters the **mixer's** rewire, so a level
probe pointed at the master lane's own rewire would read silence forever
(`twLevelProbe` answers a miss by decaying, so it would look like a broken meter
rather than a wiring bug — proposal 34's table: "A page miss must DECAY the
meter"). M2 must therefore point the master lane's meter at the mixer's rewire,
which is already what the transport master meter reads. **One master meter,
already correct, mounted in a second place** — not a second probe.

---

## Non-goals

- Sends themselves (D10). A send lane with nothing feeding it is the deliverable.
- Tempo, time-signature and marker *content* (D7). The container is.
- Plugin delay compensation (still proposal 37 P9).
- A master-lane **input**: the master is not armable and not recordable (D6).
- Changing `SStdMixer::isLane()`. It stays true; the master *lane* is a
  different object with a different job, and M1's gate is what proves nothing
  reads both for the same question.
- Multi-output routing / hardware output busses. One master per arrangement.
- Retiring `SStdMixer` in favour of a master `STrack` at the root. That is a
  much larger refactor with the same D2 path problem and no user-visible gain.

---

## Traps

Numbered so a milestone can cite one. Each was found by reading the tree, not
by anticipating.

**T1. The Closure refusal (D4).** The first master insert turns off live
monitoring today. M3 or the feature is a regression.

**T2. Index paths shift if the master is a child link (D2).** Every `.qxa`,
every fixture, both goldens, and `objects/mixer` inv. 1.

**T3. `reconnectTracksToMixer` sums every child.** A master lane in
`childLinks()` is summed into the thing it is supposed to process.

**T4. `SStdMixer::volume_` is a live field nothing reads (D5).** Ship a master
fader without retiring it and there are two.

**T5. The master lane's own `twTrackMix` and `twRewire` are inert (D3).** A
future reader will wire one of them "for consistency" and either double the
sum or move the root component.

**T6. The master lane's meter tap is not its own root component (D12).** It
would decay silently.

**T7. `ownedRefLinks()` must publish the master link.** `strack.h:70-77` records
what happened when the plugin-chain link was not published: `~SProject`'s
survivor ordering deleted the chain first and the destructor `removeRef()`'d
freed memory.

**T8. An older build round-tripping the file drops the master's FX.** The lane
is a top-level element referenced by an attribute, so an older loader
instantiates it, links it to nothing, and writes it back — but a *newer* file
opened, saved and returned by an older build keeps the element and loses
nothing; a file whose master lane is *created* by the new build and then edited
by an old one keeps it as an orphan. This is the same exposure `SPluginChain`
has carried since proposal 08 and is accepted, but it must be **stated in the
CONTRACT** rather than discovered.

**T9. `pruneUiState` walks the MODEL to decide which per-track UI keys survive**
(`sstdmixerview.cpp`, proposal 37 P6: there was no pruning before, and a removed
track left a dangling key a later track at the same address inherited). It walks
`childLinks()`. A master lane outside that walk would have its height scale,
fold state and shown-automation set pruned away on every rebuild.

**T10. `SMidiOutPump` reads every track's `eventFeed()`.** A master lane with a
`midiOutPort` set would emit; D6's instrument refusal does not cover this, and
the field is settable by verb. Refuse it in the same place.

**T11. Automation invalidation from a system lane.** A `self:Volume` lane on the
master invalidates through `SObject::invalidateRenderPathRange()`, whose walk
"goes down from the project root through `childLinks()`" — the exact pitfall
`objects/track` inv. 13 records for an `SPluginSlot`, whose fix was to have the
*track* do the walk. The master lane is not in `childLinks()`, so **its
invalidation must be issued by `SStdMixer`**, and the automation and plugin
edits it owns must reach it. Untested, this is silent: the fader moves and the
render does not change (proposal 41's `fragment_nested_edit_reaches_all_placements`
found exactly this shape, and only a RENDERED-audio assertion caught it).

**T12. `render_while_armed` byte-identity.** Proposal 21's gate asserts an armed
render equals an unarmed one. M3 changes what `twSpeaker` does while a live
plan is up; the render path suspends live lanes, so the gate should hold — which
is a prediction and therefore something to check, not to assume.

**T13. `getNTracks()` and `assert-arrangements`.** Anything that counts tracks
must keep counting user tracks. The master is not one.

**T14. Goldens.** Byte-identical is claimed only while the master chain is empty
and its gain is 0 dB (D3). Any milestone that ships a *default* master insert
(none is planned) re-freezes them under a recorded licence, the way B4 and B5
did.

---

## Milestones, acceptance criteria and gates

Every milestone ends green on: `./build.sh`, `python3 tools/check_layering.py`,
`python3 tools/check_logging.py`, `ctest --test-dir smaragd/build -j4
--output-on-failure`, a **reconciled** registered/run/skipped count (measure it;
never quote it), and **byte-identical goldens** (`smaragd/tests/goldens/`,
16-bit PCM, `cmp`) unless the milestone says otherwise.

**Every new gate must be watched FAILING on the pre-fix binary before the fix
lands**, and the PR must say which sabotage bit which assertion. This repo has
shipped a green gate over a broken paint three times (proposal 39 M2, proposal
41 M5, `fix/take-lane-domain`); the rule exists because of that.

### M0 — `systemRole`, `isHidden`, `acceptsClips` on `SObject` (pure refactor)

- **AC0.1** All three exist on `SObject` with the defaults `None` / `false` /
  `true`; nothing overrides them yet.
- **AC0.2** No caller spells any of them as a conjunction of existing predicates
  (proposal 41 D3's rule), and no new `dynamic_cast` is introduced.
- **AC0.3** Zero behaviour change: full suite green, both goldens byte-identical.
- **Gate:** full `ctest`; `cmp` on both goldens; `grep` showing the three
  predicates have exactly one definition each.

### M1 — The master lane exists, is owned, and round-trips (still silent)

- **AC1.1** Every arrangement root owns exactly one master lane: an `STrack`
  with `systemRole() == Master`, reachable through `SStdMixer::masterLane()`,
  published in `ownedRefLinks()` (T7) and **absent from `childLinks()`** (D2).
- **AC1.2** `getNTracks()`, every existing `trackPath`, `assert-arrangements`
  and both goldens are unchanged — the lane is in the file and in no signal path
  (T2, T13).
- **AC1.3** It serializes as a top-level element with
  `<SStdMixer masterLaneId='…'>`, resolved in `deferResolve`, and round-trips
  with its inserts, its gain, its automation lanes and its `hidden` flag.
- **AC1.4** A project written by an older build (no `masterLaneId`) loads and is
  given a fresh master lane; saving it adds the element and changes nothing
  audible. A committed pre-M1 fixture proves it.
- **AC1.5** Nothing reads `SStdMixer::getVolume()` (D5, T4).
- **AC1.6** An existing arrangement's master lane survives `pruneUiState` (T9).
- **Gate:** new `systemlane_test` (ctest) for ownership, serialization,
  teardown order (T7 — construct, save, destroy, under ASAN if available);
  `action_roundtrip_test`; a new qxa `master_lane_roundtrip`; `cmp` on both
  goldens; the AC1.5 grep.

### M2 — The master lane is in the signal path

- **AC2.1** With an empty chain and 0 dB, a render is **byte-identical** to the
  pre-M2 render of the same project — `cmp`, not a tolerance (D3).
- **AC2.2** A gain plugin (`tw.test.clap.gain`) inserted on the master at 2.5×
  is heard: the rendered RMS is 2.5× the pre-insert RMS, to a closed form over
  `tests/test_autosaw.wav`, and it is heard on **both** paths — the offline
  render and the playback capture backend.
- **AC2.3** The master fader is heard, and it is `twGainStage`: a −6 dB master
  fader halves the rendered amplitude, and a `self:Volume` automation ramp on
  the master is heard per second in closed form (the `automation_volume_ramp`
  shape).
- **AC2.4** A master edit **invalidates** (T11): render, then insert a master
  plugin, then render again *in the same process*, and the second render
  reflects it. A case that renders only afterwards cannot see this failure and
  is not the gate (the `fix/take-lane-click-and-slip` lesson).
- **AC2.5** The master meter reads post-master-FX and is ONE probe (D12, T6):
  after AC2.2's insert the transport master meter and the master lane's head
  meter report the same level, and neither decays.
- **AC2.6** The master's own `twTrackMix` and `twRewire` are unwired and
  asserted so, structurally (T5).
- **Gate:** new qxa `master_insert_heard`, `master_fader_heard`,
  `master_edit_invalidates`, `master_meter_postfx`; `cmp` on both goldens for
  AC2.1; `metering_test`.

### M3 — **The `Closure` master mode, wired** (the blocker; T1)

This milestone exists because of `main/shell/CONTRACT.md` inv. 18a and must land
before M4/M5 make a master insert reachable from the UI.

- **AC3.1** `twSpeaker` reads the live plan's `masterLinear` and stops adding
  the frozen root page while a non-linear plan is live; the pump's master node
  is the sole producer for that duration.
- **AC3.2** The unarmed tracks are **not** doubled and **not** lost. Measured
  with `assert-audio-continuity` over a monitored run with a master insert up:
  `maxStep` bounds the "summed twice" failure and `maxGapFrames` bounds the
  "lost" one — the verb's own header says these are two independent failures and
  neither subsumes the other, which is why both are asserted.
- **AC3.3** The readahead keeps running under Closure: the pump's
  `frozenInputs` are served, and `liveOwnedRefusals` and `liveThreadRefusals`
  are **0** (the number every proposal-21 case reports).
- **AC3.4** `SLiveMonitor` no longer refuses a non-linear master: the refusal
  message is absent from the log for a master carrying an insert, and monitoring
  is audible — asserted, not merely un-refused.
- **AC3.5** Flipping linear ↔ Closure **while the transport runs** (drop an
  insert on the master mid-playback, then remove it) costs at most the
  documented one-device-block gap. The house bound for a live lane is **1024
  frames** (proposal 21 L2); anything tighter asserts something the design does
  not offer.
- **AC3.6** `render_while_armed` still produces a byte-identical render (T12).
- **Gate:** `playback_test` gains a Closure case at widths 1/2/6 (no device
  opened, the `twmonitor::pullChannels` precedent); new qxa
  `monitor_master_insert`, `monitor_master_flip_midplay` — both `RUN_SERIAL` at
  `SMARAGD_CAPTURE_SPEED=1` against the paced `file:` input, like every other
  proposal-21 case; `render_while_armed` unchanged and green.
- **Watched failing:** with AC3.1 reverted, `monitor_master_insert` must show
  the doubling through `maxStep` — recorded in the PR with the measured numbers,
  because "it would have been doubled" is a claim, not a measurement.

### M4 — The master lane on screen

- **AC4.1** The master lane is a row in the arranger when shown, pinned below
  every user lane, with a head carrying its fader, meter, mute and FX access
  (D11).
- **AC4.2** It is **hidden by default** and `set-lane-hidden` toggles it as one
  undo step; **Show system lanes** is one composite over all of them (D8).
- **AC4.3** Hidden ≠ silent: with the master lane hidden and a master insert up,
  AC2.2's level assertion still holds (D8's last paragraph).
- **AC4.4** Existing lane geometry is unchanged: `assert-lane-alignment` passes
  over a project with a visible master, and the take/automation sub-lane groups
  of user tracks are unmoved (T9, D11).
- **AC4.5** `pruneUiState` keeps the master lane's UI state across a rebuild
  (T9) — asserted by setting a height scale, forcing a rebuild, and reading it
  back.
- **Gate:** new qxa `master_lane_rows` (row count and order at hidden/shown,
  through the real `rebuildRows`), `master_lane_hidden_still_audible`;
  `assert-track-head` over the master head; `assert-lane-alignment`;
  `action_roundtrip_test` for `set-lane-hidden`.

### M5 — The policies, each refused and each announced

- **AC5.1** A clip dropped on, moved to, or placed on a system lane is REFUSED
  with a message naming the lane; the refusal is in `splacements::laneAt()` and
  reached by `place-clip`, `move-clip`, `place-recording`, `add-sample` and the
  arranger's `dropEvent` **without any of them being edited** (D6).
- **AC5.2** `arm-track`, `remove-track`, `reparent-track`, `move-track`,
  `set-track-solo` and `set-track-midi-output` are refused on a system lane,
  each with its own message (D6, T10).
- **AC5.3** An **instrument** in slot 0 of a system lane's chain is refused;
  an ordinary effect is accepted.
- **AC5.4** Mute IS accepted on the master and is audible (D6), including as a
  `self:Muted` automation lane.
- **AC5.5** Every refusal leaves the model untouched and puts nothing on the
  undo stack (the "an applied-but-empty action would put a no-op on the undo
  stack" rule, `objects/mixer` inv. 7).
- **Gate:** new qxa `master_refuses_clips`, `master_refuses_arm_and_structure`,
  `master_refuses_instrument`, `master_mute_audible`; each refusal watched
  failing by removing its check individually and confirming which assertion
  bites — a single sabotage that fails everything proves nothing.

### M6 — Conductor lanes: the container only

- **AC6.1** A conductor lane exists as a child of the master lane, addressable
  as `$master,0`, hidden by default, refusing clips, contributing no audio, and
  round-tripping.
- **AC6.2** It carries **no tempo state of any kind**. `twTempoMap` remains the
  one authority and `set-tempo` the one write (D7) — asserted by grep in the
  gate, the way the P1 gate asserts `bpmTempo_` has exactly two writers.
- **AC6.3** Nesting it does not change `getNTracks()`, any user `trackPath`, or
  the goldens.
- **Gate:** new qxa `conductor_lane_container`; the AC6.2 grep;
  `action_roundtrip_test`; `cmp` on both goldens.

### M7 — Send lanes: shape only, routing explicitly NOT built

- **AC7.1** A send lane can be created by verb, is named, is a system lane,
  carries a plugin chain and a gain stage, sums into the master, hides and
  shows, refuses clips, and round-trips.
- **AC7.2** It is addressable as `$send:<name>` and the five plugin verbs reach
  it unchanged (D9).
- **AC7.3** **Nothing feeds it.** With no source, a send lane contributes
  silence and both goldens are byte-identical. The milestone asserts the absence
  of routing rather than implying its presence (D10).
- **AC7.4** Two send lanes coexist; names are unique and a collision is refused.
- **Gate:** new qxa `send_lane_shape`, `send_lane_silent`; `cmp` on both
  goldens; `action_roundtrip_test`.
- **Splittable.** If M7 is deferred to its own proposal, M0-M6 stand alone and
  nothing above depends on it.

### M8 — Contracts, docs, and the CLAUDE.md section

- **AC8.1** `main/objects/mixer/CONTRACT.md` gains the master-lane ownership
  invariants (D2, T7, T8); `main/objects/track/CONTRACT.md` gains the system-role
  and policy invariants (D6); `main/timeline/CONTRACT.md` gains the pinned-row
  and hidden≠silent invariants (D11, D8); `main/shell/CONTRACT.md` **inv. 18a is
  rewritten** — it currently says the Closure mode is refused and names this
  proposal as the thing that lands there.
- **AC8.2** `docs/ACTIONS.md` gains `set-lane-hidden`, `create-send-lane` and
  the `$master` / `$send:` path spellings, and every refusal above is documented
  as a refusal.
- **AC8.3** `CLAUDE.md` gains a section in the house style — the "read this
  before touching X, the obvious design is wrong" table — whose first row is
  D4/T1.
- **AC8.4** The PR body says what was gated **and what was not** (the list
  below), names every unreproduced flake, and reports the measured
  registered/run/skipped counts rather than quoting any figure from CLAUDE.md.
- **Gate:** documentation review; `docs/ACTIONS.md` rows verified against the
  registered verbs by `action_roundtrip_test`.

---

## What this proposal will NOT gate

Stated here so a green suite cannot be read as coverage it does not have.

- **Real device latency and jitter** under a Closure-mode live lane. Every
  proposal-21 measurement is against the capture backend, and this one is too.
- **ASIO and WASAPI under load** with a master insert up. The `-j` gate's own
  wall-clock caveats apply (`twlog_test`, `qxa.log_dock_scale`,
  `monitor_latency`): confirm the box is idle before reading a timing failure as
  a regression.
- **Sends** — no source can feed a send lane (D10, AC7.3). Feedback cycles,
  pre/post-fader taps and send-path latency are entirely out of scope.
- **Tempo, time-signature and marker content** (D7, AC6.2).
- **Plugin delay compensation** on the master (proposal 37 P9). A
  latency-reporting master insert is heard late by exactly its reported amount,
  and every mount that shows a latency already says so.
- **Pixel aesthetics** of the master head and row. What is gated is a geometry
  relation and a luminance relation, never a palette — the standing rule since
  proposal 39.
- **The Show-system-lanes MENU ITEM.** There is no testkit verb for a context
  menu anywhere in this repo, so what is gated is the VERB it submits and the
  predicate its enabled state reads. The wiring is hand-verified.
- **An older build's round trip** (T8). The exposure is stated in the CONTRACT
  and not exercised — there is no second build in the gate.
- **A master lane inside a nested arrangement asset.** Arrangements can be
  windowed as assets (proposal 09); what a placed arrangement's master lane does
  inside another arrangement's sum is a real question, it is answered by D3's
  topology (it is simply part of that arrangement's output), and **no case
  exercises it.**
- **More than one arrangement's master lanes at once.** The design is per-root
  throughout; nothing measures two.

---

## Sub-agent assignment

M3 is the milestone that must not be delegated without the reading list at the
top of this document: it is the only one where the failure mode is *audible
doubling* rather than a refused verb, and the only one whose gate is a
continuity measurement. M0/M1 and M8 are mechanical. M5 is a good parallel
track once M0 has landed, because its ACs are independent refusals.
