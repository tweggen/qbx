# Proposal 48 — The mixer pane: one horizontal dock, one channel strip per lane

> **STATUS: M0 AND M1 EXECUTED, 2026-09-07/08; M2-M4 PROPOSED.** Each
> executed milestone below carries an "as executed" section with what was
> measured and what the design did not anticipate — read those before the
> design text they follow. It rests on proposal 45 M4
> (the master lane has a row and a head) and on proposal 47 (send routing),
> both of which ARE executed — 45 M4 at `890fb00d` / `4274da86`, 47 M0-M6 at
> `eaeed762`.
>
> **RENUMBERED FROM 46 TO 48 (revision 3).** It was filed as 46 while
> `46_WINDOW_LAYOUT_PERSISTENCE.md` already held that number, and that one is
> executed and cited as "proposal 46" from CLAUDE.md, `main/shell/CONTRACT.md`
> inv. 59-60, `main/objects/track/CONTRACT.md`, `main/testkit/CONTRACT.md`,
> `docs/ACTIONS.md` and eight source files. Every one of those means the window
> layout; **none of them meant this document**, which is why this is the one
> that moves.
>
> **Revision 3 (2026-09-07) — PROPOSAL 47 LANDED, AND IT DELETES A DECISION.**
> Revision 2 was written on 2026-09-06 against a tree in which the per-track
> send tap did not exist. It does now: 47 M0-M6 built `SSendTap`, the send bus,
> the cycle break, and — M5 — **`SSendStrip`, a working sends UI in the Track
> Detail dock**. So D7's whole subject is gone:
>
> - **D7 said the sends section ANNOUNCES that it is empty.** It no longer can;
>   there are sends to show. D7 becomes an ordinary MOUNT, which is what this
>   proposal's own governing rule asks for anyway.
> - **`SSendStrip` lives in `app/timeline`**, the edge D4 already declares for
>   `SLevelMeter`. No new edge, no second spelling, and the widget is gated
>   (47 M5 sabotaged its `connect()` and five assertions failed).
> - **D5a's narrow mode has a SECOND occupant.** `SSendStrip`'s row is a
>   checkbox + a name label at `setMinimumWidth( 60 )` + a `QDoubleSpinBox` +
>   a `QComboBox` in one `QHBoxLayout` (`ssendstrip.cpp:97-125`) — the same
>   ~230 px layout minimum, against the same 60/96 px strips. The narrow mode
>   is two widgets in two modules, not one.
> - **AC4.5's contract numbers were taken while this sat.** 47 M5 claimed
>   `main/timeline/CONTRACT.md` **inv. 63** and moved the file's numbering note
>   to "currently 64". This proposal takes **64 and 65**.
>
> The Why table, the prerequisite reading, D5's diagram and the Non-goals each
> carried the same false statement and are corrected in place. **Nothing
> structural moved**: D1, D6a, D11b, D12, D13 and T11 are untouched by 47.
>
> **Revision 2** after an adversarial review against the tree (2026-09-06).
> **Six claims in revision 1 were WRONG**; each is corrected in place rather
> than quietly dropped, because each was the reason for a decision:
>
> - **D1/D3 put both extractions in `app/model`.** Neither can live there.
>   `STrack` is defined two layers up (`main/objects/track/.../strack.h`) and
>   each layer publishes only its own include dirs, so an `app/model` walk
>   cannot name `STrack` at all; and `SAppContext` exposes **no** submit and no
>   undo stack, so a helper down there cannot commit anything. Both move to
>   `app/objects/mixer`, and the SUBMIT half does not move at all (D3).
> - **D4 said mixerui needs no `app/timeline` edge.** `SLevelMeter` — the
>   widget the strip is built around — is `app/timeline/slevelmeter.h`. The
>   edge is declared, and with it **AC0.4's `sfadercurve.h` move is deleted as
>   pure churn**: its only includers are three timeline files.
> - **D6 pinned a master strip that a default project does not have.**
>   `SObject::laneHiddenByDefault()` returns TRUE for every system role
>   (`sobject.h:781`) and `appendSystemRows` returns early on it
>   (`sstdmixerview.cpp:4490`). D1 omitting hidden lanes + D2 honouring hidden
>   = a mixer with **no master strip until the user un-hides the arranger
>   lane**. D6a is the exemption.
> - **D5 said `SPluginEffectStrip` mounts "unchanged".** Its rows carry
>   Edit(70) + Reload(80) + Remove(80) px of buttons beside a name label
>   (`splugineffectstrip.cpp:335-350`), a layout minimum well over 200 px
>   against AC1.7's 60/96 px strips. D5a.
> - **D9's pruning inventory was stale.** The FOLD set left `pruneUiState` when
>   fold moved onto `STrack::isCollapsed()` to be saved with the project
>   (`sautomationlane.cpp:795-810`). That also breaks **D2's argument** — both
>   fold and hidden are model state now — without breaking its conclusion.
> - **AC0.3 said "seven call sites".** `SSMVMixerControl` has ONE
>   (`toggleTargets()`); `sstdmixerview.cpp` has eleven more, most wrapped in
>   `pruneNestedTargets()`, which revision 1's API omitted entirely.
>
> Three omissions of the same severity are now D12, D13 and D11's extension:
> the ARRANGEMENT ROOT, the project-close detach seam, and the automation
> recorder. Each is a bug class this repository has already paid for once.

Prerequisite reading, in this order. The first three each contain a sentence
that invalidates the obvious design:

1. `main/timeline/CONTRACT.md` **inv. 45** — "THE TRACK DETAIL PANEL SCROLLS; IT
   NEVER COMPRESSES". The bug it records (`qSmartMinSize()` REPLACES a
   layout-derived minimum with an explicit one rather than taking the larger)
   is the single most likely way this proposal ships broken, because a pane of
   N strips in a short dock is that same situation N times over.
2. `plan/proposed/34_LEVEL_METERS.md` and the CLAUDE.md metering table — the tap
   is a track's ROOT component, read BY POSITION, and **a page miss must DECAY**.
   A mixer is the first mount that will have thirty of them at once.
3. `plan/proposed/45_SYSTEM_LANES.md` **D10, D11, D12** — the master lane's
   place in the lane order, and why its meter tap is `getRootComponent()`.
   (D10's "a send lane exists and nothing can feed it" is CLOSED by 47; read
   it for the lane ORDER, not for the state of routing.)
4. `plan/proposed/47_SEND_ROUTING.md` **D2, D3, D9** and its **M5 as
   executed** — the tap on `SObject`, pre = post-FX/pre-fader against post =
   post-fader, the fact that **a live-owned source contributes nothing to a
   send** (which this pane's meter must not contradict), and the rules
   `SSendStrip` already implements: a row per send LANE rather than per tap,
   unticking DISABLES rather than removes, and a lane's own strip offers every
   other lane and no row for itself.
5. `main/mediabrowser/CONTRACT.md` — the precedent for a new app_ui module that
   is "the DOCK and nothing else", including how it stays out of `app/timeline`.
6. `main/pluginui/CONTRACT.md` invariant 3 (every model mutation is an ACTION)
   and the generic editor's **module-level registry keyed by slot** (the
   `fix/editor-ui-and-shortcuts` lesson).

---

## Why

**1. There is no mixer.** The only channel strip in this application is
`SSMVMixerControl` — the arranger's track HEAD, 1761 lines, in a 120 px column
beside a lane. It is wide-and-short by construction, which costs it three
things a mixer needs and it cannot have: its meter is capped at **two lanes**
(`SLevelMeter::MONITOR_LANES`, and proposal 34 measured the column as having
~13 px of slack), its buttons drop out under a **density ladder**
(`Full → Compact → Tiny`) as the lane gets shorter, and it shows **no inserts
at all**. The FX chain is reachable only through the Track Detail dock, which
shows exactly **one** track — the one that is selected.

