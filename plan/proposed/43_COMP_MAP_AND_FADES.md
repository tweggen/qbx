# Proposal 43 — A comp MAP on the take column, and a fade model

Status: **OPEN**, design agreed 2026-08-23. This is proposal 42's M5 — the
CAPABILITY gap, as opposed to 42's M1-M4, which were defect work. Nothing here
fixes a bug; all of it adds something the model cannot express today.

## 1. What the model cannot do

A use-case review mapped ten real comping workflows onto the model as it
stands. Four are impossible BY INVARIANT and one whole primitive is missing:

| # | capability | why it is missing today |
|---|---|---|
| 1 | a take that sounds only over PART of the column | one `activeTake_` per column |
| 2 | a boundary you can MOVE | a boundary is the coincidence of two clip edges after a split; nothing holds them together |
| 3 | two takes OVERLAPPING at a boundary | a column plays exactly one take at a time |
| 4 | a CROSSFADE at a join | **no clip fade or crossfade exists anywhere in the model** (`twGrainParams::crossfadeMs` is a granular-synthesis knob; the live monitor's 2-3 ms ramp is an RT detail) |
| 5 | a take shorter or longer than its column | `stakestack.h` invariant 1 |
| 6 | a take offset WITHIN its column | take link `startTime` is 0 by invariant |

Workflows 1-4 are one feature: **word-by-word comping with movable,
crossfadeable boundaries** — the singer-takes case. 5 and 6 are a different
feature and are NOT in scope here (see §7).

## 2. The decision: a comp MAP, not splits

The requester chose the comp-map model over the split model, knowing the cost.

```
ONE clip on the timeline
+-------+----------+--+------------------+
| take2 | take4    |t1| take4            |    comp map
+-------+----------+--+------------------+
        ^          ^  ^   boundaries are OBJECTS: drag to move,
                        give each a crossfade length
```

The rejected alternative — a boundary IS a split — **already works** since
proposal 42 M3, and needs no engine work at all. It was rejected because a
boundary is then not a thing: moving one means adjusting two clip edges, and
overlap is not expressible.

## 3. THE LOAD-BEARING ENGINE FACT

**The mix resolves a clip's component ONCE PER PAGE, and a page is 65536
frames — 1.37 s at 48 kHz.**

`twTrackMix::planPage` and `freezePage_nolock` both compute
`childPos = pageStart - clip.startTime` and call `clip.view->resolve( childPos )`
/ `clip.view->freezePage( childPos, …, length, … )` exactly once per clip per
page (`tw303a/mix/src/twtrackmix.cc`). Proposal 19 Inv-1 is that plan and render
resolve identically — one snapshot, one component.

A comp boundary lands on a WORD. Words are ~200 ms. So **per-page resolution is
two to seven times too coarse**, and no amount of model work changes that: the
map has to be honoured BELOW the clip, not at it.

Two ways to do that, and the choice matters more than anything else here:

**(a) Segment the clip in the mix.** `twTrackMix` loops clips x comp regions
instead of clips, one `freezePage` per region with its own destination
sub-range. Rejected: `clip.previousPage` — the DSP-state predecessor chain — is
per clip, and would have to become per (clip, take); every consumer of
`twPagePlan` grows a case; and the mix learns what a take column is, which is
exactly the coupling `resolveClip` exists to prevent.

**(b) THE COLUMN BECOMES A REAL COMPONENT.** `STakeStack::getRootComponent()`
today returns the ACTIVE TAKE's component; it would return its own
`twCompColumn`, which holds the takes as inputs and renders each output range
from whichever the map selects, crossfading across a boundary. **Nothing above
it changes at all** — the mix keeps resolving one component per clip per page,
and `resolveClip`'s contract is untouched.

**(b) is the design.** It is also what makes the crossfade natural (a boundary
is a range where two inputs are both live) and what leaves room for §7.

## 4. The model

A comp map belongs to the COLUMN (`STakeStack`), beside `activeTake_`:

```
<STakeStack activeTake='0'>
  <comp>
    <seg at='0'      take='2'/>
    <seg at='38400'  take='4' xfade='2400'/>
    <seg at='96000'  take='1' xfade='2400'/>
  </comp>
  <SLink .../>   <!-- the takes, unchanged -->
</STakeStack>
```

* Segment positions are in the COLUMN's own frame domain, the same domain the
  takes' windows are addressed in.
* `xfade` is the length of the crossfade CENTRED on the boundary, 0 = a hard
  cut. It belongs to the boundary, not to either take.
* **`activeTake_` survives as the map's DEGENERATE case** — an empty map means
  "the active take everywhere". Every project written before this proposal
  therefore loads and sounds exactly as it did, and `select-take` keeps its
  meaning: it rewrites the map to a single segment. That is what keeps proposal
  42's whole gate set meaningful.
* Written only when non-empty, like every other optional element in this tree
  (`SObject::serialize`'s automation rule), so no existing file or golden moves.

## 5. Milestones

**N1 — the map in the model.** `twCompMap` (a value type in `tw/events`-style
plain C++: sorted segments, exact frame positions), held by `STakeStack`,
persisted, with the verbs: `set-comp-segment`, `remove-comp-segment`,
`move-comp-boundary`, `set-comp-xfade`. Undoable, addressed like every other
take verb. NO audio yet: the map is read by nothing, so nothing sounds
different. Gate: round-trip + the verbs' inverses.

**N2 — `twCompColumn`.** The engine component. Renders each output range from
the take the map selects; hard cuts only. `STakeStack::getRootComponent()` /
`resolveClip()` return it. This is where proposal 19's rules bite: it is a
component with N inputs whose page plan must declare the takes it will actually
read for THAT page, and its own state chain per input. Gate: a two-take column
with a boundary at a known frame renders take A before it and take B after,
measured to the frame against a closed form.

**N3 — crossfades.** `xfade` becomes audible: an equal-power (or linear —
decide with a listening test, gate with a closed form either way) fade across
the boundary, both takes live inside it. Gate: the sum at the boundary centre
against the closed form for the chosen curve.

**N4 — the UI.** Boundaries drawn on the take lanes, click-to-comp writing a
segment rather than the whole column, drag a boundary, drag its crossfade
handle. The gestures go through `SMVActualView`'s existing take-row press path
(proposal 42 M1's `clipEditTargetOf` already resolves the column for it).

**N5 — CLIP fades, separately.** `fadeIn` / `fadeOut` / shape on
`SClipWindow`, which is capability 4's other half and is wanted independently
of comping (every DAW has it; this one has nothing). It is listed here because
N3 needs a fade CURVE and the two must share one, not because it is part of
comping.

## 6. Traps already visible

* **The map is in the COLUMN's domain, and a wrapped column has a wrapper's
  window over it.** Proposal 42 M4 migrates most of those away, but a REFUSED
  one (looping, stretched, panned, automated) survives — so `twCompColumn` must
  never assume its output domain is the placement's.
* **`activeTake_` and the map must not be able to disagree.** One of them has
  to be derived. The design above makes the map authoritative and the empty map
  mean "active take", which is the only reading under which every existing
  project and every existing gate stays correct.
* **A boundary is not a split, and the take lanes must not imply it is.**
  Proposal 42's own history says a paint that disagrees with what plays is not
  cosmetic (the take-lane domain bug, #133).
* **Do not compute the comp at freeze time and stash it.** The fourth time this
  codebase would make that mistake (level meters, MIDI-out, the metronome,
  folder-sum). The map is read BY POSITION inside the component, exactly like
  `twGainStage` reads an automation curve.
* **The crossfade needs both takes' pages for the same range**, so
  `twCompColumn::planPage` must declare BOTH across a boundary or the scheduler
  will not have them — the same "declare what you will read" rule
  `twTrackMix::planPage` follows.

## 7. NOT in scope

Per-take trim and a per-take offset within the column (capabilities 5 and 6),
which need `stakestack.h` invariant 1 relaxed and are a different feature: with
a comp map, "this take sounds only here" is the MAP's job, which is what those
two were wanted for in a comping context. Two clips on one take lane, and
structural editing inside a fragment, likewise stay out.