So the question "what is on track 7, and is it louder than track 3" cannot be
answered on one screen today, at any zoom.

**2. Everything the strip is made of already exists and is gated.**
`SLevelMeter` already takes an orientation and already has a Grow mode for
showing every channel (`setGrowWithLanes`, proposal 36 B8).
`SPluginEffectStrip` already renders an ordered insert list with bypass, edit,
reload, remove, add and drag-reorder, every one of them through an action.
`SApplication::meterTick` is already the one main-thread pump that keeps
running at a static position. Proposal 45 M4 already made the master a real
`STrack` with a head. **This proposal is composition, not capability** — and
that is the reason it is worth doing now rather than after the routing work
below.

**3. What is NOT there, measured rather than assumed.** Three things a
reference mixer has that this application does not, each of which would be a
proposal of its own:

| Missing | Evidence | Consequence for this proposal |
|---|---|---|
| ~~**Sends**~~ **— CLOSED by proposal 47, 2026-09-06** | Revision 2 measured `grep -rni "sendtrack\|auxsend\|send bus"` over `main/` returning nothing. It now returns `SSendTap`, `rewireSendBuses`, `wireAsSendLane` and `SSendStrip`: 47 built the tap, the bus, the cycle break and a working UI. | D7 is no longer an announcement. The pane MOUNTS `SSendStrip`. |
| **Track pan** | `SObject::pan_` exists and is written only by `set-clip-pan` (clip properties). `grep -i pan` over `tw303a/mix/include` finds one comment and no law. CLAUDE.md: "`self:Pan` is still absent — what is missing is the pan itself". | D8: **no pan control.** A knob that moves and is never heard is worse than no knob. |
| **Pre-fader metering** | Proposal 34: the tap is the track's root component, which is post-fader, post-FX, pre-summing. "a pre-fader meter is not available without new engine work". | The strip's meter is post-fader and the tooltip says so. |

---

## The rule this proposal adopts

> **The mixer is a second MOUNT of rules the arranger already owns — never a
> second COPY of them.** Lane order, the selection broadcast, the solo/mute
> audibility rule, the fader curve, the meter's lane cap and the clip palette
> each have exactly one spelling in this tree, and this proposal adds a mount,
> not a spelling.

This repository has paid for that rule four times already — the meter and the
ear disagreeing about a nested lane (`timeline` inv. 10), paint and hit-test
disagreeing about z-order (inv. 32, shipped green for two milestones), the
loop-marker geometry (`fix/loop-behaviour`), and the take-lane body fill
(inv. 35). Each was two implementations of one rule that drifted. **M0 exists
solely to make the two shared rules shareable before there is a second mount of
them**, exactly as proposal 45's M0 split `isPathContainer()` from `isLane()`
before a second container kind existed.

---

## Part A — the shared seams (M0)

### D1. The lane ORDER is one walk: `slaneorder::flattenTrackLanes()`

Today the flattened depth-first lane list is `SStdMixerView::appendRowsFor` /
`appendSystemRows` / `rebuildRows`, producing `QVector<STrackRow>` — *track*
lanes interleaved with take and automation SUB-lanes, each carrying a pixel
height. A mixer wants the track lanes and nothing else.

**The walk goes in `app/objects/mixer`, NOT `app/model`.** It has to name
`STrack` (the arranger's own walk `dynamic_cast`s to it,
`sstdmixerview.cpp:4374`), and `strack.h` is two layers above `app/model`,
whose include dirs a lower layer cannot see. `objects/mixer` already declares
`objects/track` (`check_layering.py:156`), and `objects/cut` and
`objects/fragment` besides — it is the lowest layer that can express the type.

```
namespace slaneorder {
    struct Lane { STrack *track; SLink *link; SObject *parent; int depth;
                  bool hasChildren; SSystemRole role; };
    // Depth-first over childLinks(), then the system-lane tail in proposal 45
    // D11's fixed order (sends, then master last). FOLD state is NOT consulted
    // (D2); laneHidden() is (D2), EXCEPT for the master (D6a).
    QVector<Lane> flattenTrackLanes( SObject *root, bool honourHidden );
}
```

`rebuildRows` consumes it and adds, per lane, its fold decision, its sub-lanes
and its height — the arranger's business, which stays there. The mixer consumes
it raw.

**`collectTracks` rides the same extraction.** `sautomationlane.cpp:122-147` is
a THIRD model walk (the prune sweep's), and the absence of the system-lane tail
from it was itself a proposal-45 defect. Leaving it out of M0 would leave the
tree with three walks after a milestone whose entire purpose is to have one.

**Why this and not "the mixer walks the model itself":** two walks over a tree
with folders, hidden lanes and a system-lane tail is two places to forget the
tail. 45 D11 lists four specific ways that phase goes wrong; a second
implementation earns its own four.

**Risk, stated:** `rebuildRows` is among the most heavily gated functions in the
app. M0's acceptance criterion is therefore the **full suite green with no case
edited**, plus a unit assertion over a fixture holding folders, a collapsed
folder, a hidden lane and the master.

### D2. The mixer does NOT honour the arranger's FOLD state

A folder collapsed in the arranger hides its children's lanes; in the mixer the
children keep their strips. REAPER's MCP and the TCP have independent
visibility; Pro Tools shares Hide and does not share folder collapse.

**The reason is NOT "fold is view state and hidden is model state" — both are
model state.** Fold moved onto `STrack::isCollapsed()` so it could be saved
with the project (`sautomationlane.cpp:795-798`), and `laneHidden()` is model
state because the toggle had to be one undo step (`sobject.h:775-784`). The
distinction that survives is what each one is ABOUT: fold answers "show this
lane's children as ROWS", and a mixer has no rows; hidden answers "show this
track at all", which both mounts must agree on.

**Consequence to know rather than rediscover:** the strip count therefore does
not match the arranger's visible row count, and no gate may assert that it does.

### D3. The selection TARGETS are one spelling; the SUBMIT stays at the mount

`SSMVMixerControl` commits mute, solo, arm, edit-group, track input, monitor
mode and automation mode **over the selection, as one undo macro**, through
`toggleTargets()` → `smv_.selectionTargets( &tk_ )` — "only a gesture aimed INTO
the selection broadcasts" (`sstdmixerview.cpp:1334-1345`). A mixer strip that
toggled only the strip clicked would be a second rule, and the user would find
out which one they touched by undoing.

**What extracts** (into `app/objects/mixer/.../strackbroadcast.h`):

```
namespace strackbroadcast {
    QList<STrack *> targetsFor( SStdMixer *, STrack *clicked );
    QList<STrack *> orderByLane( SStdMixer *, const QList<STrack *> & );
    QList<STrack *> pruneNestedTargets( const QList<STrack *> & );   // structural verbs
}
```

**What does NOT extract: the submit.** Every existing commit goes through
`stimeline::submitActive` (`ssubmit.h:29`) or builds a macro on
`SApplication::app().actionHistory()->undoStack()`
(`ssmvmixercontrol.cpp:411-420`), and `objects/mixer` can reach neither —
`SAppContext` exposes no submit and no undo stack, which is the deliberate
minimalism `docs/ARCHITECTURE.md` asks for. A `submitOverSelection` down there
would mean widening `SAppContext` for one convenience. **The macro shape is
three lines at each mount and is duplicated deliberately**; what may never be
duplicated is the target COMPUTATION, which is where the rule lives.

**`selectionTargets` becomes a one-line delegating member, not a deletion.** It
has twelve call sites, eleven of them in `sstdmixerview.cpp` (1097, 1207, 1235,
1563, 1631, 1668, 1687, 1737, 2944, 4798, 4830) driving remove / indent /
outdent / group / ungroup / asset / drag — the structure-verb family the mixer
never mounts. Migrating all of them buys nothing and churns gated code; one
spelling behind a delegating member is the same guarantee at a tenth of the
risk. (Revision 1 demanded "gone, not wrapped"; that was cost with no property
attached.)

### D3a. `orderByLane` has a behaviour the extraction must not silently change

Today it sorts by **row index** and sends tracks with no visible row LAST —
"Tracks with no visible lane sort last, stably" (`sstdmixerview.cpp:1304-1316`).
D1's walk ignores fold, so a selected child of a collapsed folder has no row
today and would move from the tail to its tree position. That order is
load-bearing for `newTrackReference_()` (`sel.last()`, line 1416) and for the
top-down/bottom-up loops in indent and outdent.

**Either preserve it or change it consciously, and assert which in a unit
test.** Nothing in the qxa suite covers a multi-selection spanning a collapsed
folder, so this is a change the suite would not catch.

---

## Part B — the pane

### D4. A new module, `main/mixerui` (app_ui) — the dock and nothing else

The mediabrowser precedent, for the same reason: the pane is a UI slice with no
model of its own, and putting it in `app/timeline` would add its strips to a
6506-line file whose head widget is already 1761 lines.

Declared edges in `tools/check_layering.py`:

```
'mixerui': {'actions', 'model', 'objects/mixer', 'objects/track',
            'pluginui', 'selection', 'servicesui', 'shell', 'timeline'}
'shell':   + 'mixerui'
'testkit': + 'mixerui'
```

**The `timeline` edge is not optional and revision 1 was wrong to avoid it.**
`SLevelMeter` — the widget the whole strip is built around — is
`app/timeline/slevelmeter.h`. The alternatives are worse: moving `SLevelMeter`
down drags `tw/metering` into a lower layer's engine deps, and re-implementing
it is the second copy this proposal exists to prevent. The mediabrowser's
"no timeline" rule fits a browser that must not know what an arranger looks
like; it does not fit a module whose entire content is the arranger's widgets
mounted differently.

**Revision 3: that one edge now carries the sends section too.** `SSendStrip`
(47 M5) is `app/timeline/ssendstrip.h`, so D7's mount costs no new edge. Worth
noting because it is the second time this edge has paid for itself, and it is
the argument against anyone re-proposing a `timeline`-free `mixerui`.

**And with that edge, AC0.4's `sfadercurve.h` move is deleted.** Its only
includers are three `app/timeline` files. Moving it would be churn bought for a
constraint that no longer exists.

The `servicesui` edge is there because option KEY NAMES live in `SOpt`
(`check_layering.py` gives that as the reason for the `mediabrowser` and
`eventui` edges: "a second spelling of a key is how a setting silently stops
round-tripping"). `Mixer/sections` is an SOpt key, not a raw `SSettings` write.

Engine deps: `_ENG_BASE | {'metering', 'pages'}` — `graph` is already in
`_ENG_BASE`, and this is deliberately smaller than `timeline`'s
`{devices, events, metering, pages, playback, sources}`.

### D5. The strip, top to bottom

Pro Tools' Mix window and REAPER's MCP agree on the order, and the requester
confirmed it against them:

```
┌──────────────┐
│  Drums    ▾  │   name (editable), colour chip, narrow/wide toggle
├──────────────┤
│ FX  Comp   ▫ │
│ FX  EQ     ▫ │   INSERTS  — see D5a
│ +            │
├──────────────┤
│ S  Reverb -6 │   SENDS  — SSendStrip, mounted (D7)
├──────────────┤   ← scroll ends here
│ M  S  R      │
│ ▮▮   ┃       │   METER + FADER, pinned, never scrolled
│ ▮▮   ┃       │
│  -6.0 dB     │
└──────────────┘
```

**The fader/meter/M-S-R block sits OUTSIDE the strip's scroll area**, which is
`STrackDetailPanel`'s own rule and its own words: "the fader and the meter are
what a user looks at while the transport runs, so they stay put however far the
FX content above them is scrolled". Everything above it scrolls.

**M/S/R sit BESIDE the fader**, not above it — the requester's wording, and it
is also what makes the block a fixed height independent of the button count.

**No widget in the strip that carries a layout sets an explicit minimum
height.** T1.

### D5a. The inserts AND sends sections need a NARROW mode, which does not exist yet

Revision 1 said `SPluginEffectStrip` mounts "unchanged". It cannot: each row is
a name label + a latency badge + a bypass box + **Edit (≤70 px) + Reload (≤80)
+ Remove (≤80)** in one `QHBoxLayout` (`splugineffectstrip.cpp:273-350`), plus
"+ Add Effect" at ≤200. Its layout-derived minimum width is comfortably past
200 px, against AC1.7's 60 and 96 px strips. Mounting it as-is means AC1.7
fails or the strips are 250 px wide, which is not a mixer.

Two answers, and this proposal picks the first:

1. **A narrow mode ON `SPluginEffectStrip`**, in `app/pluginui`: below a width
   threshold the row collapses to name + bypass, with Edit on double-click
   (already the row's own gesture) and Reload/Remove on its context menu. One
   widget, one registry, one set of verbs — and the Track Detail dock gets the
   same behaviour when the left column is dragged narrow, which is a defect it
   has today.
2. A separate compact list in `mixerui` reusing the strip's verbs. Rejected:
   it is a second insert UI, and the editor registry, the drop handling and the
   Missing/Unsupported states would each be re-implemented.

**This is real work in a module this proposal otherwise only mounts**, and it
belongs to M1, not to a polish milestone — AC1.7 cannot pass without it.

**Revision 3: `SSendStrip` (47 M5) has the identical problem, and it is a
SECOND widget in a SECOND module.** Its row is a checkbox + a name label at
`setMinimumWidth( 60 )` + a `QDoubleSpinBox` + a `QComboBox` in one
`QHBoxLayout` (`ssendstrip.cpp:97-125`) — the same ~230 px layout minimum
against the same 60 and 96 px strips, reached by a different route. Its narrow
mode collapses a row to the enable box + an elided lane name, with the level
and the pre/post choice on the row's context menu.

So M1 carries a narrow mode in **`app/pluginui`** and one in **`app/timeline`**,
and the cost table below says so. The alternative — one narrow mode and one
section quietly dropped from a narrow strip — is worse than it sounds: a user
who narrows a strip and loses the ability to SEE that a track feeds the reverb
has lost the answer to the question the pane exists for.

**And the Track Detail dock gets both**, which is the same argument as before:
47 M5 mounted `SSendStrip` in a dock whose left column can be dragged narrow,
so the defect is already there and is fixed for free.

### D6. The master strip is pinned at the right and does not scroll

D1's walk puts sends then the master at the tail (45 D11). In a horizontal pane
the tail is the right-hand end, which is where both reference DAWs put the
master. It goes in a **separate, non-scrolling** container beside the scroll
area, so it is on screen at every scroll position — Pro Tools' behaviour, and
the one strip a user needs while looking at any other.

Its meter needs no special case: 45 D12 already overrode
`STrack::getRootComponent()` for the master role to return the mixer's rewire,
"one answer, at the one place every consumer already asks" (verified,
`strack.cpp:480-505`).

### D6a. **THE MASTER STRIP IS EXEMPT FROM `laneHidden()`**, and without this
### decision a default project has no master strip at all

`SObject::laneHiddenByDefault()` returns **true for every system role**
(`sobject.h:781-784`) and `appendSystemRows` returns early on a hidden master
(`sstdmixerview.cpp:4489-4490`). So on a project nobody has touched, the master
LANE is not in the arranger — which is the arranger's correct default, since a
master row costs vertical space in a list of lanes and most sessions never need
one.

Compose that with D1 (hidden lanes omitted) and D2 (both mounts honour hidden)
and revision 1 shipped **a mixer whose master strip appears only after the user
un-hides a row in a different view**. AC1.2 as written would have gated that
behaviour in.

**The decision: `laneHidden()` governs the ARRANGER's row. The mixer honours it
for user tracks and shows the master unconditionally.** That is what Pro Tools
does — the Mix window shows the master whatever the track list says — and it
follows from what the two surfaces ARE: hiding a lane is about the arrangement's
row list, and a mixer without its summing point is not a mixer.

This is the one place where the two mounts deliberately disagree about a model
flag, so it is stated here, in `mixerui/CONTRACT.md`, and in the strip's own
tooltip. **The M1 fixture must be a project that has never set
`set-lane-hidden`** — a fixture that un-hides the master first would pass
whether this decision was implemented or not, which is exactly the shape of gate
this repository has shipped three times.

### D7. The sends section MOUNTS `SSendStrip` — REWRITTEN in revision 3

**What this section said, and why it is worth keeping the corpse visible.**
Revision 2 decided that the section would show one disabled row reading
"no sends (routing not implemented)", on the rule that a bound is ANNOUNCED
rather than silent (proposal 38), and because the strip's geometry and its
layout gate are easier to get right once with the section present than to
retrofit. That was the correct decision **against the tree it was written
against**, and it survived exactly one day: proposal 47 executed M0-M6 on
2026-09-06 and there are now sends to show.

**The decision now: mount `SSendStrip`, and add nothing.** It is
`app/timeline/ssendstrip.h`, reachable over the edge D4 already declares for
`SLevelMeter`, and it already implements the four rules a mixer would
otherwise have to re-decide — every one of them gated by 47 M5, which sabotaged
its `connect()` and watched five assertions fail:

| Rule 47 M5 already settled | Why the mixer must not re-decide it |
|---|---|
| A row per send LANE, not per tap — ticking one CREATES the tap | "There is a Reverb bus and this track does not feed it" must be VISIBLE, not inferred from an absence. A per-tap list makes an unfed lane invisible in exactly the mount where comparing tracks is the point. |
| Unticking DISABLES; it does not remove | `SSendTap::enabled` exists for this. A level and a pre/post choice must survive being switched off, or a user toggling a send off and on finds it back at 0 dB post. |
| A lane's own strip offers every OTHER lane and no row for itself | The verb refuses a self-send anyway; a control that exists only to be rejected is worse than one that is not offered. Load-bearing here, because D1's walk gives a SEND lane its own strip. |
| Every change commits through 47 M0's ordinary verbs | This proposal's governing rule, and D11's. |

**What the mixer adds is the narrow mode (D5a) and nothing else.** In
particular it does NOT add a plus button, a routing matrix, or a second way to
create a send lane — `add-send-lane` is proposal 45's verb and the arranger
owns that gesture.

**One thing to announce, and it is 47's, not this pane's.** 47 D9 decided that
**a live-owned source contributes nothing to a send** — an armed, monitored
track's send is silent, accepted rather than worked around. A mixer is the
first surface on which a user sees a send row lit and a send lane's meter dark
at the same time, so the send lane's strip carries that in its tooltip. This is
D11b's problem wearing a different hat: the pane must not invent an explanation
for a silence the engine already has a documented reason for.

### D8. There is no pan control, and the strip says why

Per the table above. The strip's context menu carries a disabled "Pan
(not implemented)" entry with the same tooltip discipline as D7. **Do not add a
knob wired to `SObject::setPan()`** — the value would be stored, serialized,
undoable and inaudible, which is the worst of the four available outcomes.

### D9. Collapse is TWO mechanisms and they are not the same

| | Pane-wide SECTION toggles | Per-strip NARROW/WIDE |
|---|---|---|
| Where | a toolbar row on the pane: `Inserts` `Sends` `Meter` `Fader` | one button in each strip's header |
| Scope | every strip | that strip |
| Stored | an `SOpt` key, `Mixer/sections` (a bitmask) — never a raw `SSettings` write, because SOpt owns key NAMES (D4) | per-track UI state on the pane, pruned — see below |
| Undoable | **No.** A preference is not an edit to the arrangement — the `set-count-in` / `set-pre-roll` precedent | No, same reason |

**The per-strip state must join THE pruning walk, not add a second one.**
`SStdMixerView::pruneUiState` (`sautomationlane.cpp:799-810`) is the one walk
over the per-track UI-state sets — today `takesExpanded_`, `trackScale_` and the
shown-automation set — and it exists because before proposal 37 P6 there was
none: "a removed track leaves a dangling key that a later track allocated at the
same address would inherit". **The fold set is no longer among them**: fold
moved onto `STrack::isCollapsed()` to be saved with the project, and being an
ordinary object attribute it dies with the object.

That is the cheaper answer and the pane should prefer it: **if the narrow flag
can live on the track, it needs no pruning at all.** It cannot, because narrow
is a per-USER view preference (D9's own table) and `STrack` attributes are
serialized with the arrangement. So the pane keeps a set keyed by `STrack *`,
prunes it in the same shape over the MODEL, and — per D13 — holds nothing that
outlives a project.

**Narrow mode is not a density ladder.** The head's `Full/Compact/Tiny` ladder
exists because a lane's HEIGHT is imposed on it by the arrangement. A mixer
strip's width is chosen by the user, so narrow/wide is a two-state user choice:
narrow hides the name field's text beyond an elision, the inserts and the
sends, keeping M/S/R, the meter and the fader.

### D10. The dock, and the button that shows it

The **ninth** dock. Created in `SMainWindow`'s constructor with
`objectName = "dock_mixer"` — because `restoreState()` can only place docks that
already exist (shell CONTRACT inv. 4) — added to `Qt::BottomDockWidgetArea`,
**hidden on a first run**, and thereafter riding entirely on the opaque
`ui/windowState` blob like every other dock. No settings key of its own for
visibility.

Three ways to reach it, all the same `toggleViewAction()`: the View menu (where
the other eight are), a toolbar button on the transport bar, and
`Ctrl/Cmd+Shift+M`. The shortcut is window-scoped; **check it against the event
editor and the virtual keyboard first** — bare `Q` was shadowed exactly this way
(`fix/editor-ui-and-shortcuts`), and the fix there was
`Qt::WidgetWithChildrenShortcut`.

### D11. Every mutation is an ACTION, and a hidden pane does no work

- Mute, solo, arm, volume, name, insert add/remove/reorder/bypass: existing
  verbs, through D3, one macro per gesture. `pluginui` inv. 3. A fader DRAG is
  one undo step for free — `SSetTrackVolumeAction::mergeWith` coalesces on the
  path key (`ssettrackvolumeaction.cpp:70-77`) — and the redirect that turns a
  volume write into an automation POINT on a Read-family lane is INSIDE the
  action, so the mixer inherits it with no code.
- The pane's `meterTick` handler **returns immediately when the dock is
  hidden** — the Feel Flow puppet's rule ("a HIDDEN dock does no work — not
  even the model walk"), which matters more here because the walk is per strip.
- A strip scrolled out of the viewport still ticks (its meter is 8 px of paint
  and stopping it would freeze a bar the user scrolls back to), but a **page
  miss idles it**, never holds. Proposal 34: "A page miss must DECAY the
  meter." This is what every visible arranger head already does, one probe per
  tick, so N strips cost what N heads cost.
- Double-click resets a value control to its default, through
  `sdefaultreset::onDoubleClick` — the fader to **exactly 0.0 dB** via a direct
  dB commit, because the integer fader's curve does not round-trip
  (`sDbToFader(0.0)` is tick −191 and back is +0.0625 dB).

### D11a. The fader is a THIRD automation control, and two of its three
### obligations are UI-side

`SAutomationRecorder` (proposal 37 P6) is fed by the arranger fader AND the
plugin parameter slider, and neither obligation is inside the action:

| Obligation | Where it lives today | What a mixer fader that skips it does |
|---|---|---|
| Offer each drag tick to the recorder and return if taken | `ssmvmixercontrol.cpp:107-120` | Submits an action **per slider tick** during a Touch/Latch/Write pass — thirty undo entries a second, the precise thing P6 exists to prevent |
| Pump the READ value from `meterTick` while a Read-family lane exists | `pumpReadValue`, `ssmvmixercontrol.cpp:1738-1762` | Freezes at the last hand value while the arranger's fader follows the curve — two faders on one track disagreeing on screen |
| Redirect a write to an automation POINT | inside `SSetTrackVolumeAction` | nothing; inherited |

The mixer fader does all three. The double-click reset must also route through
the recorder while a pass is open, for the same reason the ordinary drag does.

### D11b. The meter reads AUDIBILITY and the LIVE path, not the track's flags

The head's meter tick is not "probe and draw". It idles on
`!ssolo::isLaneAudible(...)` — which `main/timeline/CONTRACT.md` inv. 10 records
MUST NOT be re-derived locally, because two local copies of the
direct-children-only rule are exactly how the meter and the ear came to
disagree about a nested lane — and it has a whole branch reporting the pre-FX
INPUT level for a monitored track, because a live-owned lane has **no frozen
pages to read** (`ssmvmixercontrol.cpp:1195-1223`).

A mixer strip that just runs a probe therefore shows a **dead meter on every
armed or monitored track** and a **lit meter on a solo-muted nested lane**. Both
are the shipped bug inv. 10 exists to prevent. The strip calls the same two
rules.

### D12. WHICH arrangement — the pane follows the active tab and stamps its root

Selection is per-`SStdMixer` (`sstdmixer.h:147-149`), every action carries a
`pathRoot` (`saction.h:70-71`), and `stimeline::submitActive` stamps the
**active tab's** root onto whatever it submits (`ssubmit.cpp:24-35`). A pane
that shows the master arrangement while the user works in a second tab would
broadcast over the wrong selection and resolve its paths against the wrong tree.

**The pane follows `SViewTabs`' active root** and stamps that root on every
action it submits — never `submitActive` blind, and never the master root by
default. This repository has already shipped the wrong-root bug once:
`SClearSelectionAction` honours `pathRoot_` and the convenience helper does not
set one, so Ctrl+Shift+A cleared the master's selection from any tab
(`fix/editor-ui-and-shortcuts`).

A known narrowing inherited rather than introduced: `SPluginEffectStrip`
resolves against the master root only (`splugineffectstrip.cpp:197-209`). The
mixer does not fix that here; it is named so it is not later discovered as this
proposal's doing.

### D13. Project close and open: the pane joins the detach seam

`closeProject()` clears the undo stack (releasing the track pins that
`SRemoveTrackAction` holds), calls `destroyDocksToolbars()` — which explicitly
detaches the three track-holding panels, `detachTrackDetail` /
`detachClipProperties` / `detachEventEditor` (`smainwindow.cpp:871-899`) — and
then deletes the project.

A pane holding one `STrack *` and one `twLevelProbe` per strip, not on that
list, **dereferences freed tracks on the next 33 ms tick**. That is a crash, not
a glitch, and it is the same class as the `SCut` revalidation UAF and the
`SViewTabs` dangling-root hazard, both of which this tree already fixed by
wiring lifetime from the first commit rather than later.

The pane's `detachProject()` joins the list, drops every strip and every probe,
and the strips hold their tracks as `QPointer`. **A teardown gate is judged by
EXIT CODE, not by an assertion** — the `plugin_native_editor_teardown_safe`
precedent, where the crash is the assertion.

---

## Traps

Each of these has already happened once in this tree, at the file named.

**T1. `qSmartMinSize()` REPLACES the layout minimum.** `main/timeline/CONTRACT.md`
inv. 45. A 100 px floor on a section needing 450 told the dock it fitted, and a
`QBoxLayout` handed less than its minimum does not refuse — it distributes the
shortfall and the children overlap. The pane is this situation once per strip
plus once for the pane. **The gate is a GEOMETRY gate** (crushed/overlap counts
from `sAuditLayout`), watched failing at a short dock height, because a
screenshot proves nothing under `QT_QPA_PLATFORM=offscreen`.

**T2. A never-shown widget receives no resize event**, so its layout never
runs. `sSettleLayout()` (`WA_DontShowOnScreen` + `show()` + three layout
passes) is the only way to measure one. The tell that you got this wrong is
**identical counts at two different sizes**.

**T3. Deleting the widget under the hand.** The head's rebuild uses
`deleteLater` and says why. A pane that rebuilds every strip on every model
signal will delete the fader mid-drag; rebuild on STRUCTURE changes only
(tracks added/removed/reordered/hidden), and let per-track signals update the
strip in place.

**T4. The generic plugin editor's registry is keyed by SLOT, module-level.**
`SPluginEffectStrip::isGenericEditorOpenFor()`. A second mount of the strip
must reuse it and must not mint a duplicate editor for the same slot — that was
the shipped bug in `fix/editor-ui-and-shortcuts`, where the registry lived on
the strip and a rebuilt strip made a second window.

**T5. The meter's lane rules.** It reports `min(wantLanes, page->channels())` —
the width of the PAGE IN HAND, never the tap's declared width — and a cached
page whose width no longer matches its producer is a **MISS**, not audio
(reading `channelPtr(1)` of a width-1 page is out of bounds). The pane's strips
use **Grow** mode where the strip is wide enough and the **2-lane cap** where
it is narrow, and the cap is announced in the tooltip exactly as the head's is.

**T6. `test_sawtooth.wav` cannot gate a channel claim** — its two channels are
byte-identical, and 80 of ~90 cases use it. The lane gate is a **PAIR**:
`test_stereo.wav` must show two lanes that differ, and `test_sawtooth.wav` must
have a lane-delta assertion REJECTED. Only a real per-channel meter passes both.

**T7. INI ownership.** Any qxa case that writes `Mixer/*` declares in its header
that it OWNS those keys, restores them, and is `RUN_SERIAL`. Never gate on the
**md5** of `smaragd.ini` — `QSettings` rewrites the whole file from its own map
and does not promise section order.

**T8. A per-track UI-state key outlives its track** unless it joins the pruning
walk. D9.

**T9. `RUN_SERIAL` is already 41 tests and it is the floor on `-j`.** The mixer
cases are layout and model assertions with no wall-clock bound, so **only the
INI-writing ones are `RUN_SERIAL`**. Do not mark the rest.

**T10. Selection highlight follows the selection SET, not just the primary** —
the head is wired to both selection signals for that reason.

**T11. `sAuditLayout` AUDITS HEIGHT ONLY, so the pane's own gate is blind to
the axis it will fail on.** `sHonestMinHeight` / the crushed test compare
`a->height() < minH` and nothing else (`smainwindow.cpp:3097-3115`) — which was
right for a vertical dock. A `QHBoxLayout` handed less than its minimum WIDTH
shrinks its children side by side and they **do not overlap**, so a horizontally
crushed pane reports `crushed == 0, overlap == 0`. Shipping AC1.7 on the
existing audit would be this repository's fourth gate that sits beside the layer
its defect lives in (proposal 39 M2, proposal 41 M5, `fix/take-lane-domain`).
**The audit gains an `sHonestMinWidth` twin and the sabotage is a width crush.**
Also: `scrollNeeded`'s `findChild<QScrollArea *>()` returns the FIRST of the
pane's N+1 scroll areas — the pane audit names the one it means.

**T12. A system lane is HIDDEN by default.** D6a. Any fixture that un-hides the
master before asserting the master strip passes whether D6a is implemented or
not.

**T13. A track-holding dock that is not on the detach list crashes on close.**
D13.

**T14. `stimeline::submitActive` stamps the ACTIVE TAB's root.** D12. A pane
that submits through it while showing a different arrangement edits the wrong
tree, and the failure is silent — the action resolves an empty path and does
nothing.

---

## Milestones, acceptance criteria and gates

Every AC below carries this repository's standing rule: **a gate that was not
watched FAILING on the pre-fix binary has not been shown to bite.** Where an
assertion could pass under a plausible wrong implementation, the sabotage is
named.

### M0 — the shared seams (pure refactor, nothing on screen)

- **AC0.1** `slaneorder::flattenTrackLanes()` exists in **`app/objects/mixer`**
  (D1 — `app/model` cannot name `STrack`); `SStdMixerView::rebuildRows` and
  `collectTracks` both consume it; no `.qxa` case is edited.
- **AC0.2** The flattened order is IDENTICAL before and after over a fixture
  holding folders, a collapsed folder, a hidden lane and the master lane.
- **AC0.3** `strackbroadcast::{targetsFor,orderByLane,pruneNestedTargets}`
  exists in `app/objects/mixer`; `SStdMixerView::selectionTargets` becomes a
  one-line delegating member (D3 — not a deletion; twelve call sites, eleven of
  them structure verbs the mixer never mounts).
- **AC0.4** `orderByLane`'s "no visible row sorts LAST" behaviour (D3a) is
  either preserved or changed deliberately, and a **unit test says which**.
  Nothing in the qxa suite covers a multi-selection spanning a collapsed folder.
- *(Revision 1's `sfadercurve.h` move is deleted — D4.)*
- **Gate:** the FULL suite green with **no case file changed** (that is the
  assertion — a refactor that needed a case edited is not this refactor), plus a
  new `laneorder_test` for AC0.2/AC0.4 and `action_roundtrip_test` unchanged.

### M0 as executed (2026-09-07)

Shipped as committed. `slaneorder::flattenTrackLanes()` and
`strackbroadcast::{targetsFor,orderByLane,pruneNestedTargets}` in
`app/objects/mixer`; `SStdMixerView::rebuildRows` is one walk plus a per-lane
row fold, and the three head members are one-line delegating members.
`appendSystemRows()` is retired with its four rules carried forward in a
tombstone where it stood.

**AC0.4 / D3a resolved as PRESERVED.** "A track with no visible lane sorts
LAST" was an ARTIFACT of `rowIndexOfTrack()` answering -1 rather than a
decision, and it survives because the arranger's row list IS the flattened
walk with fold and hidden honoured — so a track absent from it is exactly a
track with no row. `laneorder_test` is the only thing that says so: the one
shape distinguishing the two orders is a multi-selection spanning a COLLAPSED
folder, and nothing in the qxa suite covers it.

**TWO CLAIMS IN D1 WERE STALE, both found by trying to execute them.**

1. **AC0.1's second consumer does not exist.** D1 calls `collectTracks` at
   `sautomationlane.cpp:122-147` "a THIRD model walk (the prune sweep's)".
   Proposal 46 M3 deleted it, and the file carries a tombstone saying so.
   Revision 2 caught the neighbouring half of that same deletion ("D9's
   pruning inventory was stale") and did not follow it through.
2. **A DIFFERENT `collectTracks` has the very bug the shared walk prevents.**
   `spluginnativeeditor.cpp:172` walks `childLinks()` only, so
   `restoreOpenEditors()` misses the master lane and every system lane —
   proposal 45 AC4.6's shape again. An editor left open on a master-lane
   insert is not restored on load. **Not fixed:** it is a legitimate consumer
   (`pluginui` already declares the `objects/mixer` edge), but
   `restoreOpenEditors()` returns early in `--test-case` mode, so the fix
   would be ungated — and M0's entire claim is that it changes nothing.

**Also found: SEND LANES HAVE NO ARRANGER ROW** — zero mentions in
`sstdmixerview.cpp`, though 45's design text says the tail is "sends above,
master last". `slaneorder.h` records it and offers `SystemLanes::All`; the
arranger keeps asking for `MasterSubtree`, so the row count is unchanged.

**The test caught an error in the AUTHOR's expectations**, which is worth
recording because it is the same class of mistake the milestone exists to
prevent: un-hiding the master lane does NOT reveal its conductor lane —
`laneHiddenByDefault()` is true for every system role, so hidden is per lane.
One check was passing vacuously as a result; both were fixed and the per-lane
rule is now pinned before the check that depends on it.

**Suite:** 367/367 passed, 0 failed, 455 s at `-j4`; 370 registered / 367 run
/ 3 Not Run (Disabled), reconciled both ways — the disabled three are the
macOS-only `au_*` trio. **No case file changed**, which is AC0.1's actual
assertion. **Watched failing under five sabotages**, three of them cleanly
disjoint: fold ignored everywhere (7 checks), the system tail appended FIRST
(4), the D6a exemption descending (2 — the mixer-pane pair only), lane-less
tracks sorting FIRST (2 — the AC0.4 pair only), any multi-selection
broadcasting (1 — the aimed-outside check only).

### M1 — the pane, the strips, inserts, fader, meter, M/S/R

- **AC1.1** A `Mixer` dock exists, ninth, `objectName="dock_mixer"`, hidden on a
  first run, restored from `ui/windowState`, reachable from the View menu, a
  toolbar button and the shortcut. (shell CONTRACT **inv. 7** is the dock rule;
  inv. 4 is the restore ORDER it depends on.)
- **AC1.2** One strip per lane of D1's walk, in that order; a hidden USER lane
  has no strip; a collapsed folder's children DO have strips (D2); **the master
  strip is present on a project that has never called `set-lane-hidden`**
  (D6a, T12) and is pinned right.
- **AC1.3** The strip's sections are in D5's order, and the fader/meter/M-S-R
  block is outside the scroll area.
- **AC1.4** M, S and R over a **multi-selection** are **one undo step** and
  drive every target to the pressed state; a gesture aimed at a strip OUTSIDE
  the selection acts on that strip alone (D3's rule, both halves).
- **AC1.5** The fader commits `set-track-volume`; a drag is one undo step; the
  arranger head and the mixer strip show the same value after either moves.
- **AC1.6** The inserts section reaches the render (add, bypass, remove,
  reorder) and **reuses** the slot's existing editor rather than minting a
  second (T4); `SPluginEffectStrip` has a narrow mode (D5a) and the Track Detail
  dock gets it too.
- **AC1.6a (revision 3)** The sends section is **`SSendStrip` mounted** (D7),
  not a second sends UI: ticking a row creates the tap, unticking DISABLES it
  and the level and pre/post survive, a send lane's own strip has no row for
  itself, and every change is one of 47 M0's verbs. `SSendStrip` has a narrow
  mode (D5a) and the Track Detail dock gets that too.
- **AC1.7 (the layout gate)** At dock heights 180 / 260 / 500 px and strip
  widths 60 / 96 px, `crushed == 0` and `overlap == 0` **on both axes** (T11),
  and the pane SCROLLS instead of compressing.
- **AC1.8** The pane submits with the ACTIVE TAB's `pathRoot` (D12): a
  broadcast made while a second arrangement tab is active edits that
  arrangement, not the master.
- **AC1.9** Closing a project with the pane open and letting one meter tick fire
  does not crash (D13).
- **Gates:** new qxa `mixer_pane_strips` (AC1.2, incl. the default-project
  master and the fold/hidden pair), `mixer_broadcast` (AC1.4/1.5),
  `mixer_inserts` (AC1.6), `mixer_sends` (AC1.6a — over a project with a send
  lane, in the shape 47's `send_strip_ui` already uses),
  `mixer_pane_layout` (AC1.7), `mixer_path_root`
  (AC1.8), `mixer_close_teardown` (AC1.9, **judged by EXIT CODE** — the
  `plugin_native_editor_teardown_safe` precedent). New verbs
  `assert-mixer-pane`, `assert-mixer-layout`, `mixer-strip-toggle`.
  `action_roundtrip_test` gains all three.
  **Watched failing:** AC1.7 twice — once with a `setMinimumHeight()` restored
  on the strip content (the T1 shape, which reproduced 13 crushed / 7 overlap at
  200 px and 0/0 at 500 in `fix/detail-pane-layout`) and once with a **width**
  crush, which the pre-T11 audit reports as 0/0 and is the whole reason for the
  twin; AC1.4 with the broadcast replaced by a single-track commit; AC1.2 with
  the system-lane tail dropped from the walk AND with D6a's exemption removed
  (two different failures); AC1.8 with `submitActive` used blind; **AC1.6a with
  unticking REMOVING the tap instead of disabling it** — 47 M5's own sabotage,
  which bit the two assertions that check the level and the mode survive, and
  which a second mount can reintroduce without touching `SSendStrip` at all.

### M1 as executed (2026-09-08)

The dock, the pane, the strip, D5a's two compact modes, three verbs and six
qxa cases. Every acceptance criterion is met; three of them cost a design
correction and one is not gateable in the shape AC1.9 imagined.

**THE SECOND MOUNT BROKE THE FIRST ONE, and it is exactly what this proposal's
governing rule is about.** `SLiveMonitor::takeInputPeak()` CLEARS the source's
peak — `twLiveInputSource::takePeak()` against its own documented `peekPeak()`
twin. Harmless while the arranger track head was the only caller; with the
pane ticking on the same 33 ms broadcast, whichever ran second read 0 and its
meter sat dead. **Exactly one mount may TAKE and every other must PEEK.**
`peekInputPeak()` added; the head keeps taking, and so keeps clearing.

**D12 WAS IMPLEMENTED WRONG FIRST, and the failure mode is silence.** The
strip used `stimeline::submitActive`, which stamps whichever editor TAB is
active. A pane showing a different arrangement than the tab in front then
submits an action that resolves an empty path and DOES NOTHING — no refusal,
no log line, nothing to notice. `SMixerStrip::submit_` now stamps the strip's
own arrangement. `mixer_path_root` asserts both halves, because the first
alone would pass if the action reached both trees.

**AC1.9's shape had to change, and the reason reshaped every seam.** A
`--test-case` run NEVER BINDS ITS PROJECT INTO THE WINDOW: `SActionRunner`
builds it straight on `SApplication` and `main.cpp` calls
`adoptCurrentProject()` only when `!testMode`, its own comment saying so. The
live pane reports `strips=0` for a whole scripted run — measured before the
fix. Every seam therefore builds a pane on demand, which is why
`describeTrackDetailLayout` and `describeSendStrip` already build their own
panel. **Consequence recorded rather than hidden:** a gesture and the
assertion after it run against DIFFERENT pane instances, so what is gated is
the VERB PATH and never widget-state persistence; anything that IS per-pane
view state (the narrow flag) cannot be asserted this way at all, and
`mixer_inserts` says so where that assertion would otherwise sit.

**AC1.7 COST FOUR LAYOUT FLOORS, every one found by measuring:**

| Floor | Measured | Answer |
|---|---|---|
| A pinned widget reads as crushed | a 20×20 button whose hint is 28, ×12 | `sHonestMinHeight` honours an explicit fixed size as the author's number. The detail-pane gate is untouched: its defect was `setMinimumHeight()` alone (min != max) |
| Three 20 px squares do not fit 60 px | `SMixerStrip(w 60<72)` | narrow uses 16 px squares, drops the dB readout, margins to 1 |
| **A `QScrollArea`'s own minimum** | ~113 px, wider than a WIDE strip | the name header moved OUT of the scroll area — a deliberate departure from D5's diagram, and the better shape: a strip's name is its identity, so D5's own rule for the fader block applies to it at least as strongly. Narrow hides the scroll area outright |
| **A `QLabel`'s minimum width is its FULL TEXT** — the one that actually held 96 px hostage | 91 px for an ordinary generated name; with the toggle and margins, exactly the 113 the strip was reported as owing | the name ELIDES at both widths, as a fixed-width column must; the full name stays in the tooltip and in `describe()` |

**And T11's width twin had to ask the LAYOUT, not the leaf.** The first
version compared every widget's own `minimumSizeHint().width()` and
immediately reported the *Track Detail* dock crushed: `SPluginEffectStrip`'s
Edit button is `setMaximumWidth( 70 )` against an 81 px hint — an author
squeezing a leaf on purpose, where a too-short widget in a vertical stack
pushes its neighbours into each other. Asking only widgets that CARRY A LAYOUT
is precisely T11's sentence and has no benign reading.

**D5a is TWO widgets in TWO modules, as revision 3 predicted**, and the FX
strip's own 113 px is what made AC1.7 fail rather than the >200 px revision 2
estimated. `SPluginEffectStrip::setCompact` and `SSendStrip::setCompact`; the
Track Detail dock keeps the full row in both, which `mixer_inserts` asserts
from both sides.

**Measured:** `crushed 0 / overlap 0` at 96 px and 60 px strips against pane
heights 180 / 260 / 500, on BOTH axes, plus scroll-instead-of-compress at a
200 px pane.

**What M1 did NOT build**, beyond the proposal's own non-goals: the four
pane-wide section toggles are plumbed (`setSectionsVisible`) and have no UI or
storage — that is M2, which also owns the narrow flag's persistence.

### M2 — collapse, persistence, pruning

- **AC2.1** The four pane-wide section toggles hide and show their section in
  EVERY strip, survive a restart through the `Mixer/sections` **SOpt** key, and
  put nothing on the undo stack.
- **AC2.2** The per-strip narrow toggle keeps M/S/R, the meter and the fader,
  and hides the name text, the inserts and the sends.
- **AC2.3** Removing a track removes its narrow-state key: a new track that
  lands at the same address is WIDE. (T8 — sabotage: skip the prune and assert
  the inherited state.)
- **AC2.4** With every section off, the pane is still usable and the layout gate
  still reads 0/0 on both axes.
- **Gates:** `mixer_sections` and `mixer_narrow_strip` (both `RUN_SERIAL`, both
  declaring `Mixer/*` ownership per T7), plus `mixer_pane_layout` re-run with
  sections off.

### M3 — meters, audibility, and the live path

- **AC3.1** Every strip's meter reads its own track: with two tracks at
  different gains, the two meters differ in the direction the gains do.
- **AC3.2** The **lane PAIR** (T6): `test_stereo.wav` shows two lanes that
  differ; `test_sawtooth.wav`'s lane-delta assertion is REJECTED.
- **AC3.3** A page MISS decays a strip's meter to the floor; it never holds.
- **AC3.4** The master strip's meter reads the mixer's rewire (45 D12) and is
  non-silent while the arrangement is — **on a default project** (D6a).
- **AC3.5** With the dock HIDDEN, the tick handler does no per-strip work
  (asserted by a counter on the pane, not by timing).
- **AC3.6 (D11b)** A solo-muted nested lane's strip meter is DARK, and an
  ARMED/monitored track's strip meter is LIT — the second one reading the
  pre-FX input level, because a live-owned lane has no frozen pages. Both
  through `ssolo::isLaneAudible` and the head's own monitored branch, never a
  local copy.
- **Gates:** `mixer_meter_lanes`, `mixer_meter_master`, `mixer_hidden_no_work`,
  `mixer_meter_audibility` (`RUN_SERIAL`, `SMARAGD_CAPTURE_SPEED=1`, the L1b
  paced `file:` input — the shape `monitor_through_chain` already uses).
  **Watched failing:** AC3.2 with the meter fed `min(2, lanes)` of channel 0
  duplicated — which passes over `test_sawtooth.wav` alone and is exactly the
  defect that made `channel_assert_dupmono` incapable of detecting a narrow
  sink; AC3.6 with the probe run unconditionally.

### M3a — the fader as an automation control (D11a)

- **AC3a.1** A Touch pass driven from the MIXER fader commits **one**
  `set-automation-points` for the gesture and is reverted by ONE undo — the
  `automation_write_pass` shape, through the other fader.
- **AC3a.2** While a Read-family `self:Volume` lane exists, the mixer fader
  DISPLAYS the curve's value at the position being heard, and a fader being
  RECORDED is exempt (it shows the hand).
- **AC3a.3** A double-click reset during an open pass goes through the recorder,
  not through a bare `set-track-volume`.
- **Gate:** `mixer_write_pass` (`RUN_SERIAL`). **Watched failing** with the
  `writeTick` offer removed — expect an action per slider tick, which the
  undo-count assertion reads directly.

### M4 — the finish, and the documents

- **AC4.1** Double-click resets the fader to exactly 0.0 dB and each plugin
  slider to the plugin's declared default (`sdefaultreset`), subject to AC3a.3.
- **AC4.2** The strip's context menu offers the TRACK-verb subset of the
  arranger's menu, extracted and shared — **not** the whole menu, half of which
  is row concepts (lane height scale, take lanes, the automation-lane picker)
  that are meaningless in a pane with no rows, and **not** a second copy. If the
  extraction is not done, the menu is dropped from this milestone rather than
  duplicated.
- **AC4.3** Track colour: the strip's header uses `sclipcolors`' resolved
  colour, so the mixer and the arranger cannot disagree.
- **AC4.4** Scroll: the pane scrolls horizontally, the master stays put, and a
  wheel over a fader is 1 dB per notch (the head's one deliberate exception).
- **AC4.5** `main/mixerui/CONTRACT.md` exists — carrying D6a (the one place the
  two mounts deliberately disagree about a model flag), D12 and D13;
  `main/timeline/CONTRACT.md` gains inv. **64** (the shared walk) and inv. **65**
  (the shared targets) — **not 63/64: 47 M5 took 63 and moved that file's
  numbering note to "currently 64"**, so MEASURE the note rather than quoting
  this line; `main/shell/CONTRACT.md` gains the ninth-dock and detach
  invariants; `docs/ACTIONS.md` gains the new verbs;
  `docs/ARCHITECTURE.md`'s module table gains `mixerui`; CLAUDE.md gains a
  section in the house style, including a **NOT gated** list.
- **Gate:** `mixer_pane_polish`, plus `check_layering.py` / `check_logging.py` /
  `check_includes.py` and a full-suite reconcile (`ctest -N` registered vs run
  vs disabled — MEASURE it, never quote the stale figure).

---

## Non-goals

- **Building any routing.** Revision 2 listed sends here; proposal 47 executed
  them, so the pane MOUNTS the sends UI (D7) and builds none of it. What stays
  a non-goal is everything 47 itself left out: the send-path **latency / PDC**,
  a routing MATRIX, and creating a send lane from the mixer (`add-send-lane` is
  45's verb and the arranger owns that gesture).
- **Pan.** D8. Needs an engine pan law, a stored per-track value and a
  `self:Pan` automation target.
- **Pre-fader metering, gain reduction meters, PDC.** Each needs engine work
  (proposal 34's tap note; proposal 37 P9).
- **A separate mixer WINDOW or tab.** A dock is what was asked for, and the
  view-tab machinery (`SViewTabs`) is per-editor-ROOT, which a mixer is not.
- **Channel-strip EQ, input gain, phase, record-path monitoring controls.**
- **Reordering tracks from the mixer.** The grip drag resolves through
  `move-track`, which the arranger already owns and which 45 D6 refuses for
  system lanes; a second drag surface is its own milestone.
- **VCA / group faders.** The edit-group mechanism exists but is a link, not a
  fader.

## What this proposal will NOT gate

Named up front, so a green suite is not read as coverage it does not have:

- **Pixels and aesthetics.** The layout gate asserts a geometry RELATION
  (crushed/overlap, scroll-instead-of-compress) and the colour assertion a
  LUMINANCE relation — never a palette. No `paintEvent` of the pane is gated
  anywhere.
- **The dock's docked/floating/closed round trip** through Qt's opaque
  `ui/windowState` blob. The same manual gap the media browser's own runbook
  still records as unrun.
- **The toolbar button and the View-menu item themselves**, and the context
  menu. There is no testkit verb for a menu or a toolbar anywhere in this repo;
  what is gated is the VERB each one submits.
- **The shortcut's interaction with real keyboard focus.**
  `QApplication::focusWidget()` is always null in a `--test-case` run, because
  the main window is never shown — `main/eventui/CONTRACT.md` records this.
- **Drag ergonomics** — synthesised presses go straight to the handler.
- **Repaint cost with many strips.** It will be MEASURED at M3 (a canvas grab,
  baseline-subtracted, the shape proposal 41 AC5.4 used) and reported, not
  bounded. A latency bound would be a wall-clock assertion, and this suite
  already carries 41 `RUN_SERIAL` tests because of those (T9).
- **Real device meters and real driver latency.** Everything is measured
  through the capture backend.
- **Either narrow mode below the threshold this proposal picks** —
  `SPluginEffectStrip`'s and `SSendStrip`'s are each gated at the two widths
  AC1.7 names and nowhere between.
- **A send whose destination lane is HIDDEN**, and a send lane's own strip
  showing a send to another send lane (47 T9 calls that shape legitimate and
  47 D6's cycle break governs it). The pane mounts the widget that handles
  both; nothing here measures either.
- **The silent send under monitoring** (47 D9). The pane's tooltip states it;
  47 M4 gates the mechanism, and neither gates what a user HEARS.
- **The master strip's exemption from `laneHidden()` under a project that
  EXPLICITLY hides the master** (D6a says the mixer shows it anyway; the case
  asserts the default-project shape, not the explicit-hide one).
- **A mixer open on a second arrangement tab while the first is edited** —
  AC1.8 gates the root stamp on one submit, not the two-tab interaction.

## Cost, roughly

Revised after the review — M1 grew a narrow mode in `pluginui` (D5a) and three
seams revision 1 missed (D12, D13, and the audit's width twin), and M3a is new.
**Revision 3 moved the total slightly DOWN in risk and slightly UP in lines:**
D7 stopped being a bespoke placeholder and became a mount of a gated widget,
and that same widget brought a second narrow mode with it (D5a).

| Milestone | New code | Touched | New cases |
|---|---|---|---|
| M0 | ~300 lines (two headers + a unit test) | `sstdmixerview.cpp`, `ssmvmixercontrol.cpp`, `sautomationlane.cpp` | 1 unit test |
| M1 | ~900 (`mixerui`: pane + strip) + ~200 (`pluginui` narrow mode) + ~120 (`timeline` `SSendStrip` narrow mode, **new in r3**) + ~300 testkit incl. the width audit | `smainwindow.cpp` (dock, menu, button, **detach list**), `check_layering.py`, CMake | 7 qxa + 3 verbs |
| M2 | ~200 | pane + `SOpt` | 2 qxa |
| M3 | ~200 | pane only | 4 qxa |
| M3a | ~120 | pane only | 1 qxa |
| M4 | ~150 + the context-menu extraction (or drop AC4.2) | contracts, docs, CLAUDE.md | 1 qxa |

M0 is the milestone most likely to cost more than it looks, and it is the one
that must not be skipped: it is the only thing standing between this proposal
and a second spelling of two rules the arranger already owns. Revision 2 cut its
scope where the cost bought nothing (the `sfadercurve.h` move, the "delete
`selectionTargets`" demand) and widened it where a third walk would otherwise
have survived the milestone (`collectTracks`).

**The three findings that changed this proposal most are not in Part A at all.**
D6a (a default project has no master lane), D12 (the pane must stamp the active
tab's root) and D13 (the close-project detach seam) are each a bug class this
tree has already paid for once, and none of them was visible from the feature
description. Whoever executes this should expect the same ratio: the strip is
the easy half.
