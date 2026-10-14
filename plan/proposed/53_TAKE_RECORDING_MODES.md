# Proposal 53 — Take recording modes: union columns, split-and-stack, overdub, replace

**Ticket:** QBX-156 (decisions taken 2026-10-11); QBX-157 is the view half.
**Status:** PROPOSED (2026-10-11). Decisions from QBX-156 by the user;
reviewed adversarially in five rounds (GREEN at round 5); D1–D6 (§0a) open,
each with a default the phases implement. Written against `origin/main`
`c682e944`; §13 lists every review finding and what was done with it.

Source locations are given by function or symbol name; a line number appears
only where a GATE pins it (and was re-read at `c682e944`). The four working
documents behind QBX-156 were written against `c10bdd9b`; every reference
kept here was re-verified, and the ones that moved are named in §11.

This plan does not re-open the ticket's decisions Q1–Q14. It makes them
precise, finds where they collide with each other or with the code, and cuts
the work into PRs an implementing agent can land one at a time, each behind a
gate that was watched failing first.

## 0. Prerequisite reading

- `plan/proposed/42_TAKE_COLUMN_ONE_SHAPE.md` — the two shapes of a column, and
  why a consumer that knows one spelling is a defect generator.
- `plan/proposed/43_COMP_MAP_AND_FADES.md` — the comp map, `twCompColumn`, the
  fade model. **§7 of it excludes per-take extents; Q1 re-opens that.**
- `plan/proposed/52_TAKE_COLUMN_DELETE_INVERSE.md` — the PIN pattern for an
  inverse that must restore by identity; `merge-columns` (§6 here) uses it.
- `smaragd/main/objects/cut/CONTRACT.md` — "Take stacks", "The COMP MAP", "A
  clip FADE is audio-only", "`place-recording` may carry a NEGATIVE source
  offset".
- `smaragd/main/shell/CONTRACT.md` inv. 19–31 — the recorder: placement
  conversion, trim floor, growing clips, loop passes, punch.
- `docs/ACTIONS.md` — `place-recording`, `place-midi-recording`, `add-take`,
  `select-take`, `split-clip`, the `set-comp-*` family, `set-clip-fade`.
- `smaragd/main/objects/cut/src/splacerecordingaction.cpp` — the planner this
  replaces, read in full (it is 190 lines and every comment in it is a fact
  the new planner must keep true).

## 0a. Decisions still needed

Six points where the ticket's decisions do not reach, or collide with the
code. Each phase that depends on one says which way it goes BY DEFAULT, so an
implementer is not blocked if the user takes the recommendation. Everything
else in this plan is decided.

| # | question | options | recommendation (= the default the phases implement) |
|---|---|---|---|
| **D1** — **DECIDED AND SHIPPED (P2, QBX-180)** | Q7 under mode A: two legacy OVERLAPPING plain clips SUMMED; merged into one column, only one take plays per position outside `[a,b)` | (a) accept — merge, topmost plays, the other is a selectable take; (b) refuse the pass on such a lane; (c) leave the lower clip outside the column, so it keeps summing with the column (X6 survives there) | **(a)**, taken. (b) blocks the commonest legacy lane (C13). (c) keeps a doubled signal the user never asked for. `rec_union_overlap_legacy.qxa` asserts BOTH halves: `takeAtPos` names the topmost as the previous answer outside `[a,b)`, and a render before and after measures the level HALVING over the overlapped second — the fixture is built so the two clips play the same sample where they overlap, so the sum is exactly twice one of them |
| **D2** | Q2 "per TRACK GROUP": folder track or edit group? | (a) the FOLDER track (`STrack::hasChildTracks()`), an object that already serializes attributes; (b) the EDIT group, which is an int id on members and needs a per-project `editGroupId → mode` map and a UI home | **(a)**. M0 implements the folder reading; (b) is one more `SProjectProps` table and can be added later without changing the resolver's shape |
| **D3a** — **DECIDED AND SHIPPED (P5, QBX-183)** | Q9 default 3 ms: the auto-fade is SEEDED into new projects only; a file without `autoFadeMs` reads 0 | (a) seed only (existing projects stay as they render today); (b) read 3 ms as the default (every existing render and dozens of gates move) | **(a)**. (b) flips byte-identity goldens and per-second RMS gates project-wide |
| **D3b** — **DECIDED AND SHIPPED (P5, QBX-183)** | Auto-fade at a BUTT JOIN of one continuous source (every `split-clip`, every mode-B seam, every `replace` R2 split) is a 3 ms dip, not a crossfade — an audible notch in sustained material | (a) exempt SEAMLESS edges: two abutting clips over the same content whose source positions are continuous get no auto-fade; (b) apply everywhere (Cubase's Auto Fade does, which is why it ships OFF there); (c) ship the feature OFF by default (seed 0) | **(a)**. P5 implements the seamless test (same `windowContent()`, `srcEnd(left) == srcStart(right)` exactly, same stretch); the dialog shows which edges are exempt |
| **D4** — **DECIDED AND SHIPPED (P4, QBX-182)** | Q5 when an un-armed edit-group member's clip crosses the armed lane's column extent `C`, or two armed lanes in one group have different `C`s (group twins need exact `startTime` + `getDuration()` equality, `seditgroups.h` `correspondingClip`) | (a) member column extent := the group's C (union of the armed lanes' C); member children crossing its edges are split there first (reversible); (b) skip the member with a hint when its clips do not fit; (c) ask | **(a)**, taken. It is what the group is for; the split is the mode-B primitive and undoes cleanly. **What (a) did not say is that `C_g` governs the ARMED lanes too** — §3.4 does say it ("ONE rule for EVERY member") and it is the half that is easy to miss: two armed lanes with different `C`s are not each other's twins either, so BOTH of their columns come out `C_g` long. And a member's column being `C_g` rather than its operands' union needed a verb that could say so: `merge-columns` gained `start=`/`length=`, with ONE operand accepted only when an extent is given. `takes_group_record_crossing` is the gate |
| **D5** | `cleanup-takes` "unused take": after W1 the ORIGINAL clip A is an inactive take no segment selects → "unused" → removed (undoable, file kept — but it is the user's arrangement, not a recording) | (a) takes that came from pre-existing clips (not from a recording) are EXEMPT by default; a checkbox includes them; (b) treat every unused take alike | **(a)**. The confirmation lists takes by origin ("3 recordings, 1 original clip"). Origin = the take's name stamp from the recorder (§8 P4) or, before P4 lands, "file in the project directory matching the recorder's pattern" |
| **D6** — **DECIDED AND SHIPPED (P3, QBX-181)** | MIDI take extents (P3): ticks or frames? The column is frame-anchored; the event take window is TICK-native (`smidicut.h`, "THE WINDOW IS TICK-NATIVE") | (a) ticks on the take, converted through the tempo map at the column; (b) frames for parity with audio | **(a)**, taken, and the MODEL ALREADY DELIVERED IT: no new per-take field was needed. A take's link inside a column is born `timebase=beats` for event content (`SLink::defaultTimebaseFor`) so `set-tempo`'s re-derivation moves its extent START, and the window's length is `lengthTicks_` on the `SMidiCut` so the extent LENGTH follows — measured, halving the tempo moved a take extent 0-96000 → 0-192000. `add-midi-take atTicks=`/`extentTicks=` therefore take TICKS and convert once, through the tempo map and through `setWindowTicks` rather than the frame-valued `setDurationFromTimeline` (a ticks → frames → ticks round trip is exact only where a tick divides a frame). **What did NOT follow the bar was two stored frame values the decision did not name**: the column's own `duration_` and the COMP MAP's positions — see §8's P3 row |

## 1. Why

One vocal track, one clip split at bar 8 beat 1: `A = [0, 8.1)`,
`B = [8.1, end)`. Record from a stopped transport at 0, stop at `T` with
`8.1 < T < end`. The part from 8.1 vanishes.

Root cause, three parts, all verified:

1. **`place-recording` judges every existing child alone.** `SPlaceRecordingAction::apply`
   collects children overlapping `[recStart, recEnd)`; a child whose
   `startTime < recStart` is dropped with a warning (the "predates" rule,
   `splacerecordingaction.cpp:87-118`); every other child becomes a column
   and gets the recording as a take sized to the WHOLE column
   (`SAddTakeAction::apply`, `takeCut->setDuration( stack->getDurationBlocking() )`,
   `saddtakeaction.cpp:79-81`), activated (`:92-94`). So A and B each become
   a two-take column with the recording active.
2. **The column's take runs past its audio.** B's new take spans
   `[8.1, end)` but the file ends at `T`, so `[T, end)` is silence. The gate
   `takes_recording_placement.qxa:40-51` asserts that silence as the
   expected result ("then silence (recording ended but the column window
   continues)"). That is X2, pinned.
3. **Nothing shows the old material.** `STakeStackRendererInline::draw`
   paints the active take and a `k/n` badge only (`stakestack.cpp:505-521`);
   the old B is a dimmed row behind the "T" toggle
   (`ssmvmixercontrol.cpp:832`).

And one structural fact that makes the result depend on the transport: from a
stopped transport the trim floor pins `placementFrame(trimmed_) == recordStart_`
exactly (`saudiorecorder.cpp:336-338`), so A is a column; from a running
transport (P1, pre-roll) the landing is a few ms late, A "predates", and the
recording is laid over it as a plain cut — both play. Same gesture, two
structures (X1).

Nothing is lost today; it is invisible, partly silent, and non-deterministic.

## 2. Vocabulary

**R** — relation of one existing lane child `[s, e)` to the recorded span
`[a, b)`:

| R | geometry | today |
|---|---|---|
| R0 | disjoint or touching (`e <= a` or `s >= b`) | untouched |
| R1 | started before, ends inside (`s < a < e <= b`) | OVER (plain cut on top, both sum) |
| R2 | contains the recording (`s < a`, `e > b`) | OVER |
| R3 | starts with it, ends inside or with it (`s == a`, `e <= b`) | TAKE, exact |
| R4 | starts with it, ends after (`s == a`, `e > b`) | TAKE + silence tail |
| R5 | starts inside, ends inside or with it (`a < s`, `e <= b`) | GAP cut + TAKE |
| R6 | starts inside, ends after (`a < s < b < e`) | GAP cut + TAKE + silence tail |

**K** — kind of that child: **Kp** plain `SCut` (slipped, stretched, looped,
faded included); **Kc** a direct column, no comp map; **Km** a direct column
WITH a comp map; **Kw** a wrapped column (`SLink → SCut → STakeStack`) that
`stakes::normalizeColumns` refuses to fold; **Ko** another content kind
(`SMidiCut` on a lane recording audio, or the reverse).

**Touched set** `T` of a lane for a span: every child with `s < b && e > a`
(R1–R6), after the kind filter (§7 B2) and K-normalisation.

**Extent** of a take: the column-relative range `[tStart, tEnd)` over which
the take has material (§4). Today every take's extent is the whole column.

**Modes** (Q2 naming, decided here): `takes` (union, mode A, DEFAULT),
`takes-split` (mode B), `overdub`, `replace`.

## 3. The model: what each mode does, per cell

Conventions: `O` is the existing child (its takes, if a column); `R` is the
recording; `C` is the resulting column's extent; a map is written as
`p→take` segments in COLUMN frames; "heard" is left to right along the lane.

### 3.1 Mode `takes` (A, union) — the rule

For a lane whose touched set is empty: a plain windowed cut `[a, b)`
(`place-clip`), exactly today's empty-lane result. Otherwise:

1. **One column per pass per lane**, extent
   `C = [ min(a, min s_i), max(b, max e_i) )` over `T`. Contiguous by
   construction: every member of `T` intersects `[a, b)`. (This is the rule
   for a lane in NO edit group; a grouped lane uses the group extent `C_g`
   of §3.4, from P4 on.)
2. Every member of `T` **becomes a take of that column at its own extent**:
   a plain cut as one take over `[s_i − C.start, e_i − C.start)`; a column's
   takes transfer with the column's extent; a column's comp map transfers
   shifted by `s_i − C.start`.
3. `R` is added as a take over `[a − C.start, b − C.start)` and becomes the
   active take.
4. The comp map is `M0` **outside** `[a, b)` and `R` **inside**, where `M0`
   is "the previous answer" at each position: the touched member that had
   audio there, and inside a member column its own map (its active take where
   the map is silent). Where two touched members overlapped (pre-existing
   OVER lanes, Q7) the TOPMOST (highest child index) one is the previous
   answer.
5. **X2 under A, for a free or punch span:** a take body is exactly as long
   as its material (its extent), and the planner writes no segment that
   selects a take outside its extent. There is no silent take body anywhere
   in the result. Should a later edit (a `remove-take`) leave a position
   whose selected take has no material, `twCompColumn` renders silence there
   and the painter hatches it (§4.4) — visible, never hidden.
   **For a cycle span (`Span.fromCycle`) the take extent is the WHOLE cycle**
   and a late-started pass keeps its leading silence (negative `srcOffset`,
   shell CONTRACT inv. 27, `record_loop_late_start.qxa`) exactly as today:
   the cycle region is the take region, and a pass that began mid-cycle IS
   silence then material. X8 (narrowed) is what step 5 guarantees there, and
   it is about the RECORDED passes: no recorded take extends beyond the cycle.
   Old material the cycle touched keeps its own extent (C8's clip `[6,20)`
   stays `[6,20)` inside the union column).

Mode A is deterministic under every transport state WITHOUT the sliver rule
(a 3 ms overlap is still an overlap), but the sliver rule still runs (§3.5):
without it, a recording landing 3 ms before 8.1 would pull all of A into the
column.

> **SHIPPED as P2 (QBX-180). FIVE CORRECTIONS from building it.**
>
> **(s) STEP 5 NEEDS A NUMBER, NOT A FLAG, and `Span::fromCycle` is gone.**
> QBX-171 left a boolean here for exactly this rule and a boolean cannot carry
> it: `place-recording` clamps the span to the MATERIAL
> (`min(length, waveDur - srcOffset)`) while the recorder's cycle branch always
> asks for the whole `loopLen`, so the LAST pass of essentially every cycle
> recording has `b < cycleEnd` and nothing in `Span` says where the cycle ends.
> It is now `Span::extent` (0 = the placed span), carried by a new
> `place-recording extent=` attribute and passed by the recorder as the cycle
> length. Gated exactly by `rec_union_cycle_extent`, which is SCRIPTED: the
> rule is about the last pass, whose take index depends on how many passes the
> box got through, so a recorder-backed case can only assert it as a floor.
>
> **(t) A DEVIATION FROM STEP 4 WHERE THE EXTENT REACHES PAST `b`.** Step 4
> says the map is `M0` outside `[a, b)`. `merge-columns` paints an operand's
> answer over its whole EXTENT, so a cycle pass placed `extent` long answers
> over `[a, extentEnd)` and renders silence from `b` on instead of falling back
> to the old material. Separating the two would need a verb that sets a take's
> extent independently of the span it answers for, and none exists —
> `add-take at=/length=` is the only writer of an extent and it creates a take.
> The combination is reachable only by a live cycle pass that STOPS MID-CYCLE
> over pre-existing material, and the behaviour is defensible on its own: the
> user asked for a pass over those bars, that pass is the answer for all of
> them, silence included, which is what a tape machine does — and the previous
> answer is one `select-take` away. Recorded rather than coded around.
>
> **(u) MODE A IS TWO VERBS, and it has to PRE-FLIGHT A1's REFUSAL.**
> `place-clip` the pass, then `merge-columns` over the touched children in
> ASCENDING CHILD INDEX with the pass LAST — §6.1 is already steps 1, 2 and 4,
> and the pass being last gives step 3. But §6.1's parameter refusal
> (correction (n)) is fatal for a recording: `SCompositeAction::apply` rolls
> back every earlier child on the first failure, so a `merge-columns` that
> refuses takes the pass's own `place-clip` with it and LOSES THE TAKE. The
> planner therefore asks the same questions of the snapshot — volume, pan, a
> `cut:Gain` envelope, a clip fade, and whether an operand is something the
> verb can take at all — and DEGRADES to a plain placement, announced. That
> degrade changes no existing child, which is the reason to prefer it: a
> `LegacyTakes`-shaped one would wrap the user's faded clip into a column where
> `parametersOf` resolves to the ACTIVE take, so the fade would stop applying
> and the degrade would itself change what plays.
>
> **(v) NO GAP IS FILLED, and the plan never says so explicitly.** Every member
> of `T` intersects `[a, b)`, so the one child that can start before `a` must
> also end after it and therefore covers `[C.start, a)` entirely; the same
> holds at the other end, and the pass covers `[a, b)`. `C` is contiguous AND
> FULLY COVERED. That is what "contiguous by construction" above is worth: mode
> A, unlike `LegacyTakes` and `TakesSplit`, has no uncovered sub-span to place
> a cut over.
>
> **(w) ASCENDING CHILD INDEX IS LOAD-BEARING, and the obvious fixture cannot
> see it.** Step 4's "the TOPMOST (highest child index) one is the previous
> answer" is implemented by the ORDER of the merge's operand list, because §6.1
> gives an overlap to the LATER operand — not by the time order `columnsFor`
> returns. The two orders agree on every fixture whose clips were placed LEFT
> TO RIGHT, which is every natural one: `rec_union_overlap_legacy` was first
> written that way and the sabotage (sorting by start time) PASSED. It now
> places the later-STARTING clip FIRST, so child order and time order disagree,
> and the sabotage moves `extents`, `comp` and two `takeAtPos` answers.
>
> It moves no SAMPLE, and that is not a weakness of the gate but a property of
> the claim: the same fixture has to make the two clips COHERENT where they
> overlap for the "the sum is gone" half to be measurable as an exact halving,
> and coherent means indistinguishable. The topmost half is therefore carried
> by the structural assertions and the sum half by the render. One fixture
> cannot do both audibly, because without a stretch two clips' source positions
> either coincide everywhere they overlap or nowhere.

### 3.2 Mode `takes-split` (B) — the rule

Every member of `T` is split at `a` and at `b` where those fall strictly
inside it (`split-clip broadcast=0`); the pieces inside `[a, b)` become
columns with the old piece as take 0 and `R` (windowed to that piece) as take
1, active; pieces outside are untouched; uncovered sub-spans of `[a, b)` get
plain cuts.

> **"STRICTLY INSIDE" IS NOT `split-clip`'s PREDICATE, and the difference costs
> the whole take.** Found while building P1b (QBX-172).
> `SSplitClipAction::apply` refuses `inObjOffset <= 1` and
> `inObjOffset >= fullDur - 1`, so a cut needs TWO frames of material on each
> side; and `SCompositeAction::apply` rolls back every earlier child and
> returns false on the first one that fails. So a planner that emits a cut one
> frame inside a clip does not get a slightly wrong shape — it loses the
> recording, which is the one thing a recorder may not do. The planners guard
> with `pieceCanSplitAt()` and degrade to the no-split cell, whose take anchor
> then carries the ≤ 2-frame difference as leading silence (a negative
> `srcOffset`, legal since proposal 23). §3.5's snap removes almost every
> occurrence before a planner sees it — a landing within `N` of an edge is
> moved ONTO the edge — but only when every armed lane agrees, so with lanes
> disagreeing a one-frame landing is reachable. A column that is split carries its map with it (§7 B3); the
inside piece's map is cleared when `R` is activated (§7 B4). Takes are
alternatives for exactly the span they cover, so invariant 1 holds as written
today.

### 3.3 The matrix

Each cell: **structure · map · heard**. `prev` = the previous answer (§3.1
step 4). For Kc/Km, `O` stands for all of the column's takes, `j` for its
active take.

| cell | `takes` (A) | `takes-split` (B) |
|---|---|---|
| R1 · Kp | `C=[s,b)`; takes O`[0,e−s)`, R`[a−s,b−s)` · `0→O, a−s→R` · O to `a`, R to `b` | O split at `a`: plain `[s,a)`, column `{O₂/R₁}=[a,e)`, plain R cut `[e,b)` · (none) · O to `a`, R to `b` |
| R1 · Kc | takes O₀..Oₖ at `[0,e−s)`, R · `0→j, a−s→R` · take `j` to `a`, R | column split at `a` (both halves keep active `j`); inside half gets R active · cleared on the inside half · `j` to `a`, R |
| R1 · Km | O's map shifted by 0 (`s == C.start`) outside `[a,b)`, R inside · `M0 ∪ {a−s→R}` · O's comp to `a`, R | split carries the map (head keeps `< a`, tail shifted); inside half's map cleared by R's activation · `j`-less: O's comp to `a`, R |
| R2 · Kp | `C=[s,e)`; O`[0,e−s)`, R`[a−s,b−s)` · `0→O, a−s→R, b−s→O` · O, R, O | split at `a` and `b`: `[s,a)`, `{O₂/R}=[a,b)`, `[b,e)` · O, R, O |
| R2 · Kc/Km | as R1·Kc/Km plus the re-entry segment `b−s→prev` | split at `a` and `b`; three columns (outer two keep `j` / their map pieces) |
| R3 · Kp (`e == b`) | `C=[a,b)`; O, R both full · empty map, active R · R | `{O/R}` · R — identical to today |
| R3 · Kp (`e < b`) | `C=[a,b)`; O`[0,e−a)`, R full · `0→R` (= empty, active R) · R | `{O/R}=[a,e)` + plain R cut `[e,b)` · R |
| R3 · Kc/Km | O's takes at their extent, R full · `0→R` — O's map is replaced inside `[a,b)` and nothing of the column lies outside it, so none of it survives (it comes back with `undo`) · R | `{O₀..Oₖ/R}` with the map cleared (B4) + plain R cut if `e < b` · R |
| R4 · Kp | `C=[a,e)`; O full, R`[0,b−a)` · `0→R, b−a→O` · R to `b`, then **O** | split at `b`: `{O₁/R}=[a,b)`, plain O₂ `[b,e)` · R, O |
| R4 · Kc/Km | R`[0,b−a)`, O's takes full · `0→R, b−a→prev` (prev = `j` or O's map from `b−a` on) · R, then O's comp | split at `b`; inside column R active, map cleared; tail column keeps O's map shifted | 
| R5 · Kp | `C=[a,b)`; O`[s−a,e−a)`, R full · `0→R` · R | plain R `[a,s)`, `{O/R}=[s,e)`, plain R `[e,b)` · R |
| R5 · Kc/Km | O's takes at `[s−a,e−a)`, R full · `0→R` · R | GAP, `{O₀..Oₖ/R}` (map cleared), GAP · R |
| R6 · Kp | `C=[a,e)`; O`[s−a,e−a)`, R`[0,b−a)` · `0→R, b−a→O` · R to `b`, then O | plain R `[a,s)`, split O at `b`: `{O₁/R}=[s,b)`, plain O₂ `[b,e)` · R, O |
| R6 · Kc/Km | O's takes at `[s−a,e−a)`, R`[0,b−a)` · `0→R, b−a→prev` · R, then O's comp | GAP, column split at `b`, inside half R active (map cleared), tail keeps shifted map · R, O's comp |
| any R · Kw | the lane is REFUSED for this pass with a visible report (§7 B6); file listed as unused (Q13) | same |
| any R · Ko | treated as R0 with a warning (§7 B2); the recording lands as a plain cut or column over it, and the two never share a column | same |
| R0 · any | untouched | untouched |

> **"(= empty, active R)" IS NOW LITERALLY TRUE (P2, QBX-180).** R3's and R5's
> cells write the map as `0→R` and add "(= empty, active R)" as if the two
> spellings were interchangeable. They answer the same thing but only one
> SERIALIZES, and §4.3 already forbids writing the other on a `select-take`
> click for that reason. As a user gesture a merge whose last operand covers
> the whole result is rare; as the recording planner it is these two cells —
> every cycle pass after the first, and every pass over a clip lying wholly
> inside it — so mode A would have put a `<comp>` element into essentially
> every project that records twice. `merge-columns` therefore declines to write
> a degenerate map (one segment at 0 naming the take that becomes active, where
> that take covers the whole column). A small refinement to A1, required by P2,
> and asserted as `comp=""` in `rec_union_r3` and `rec_union_r5`.

**`overdub`**: one plain cut `[a, b)` on top of whatever is there; nothing is
split, wrapped or trimmed; everything sums. (Today's R1/R2 result made
uniform.) For MIDI it keeps its existing meaning: the pass is MERGED into the
active take of the first touched event column (`smidirecordactions.cpp:379-406`).

**`replace`** (tape): every member of `T` loses `[a, b)` — R1 trimmed to
`[s, a)`; R2 split into `[s, a)` and `[b, e)`; R3/R5 removed (through
`remove-sample`'s pinned inverse, plan 52, direct shape only — a wrapped
column refuses, so the lane is refused); R4/R6 trimmed to `[b, e)` (left-edge
`resize-clip`, source anchor advanced) — then a plain cut `[a, b)`. Never
wraps.

> **TWO CORRECTIONS from building it (QBX-172).**
>
> **(a) "direct shape only" HAS EXPIRED.** `remove-sample` has pinned BOTH
> shapes since QBX-155 landed (#241 `3ace4ba4`, #244 `19e75974`);
> `sremovesampleaction.cpp` says so in as many words. §12's QBX-155 row —
> "blocks `replace` and `cleanup-takes` on Kw lanes only; both refuse those
> lanes until it lands" — is satisfied. Both new planners still refuse a Kw
> lane, but that is now the G11 POLICY rather than this limitation: a surviving
> wrapper is non-foldable, both modes would have to split or delete it, and no
> gate has ever exercised one. Saying which reason applies matters, because the
> expired one would have been removed by QBX-155 and the real one will not be.
>
> **(b) IT IS BUILT AS `split-clip` + `remove-sample`, NOT as the left-edge
> `resize-clip` this paragraph and §12 prescribe.** A left trim has to name the
> piece's new SOURCE anchor, and the exact one is
> `SClipWindow::timelineToSourceExact( b - s )`, which runs through the clip's
> warp map — so `srcStart + (b - s) * stretch` is wrong for any warp-anchored
> clip. §5.1's rule 2 forbids a planner from reading the lane again to ask, and
> widening `Child` to carry `srcStart` plus `stretch` would carry exactly the
> arithmetic that is wrong. `split-clip` computes that anchor exactly, inside
> itself, and `comp_map_survives_split` already gates it — so cutting at `b`
> and removing the head yields the identical window with no new arithmetic
> anywhere. All four cells then reduce to ONE code path shared with
> `takes-split`: isolate the inside piece by cutting, then remove it instead of
> adding a take to it. `rec_replace_cells` asserts the surviving piece's
> advanced anchor, with the wrong value rejected beside it, because that is the
> only part a wrong implementation gets wrong silently. For MIDI it keeps its existing meaning: notes inside the pass window
are deleted from the active take, CCs survive (`:393-403`).

### 3.4 Composition rules

- **Several children in one pass, mode A:** all of `T` into ONE column (§3.1).
  The user's W1: `T = {A, B}`, `C = [0, end)`, takes A`[0,8.1)`, B`[8.1,end)`,
  R`[0,T)`; map `0→R, T→B`; heard R then old B. Compact view: one clip, badge
  `3/3`, the comp result drawn (QBX-157). The split at 8.1 is a cut mark, not
  a column edge.
- **Several children, mode B:** independent per child, left to right; the
  recording is windowed to each piece it covers. W1: `{A/R}`, `{B₁/R'}=[8.1,T)`,
  plain `B₂=[T,end)`.
- **Pre-existing overlapping children (Q7, D1):** A — all become takes of the
  one column; outside `[a, b)` the topmost plays. **The sum they used to
  produce is NOT preserved** — a column plays one take per position (D1,
  default (a)).
  B — each is split; the topmost inside piece is the column that receives R;
  the other inside pieces are left as they were (they summed before, they sum
  after; `takes-split` never deletes).

  > **"THE TOPMOST INSIDE PIECE" IS UNDERSPECIFIED FOR A PARTIAL OVERLAP, and
  > P1b does something else (QBX-172).** It is well defined only when one piece
  > contains another. Where two pieces overlap PARTIALLY, the rule names no
  > answer for the part of `[a, b)` the topmost piece does not cover — and
  > leaving that part unplaced loses recorded audio. `TakesSplit` therefore
  > walks the touched children LEFT TO RIGHT with a cursor and gives the pass to
  > the FIRST piece covering each position, placing it exactly once. That
  > agrees with this bullet wherever it is precise (W1's adjacent children,
  > gated by `rec_split_w1`) and never loses audio where it is not. Which piece
  > *should* win depends on decision D1, which P2 owns — so this is deliberately
  > left as a deviation to be settled with mode A rather than guessed at now.
- **Cycle record (Q8):** every pass is one span `[cycleIn, cycleOut)` against
  the lane as the previous pass left it. A — pass 1 forms the union column;
  pass 2+ touch that column (R3 or R4) and join it: ONE column per cycle, as
  many takes as passes plus the old material, every take's extent inside the
  cycle. C8 (`C=[6,20)`, cycle `[0,16)`): `C'=[0,20)`, takes C`[6,20)`,
  R1`[0,16)`, R2`[0,16)`, map `0→R2, 16→C`. X8 (narrowed) holds: no take
  extends beyond the cycle. B — two columns per cycle when a clip starts
  inside it (G3), accepted.
- **Multi-track band punch:** `(a, b)` are decided ONCE per pass by the
  recorder (§3.5) and every armed lane is planned against the same span. Lane
  structures still differ by what each lane held (A: one column per lane that
  had material, a plain cut where it had none; B: identical EDGES on every
  lane that had material).
- **Un-armed edit-group members (Q5, takes modes only; D4 default (a)):**
  the GROUP extent `C_g` is the union of the armed members' `C`s for this
  pass. A — ONE rule for EVERY member, armed or not: the touched set for the
  pass is "children overlapping `C_g`, split at `C_g`'s edges where they
  cross them" (reversible, the mode-B primitive), and the column is exactly
  `C_g`. An armed lane whose own `C` is smaller than `C_g` therefore also
  pulls in children that were R0 for its own span but lie inside `C_g` — they
  become takes at their extent, not left summing, not refused. An un-armed
  member gets the same column with no new take; with a single touched child
  this needs invariant 3′ (§4.2) — a one-take column whose take extent is
  smaller than the column. Every member then holds a column with the same
  `startTime` and duration and `correspondingClip` finds the twins.
  B — split at `a`/`b`, nothing stacked. `overdub`/`replace` never touch an
  un-armed lane (G9). A MIDI-armed member of an audio lane's group is a no-op
  for the audio recorder (kind filter), and the reverse.
- **Audio and MIDI in one press:** one undo step (§7 B7).

### 3.5 The sliver rule (Q4, G1, G2)

`N = max( one device buffer, 10 ms )` in project frames — a constant inside
`SAudioRecorder`, never a setting. Decided once per pass by the recorder, in
PHASE 1 of the stop (§5.4: after `endCapture()`, the no-anchor fallback and
`removeGrowingClips_()`, once `firstPlace` is known and before the segments
are planned), for `a` and for `b` separately:

1. Candidates for `a`: every child edge (start or end) on every armed lane —
   and, in takes modes, on every un-armed member of an armed lane's edit
   group — within `N` of `a`, skipping any child whose object
   `isLiveRecording()` (a growing clip of this or another recorder; they are
   removed before the plan, §5.4, and excluded here as well so a MIDI-only
   press coinciding with another lane's live clip cannot regress it). If the set is non-empty and every candidate is
   the SAME frame, `a := that frame`; otherwise no snap. Same for `b`.
2. The snap moves the clip WINDOW only; the file is untouched. `a` later by
   Δ: `srcOffset += Δ` (the head stays in the file; Alt-slip reveals it).
   `a` earlier by Δ: `srcOffset −= Δ` — into the pre-roll handle when the
   capture has one, else ≤ N frames of leading silence (a negative
   `srcOffset`, legal since proposal 23). `b` earlier: `length −= Δ`. `b`
   later: `length += Δ` (runs past the source by ≤ N: silence, which
   `place-clip` already tolerates).
3. Punch and cycle spans are the markers; the rule does not run.
   > **AND G12 — "the transport shows 'cycle overrides punch' while both are
   > set" — NAMES A STATE THE MODEL CANNOT REPRESENT (QBX-172).** The project
   > has ONE range (`RangeStart`/`RangeEnd`/`RangeValid`) plus a `Cycle` bool,
   > and `SAudioRecorder::punchRegion_()` returns false whenever
   > `cycleRegion_()` succeeds — punch IS "a range, with cycle off".
   > `SProjectProps` has no Punch property, so there is no second flag to set
   > and nothing for the transport to report as overridden. What CAN be gated
   > is the precedence itself, which the recorder already reports: with a range
   > set and cycle on it logs `[cycle]` and must never log `[punch]`.
   > `record_loop_takes` now asserts both halves, the absence with
   > `minCount="0" maxCount="0"`, and the assertion was verified by SABOTAGE —
   > removing `punchRegion_()`'s yield makes it fail — because a
   > characterization gate over behaviour that already works is otherwise
   > indistinguishable from a vacuous one. The UI half of G12 has nothing to
   > show; what the transport genuinely lacks is any indication of WHICH
   > reading of the range is in force, and that belongs with the transport
   > selector (QBX-170).
4. Logged at info: `[REC] snap a by -144 to 388800 (3 lanes agree)` or
   `[REC] no snap for a: candidates 388800, 388944`.
5. **It breaks an identity the suite relies on, and the recorder has to report
   the delta.** Not foreseen here; found while building P1a (QBX-171).
   `assert-recorded-clip` checks, on every plain single-pass take, that the
   clip's link start equals `placementFrame( trimmedFrames() )` — the one
   assertion in the suite that catches a placement term reported correctly and
   never applied. A snapped pass lands deliberately somewhere else. So
   `SAudioRecorder` exposes `snapDeltaA()` and the check expects
   `placementFrame( trimmed ) + snapDeltaA()`, which still catches exactly what
   it caught before and now covers the snap as well. Without the term every
   snapped take would fail there — and, worse, an EXISTING case whose landing
   happens to come within `N` of a neighbouring clip's edge would fail
   intermittently, which is the shape this project has twice spent a session
   chasing.

This is G2's answer in the user's words: the recording's head is trimmed from
the WINDOW, not from the source.

**The snap IS a flip, and §8's "P1 — Flips: none" does not cover it.** That
sentence is true of the PLANNER — `takes` still runs `LegacyTakes` — but §3.5
deliberately changes where a pass LANDS next to existing material, so any
recording whose landing comes within `N` of a clip edge changes outcome.
Measured while building P1a: `record_stays_armed` took both its passes from
locator 0, which puts the second landing a few thousand frames from the first
pass's clip while `N` is about 1024 frames here. The snap fires 3/3, the second
pass begins exactly where the first pass's clip does, `LegacyTakes` duly makes
it a TAKE on that clip, and the lane holds ONE child instead of two.

That is the comping this plan wants. It is not what that case is about, and
asserting it there would tie the case to the jitter staying under `N` — so the
case's second locator moved out of range instead, which also removes a flake it
already had. The lesson generalises: **a case whose claim is about arm or
transport state must keep its passes out of snap range of each other**, and
§9's flip list should be read as covering the comping semantics only.

## 4. Model changes mode A needs

Mode A is the default, so these are prerequisites, not tidy-up. Review 1
found that the first draft relaxed invariant 1 and left five load-bearing
consequences to the implementer; they are spelled out here, each with the
code fact that forces it.

### 4.1 Take extents (invariant 1 → 1′) — **SHIPPED as A0a (QBX-177)**

> **SIX CORRECTIONS FROM BUILDING IT.** The rules below are what landed; these
> are where the section was wrong, underspecified, or right about a defect and
> wrong about how to gate it.
>
> **(a) `normalizeColumns` IS A TAKE-GEOMETRY CONSUMER AND THE SECTION DOES NOT
> LIST IT.** The post-load fold (proposal 42 M4) writes the WRAPPER's window
> onto every take — so under 1′ it must also set the column's new
> `duration_`, or the folded column keeps the length it LOADED with and reports
> every take as partial. Measured: `takestack_wrap_takeslip.qxp` loads a 192000
> column behind a 96000 wrapper, and without the fix the folded column read
> `extents=[0-96000;0-96000] duration=192000`, routed a plain legacy column
> through `twCompColumn`, and `take_lane_click_wrapped`'s render came back half
> take 0. The fold now also REFUSES a column that already has a partial take,
> like every other inexact fold. This is the one place where 1′ could have
> changed what a years-old project sounds like.
>
> **(b) THE INVALIDATION GATE CANNOT BE BUILT FROM `resize-clip take=`.** The
> bullet is right that three verbs are affected; it is wrong that a slip can
> show it. `resize-clip` on a direct column calls `applyWindowAll` FIRST, and
> that publishes `durationChanged` for the whole column before the named take
> is touched — `slip-clip`'s own documentation already said so ("`resize-clip`
> and a released `drag-clip-edge` both commit through `setWindow`, whose
> `durationChanged` reaches `twTrackMix::updateClip` and stales the extent
> regardless"). And `set-clip-volume take=` / `set-clip-fade take=` go only
> through the dirty-range walk, as the bullet says, but their value is applied
> ABOVE the column: `STrack::refreshClipGainCurves()` reads the clip's gain,
> which for a column is the ACTIVE take's. MEASURED: −20 dB on a comped
> INACTIVE take left the render bit-identical even with 1′ in place. That is a
> real hole — per-take gain is inaudible on a comped column — and it is NOT
> A0a's; it belongs with per-region gain (P5). The gate is built on
> `slip-clip`, which gained `take=` for it.
>
> **(c) NO `bumpRenderChainEpochRange` OVERRIDE ON THE COLUMN, measured.** It
> looks obviously required — the column owns a `twCompColumn` whose per-take
> `twView`s hold page caches, and the base default does nothing — and it is
> redundant: `STrack::bumpRenderChainEpochRange` already invalidates
> `cpTrackMix_` over the same range. Added on a wrong diagnosis, then
> SABOTAGED: with the push removed all five A0a gates still pass, including the
> invalidation one. What made the wrong diagnosis look right was the SLIP
> THROTTLE (`SCut::SLIP_INVALIDATE_MS`, 333 ms, one immediate shot per process
> which `add-take startOffset=` had already spent): the model moved and the
> render did not, which reads exactly like a missing invalidation.
>
> **(d) §4.1's "W1's column split at 4 s" IS A NUMBER THAT DOES NOT FIT ITS OWN
> EXAMPLE.** §3.4's W1 has A = `[0, 8.1)`, so a cut at 4 s falls INSIDE A and
> yields head `{A, R}` / tail `{A, B, R}`. The outcome this bullet states —
> head `{A, R}`, tail `{B, R}` — needs the cut AT A's end, which is what a
> split between two adjacent children is. The outcome is the specification;
> `take_extents_split` gates it at this suite's scale.
>
> **(e) "THE INVERSE RESTORES THE MOVED TAKES BY IDENTITY" IS NOT ASSERTABLE.**
> No verb in the suite can see object identity — `placements` and
> `linkObjectRefs` read the column and the placement, never a take — and a
> clone carries the window, slip, fades, volume and envelope faithfully, so no
> render separates a move from a clone either. The gate asserts every
> observable consequence (order, extents, duration, per-take slip, both maps,
> a byte-identical render) plus `drain-pending-deletes`. The move is still the
> right implementation, for plan 52's reason; it is simply not what the gate
> proves.
>
> **(f) `assert-file-contains` WAS UNDOCUMENTED ENTIRELY**, not just its
> `absent=` attribute (§8's parenthesis and review 2 m4 both say the
> attribute). A0a adds the whole `docs/ACTIONS.md` row.
>
> Two scope calls recorded rather than assumed: `hasPreview`/`getPreview` move
> with the PAINTERS in A0b (a preview is what a painter reads; windowing one
> while the painters still draw across the whole column makes a half-windowed
> picture), and `resize-clip`'s left-edge loop still overwrites a partial
> take's extent — unreachable today, noted in the code, and A0b's to fix
> together with the EMPTY-refusal.
> **BOTH DONE IN A0b (QBX-178)**, together as the note required.

Today (`stakestack.h` invariants): every take shares the column's duration;
take-link `startTime` is always 0; and **the column has no length of its
own** — `STakeStack::getDuration()` / `getDurationBlocking()` answer the ACTIVE
take's duration, else take 0's (`stakestack.cpp`, both getters), and every
consumer of the column's length reads that: the `durationChanged` emitters,
`add-take` sizing, `split-clip`, `place-recording`, `resize-clip`,
`correspondingClip`'s exact-equality twin match, and `twTrackMix::updateClip`
through `STrack::trackChildDurationChanged`. New:

- **1′.** Every take has an extent `[tStart, tEnd) ⊆ [0, duration_)`, stored
  as the take link's `startTime` and the take window's duration.
  **`STakeStack::duration_` is a NEW stored field** — without it W1's column
  (active R over `[0, T)`) would report length `T` and old B's tail would
  never be rendered, and Q5's one-take placeholder (§4.2) is not
  representable at all. Both getters return `duration_`; every
  `durationChanged` carries it; `setDurationAll`/`applyWindowAll` write it.
  Serialized as `duration='…'` on the `<STakeStack>` element ONLY when it
  differs from `max_i( tStart_i + takeDur_i )`, and derived as that maximum
  when absent on load — which is every existing file's value, so the
  degenerate case (full-extent takes) re-serializes byte-identically.
- **The take link's `startTime` is always written (`SLink::hasStartTime()`
  is unconditionally true; `serializeSelfAttributes`), today always `0`, and
  the loader DISCARDS it:**
  `STakeStack::instantiateFromDomElement` re-inserts every take through
  `insertTake`, which forces `setStartTime( 0 )`. So `insertTake( window,
  atIndex, tStart, tDur )` gains the extent (defaults = the whole column, so
  every existing caller is unchanged), and the loader reads the child
  `<SLink>`'s `startTime` and the take's own duration into it.
- **Resolution rule — one place honours extents.** A column with ANY partial
  take resolves through `twCompColumn` even when its map is empty. Today the
  empty map takes the ORIGINAL path — `getRootComponent`, `resolveClip`,
  `mapTimelineToComponentPos`, `seekTo`, `resolveEventClip`, `hasPreview` /
  `getPreview`, `windowStep` (`stakestack.h`, "the step is the identity into
  that take") and the renderer's `collectEnvelope` / `drawTake` all hand the
  column position straight to the active take, and `ensureCompColumn`'s
  resolve lambdas pass `p` through unshifted. Under 1′ that path is correct
  only when the active take is full-extent, so `STakeStack::hasPartialTake()`
  (cached, maintained by `insertTake`/`removeTake`/the write-through) gates
  it. What each delegate does for a partial take:
  `twCompColumn::setInputExtent( i, tStart, len )` (written under `mutex()`
  like `setCompMap`, read in `runAt_nolock`/`planPage`) shifts the input's
  position by `tStart` and contributes nothing outside its extent — a run
  whose selected take has no material declares NO dependency and freezes
  nothing, so no reader is built for a position outside the window;
  `windowStep` subtracts `tStart` and returns false outside the extent;
  `hasPreview`/`getPreview` and `collectEnvelope`/`drawTake` are
  extent-windowed (nothing outside); `seekTo` and
  `mapTimelineToComponentPos` go through the comp column; `resolveEventClip`
  is P3's (§8) and **was** extent-shifted there — and only shifted: it still
  resolves through the ACTIVE take alone, so an event column plays silence
  wherever that take has no material instead of the take its comp map names
  (QBX-189; this seam returns one sequence per clip and two takes are two
  sequences, which is this bullet's own "the event path has no `twCompColumn`
  to do that in"). `twCompColumn::clampedXfade_nolock`
  additionally clamps a crossfade to BOTH takes' extents (W1's boundary at
  `T` has no outgoing material past it).
- **Invalidation and resync.** `STakeStack::onTakeCutChanged` forwards
  `durationChanged` only when the sender is the ACTIVE take, and
  `mapChildRangesToSelf` returns `{}` for every inactive take ("only the
  active take reaches the mix"). Under A the inactive old take is audible
  from `T` on, so a later `resize-clip take=` / `set-clip-volume take=` /
  `set-clip-fade take=` on it would bump no epoch and invalidate no range.
  Fix: forward for EVERY take (simplest, and the cost is a resync the mix
  already debounces), and in `mapChildRangesToSelf` shift the child's ranges
  by `tStart` and intersect with the extent. Pre-existing since 43 N2 for
  hand-made comps; after mode A it is every recording over material.
- **`split-clip` on a column with partial takes.** The direct branch trims
  EVERY take to the split (`ssplitclipaction.cpp`, `setDurationAll(
  inObjOffset )`) and `unsplit-clip` re-grows every take through
  `setDuration( restoreDuration_ )`. Under 1′: a take wholly on one side
  belongs to THAT half only; a take crossing the split is windowed into both;
  take indices are renumbered PER HALF and each half's map is remapped to the
  new indices (so B3's "head keeps `< split`, tail shifted" becomes "… and
  re-indexed"). The inverse is MIXED, because the forward is: a take wholly
  on one side is MOVED (same window object; one wholly in the TAIL has its
  `tStart` shifted by `−inObjOffset` as it moves, and the inverse shifts it
  back and re-links it at its original index — plan 52's pattern), while a
  take crossing the split
  is, as today, trimmed in the head and CLONED into the tail
  (`cloneWindowOver`), so the inverse deletes the tail's clones and re-grows
  the head's crossing takes (today's `SUnsplitClipAction` body), then moves
  the wholly-one-side takes back, and restores BOTH halves' maps and the
  original take ORDER. Each half's `activeTake_`: the active take if it has
  material in that half (re-indexed); otherwise the half's lowest-index take
  that has material at the half's start — a half may not come out with an
  active take that is silent everywhere in it. W1's column split at 4 s:
  head `{A, R}`, tail `{B, R}` — nothing empty, nothing refused.
- **`resize-clip` on the column** (the write-through, `setDurationAll` /
  `applyWindowAll`): a column edge move CLAMPS extents — a right-edge trim
  shortens every take that reaches past the new end; a right-edge extend
  grows only the takes whose extent ENDED at the old end (they were clamped
  by the column; a take that ended earlier was not). Mirror for the left
  edge, moving the source anchor as `resize-clip`'s left edge does today. A
  trim that would make any take EMPTY is **refused** with a message naming
  the take (the QBX-146 rule: refuse rather than destroy; the user removes
  the take first). The refusal applies to `resize-clip` ONLY, never to
  `split-clip`.
- **`remove-take`** (today `SRemoveTakeAction::apply` → `STakeStack::removeTake`)
  must (i) renumber the comp map — neither touches `compMap_` today, so
  removing take 0 of a comped column leaves every segment pointing one take
  off (**B10**, §7, a pre-existing hole mode A makes common), and (ii) carry
  the removed take's extent in its inverse — the inverse is
  `SAddTakeAction( filePath, off, index, active, stretch, pitch )` with no
  extent, so undoing a `remove-take` would restore a FULL-extent take. `add-take`
  gains `at=`/`length=` (column frames; defaults = whole column) and the
  inverse uses them.
- **Serialization and version.** No new element and one new attribute:
  `duration=` on the stack, written only when informative; a take link's
  `startTime` is written as today (always), now non-zero for a partial take.
  `FORMAT_VERSION` 2 → 3, because an older
  build that reads such a file sets the COLUMN's length to the active take's
  partial length (its `getDuration()`), so the column would shrink and grow
  with take selection; the loader only WARNS on a newer version and reads
  anyway, so the bump is what makes that build say so. This flips
  `load_unknown_object_survives.qxa:44-47` (asserts a legacy file re-saves
  as `formatVersion='2'`), edited in the same PR with the reason in its
  header; `load_missing_sample_placed_survives` declares `formatVersion='99'`
  and is unaffected.
- **`cloneColumn`**, `duplicate-clip`, `resize-clip take=`, the take-lane slip
  and the four testkit asserts read the take's extent instead of assuming the
  column's. Proposal 42 counted 43 consumers of the column shape; the extent
  has fewer (it is a property of the take link, and most consumers go through
  `takeAt`), but the list must be MEASURED in the PR, not estimated here, and
  it includes the nine delegates named above.
- **`assert-take-column`** gains `extents="0-192000;96000-192000"` (per take,
  column frames) and `duration=`; the existing `takeAtPos=`/`expectTake=`
  keep working.

### 4.2 Invariant 3 → 3′

A column with ONE take collapses to a plain cut only when that take's extent
is the whole column. A one-take column with a partial take is legal; it is the
"union placeholder" Q5 produces on an un-armed member (§3.4), and it is what a
column becomes when the user removes all but one partial take. The painter
shows the uncovered part hatched (§4.4). Its length is `duration_` (§4.1), so
it is representable.

### 4.3 `select-take` becomes extent-aware — **SHIPPED as A0b (QBX-178)**

Today: "this take for the WHOLE column", map cleared, inverse restores
(`sselecttakeaction.cpp:68-80`, `docs/ACTIONS.md:291`). Under 1′ a take may
not cover the column, so: `select-take` selects the take **wherever it has
material** — segments inside `[tStart, tEnd)` are replaced by `tStart→take`
(and the previous answer re-enters at `tEnd`), segments outside are kept,
`activeTake_ := take`. For a take whose extent is the whole column this
reduces to exactly today's clear. One undo of a click still restores the whole
previous map. The take-lane click (`SMVActualView::mousePressEvent`'s take-lane
branch) needs no change.

**Correction (g), from the build.** Two things this section does not say, both
load-bearing:

- The full-extent clear is **not an optimisation and must stay a branch**. A
  one-segment map `{0→take}` answers exactly what an empty map plus
  `activeTake_` answers, but it SERIALIZES — so a click on an ordinary
  un-comped column would start writing a `<comp>` element into project files
  that never had one, against the standing rule that a new serialized feature
  writes nothing when unused.
- "One undo of a click still restores the whole previous map" needed a NEW
  FLAG to stay true. An inverse names the PREVIOUSLY active take, which may
  itself be partial, so under this section's rule an inverse can no longer be
  told from a forward pass by the take it names: it would recompute the map
  above instead of restoring the one it was handed.
  `SSelectTakeAction::setPreviousCompMap()` now also marks the action as an
  inverse, and that mark — not the take's extent — is the discriminator.

### 4.4 The painter and QBX-157 — **SHIPPED as A0b (QBX-178)**, except the pixels

- Compact: the column draws the COMP RESULT — per segment, the selected
  take's envelope over that span; a span whose selected take has no material
  is hatched, as `no take` is drawn today. This closes X10 and is QBX-157's
  part 1; whichever PR lands first owns `STakeStackRendererInline::draw` and
  `collectEnvelope`. **A0b landed first and owns both**, plus
  `drawTake`/`collectTakeEnvelope`.
- Expanded: each take row draws its material over its extent only.
- Cut marks at comp boundaries and column edges: QBX-157 part 2.

**Correction (h), from the build.** The expanded row's parenthesis — "the
comp-aware per-region paint already exists; it learns the extent" — understates
it twice:

- The per-region walk knew only the MAP's segment boundaries, so it had to
  learn to break at EXTENT EDGES as well, or a region straddles the end of its
  own take's material and gets dimmed or ringed on the strength of its other
  half. That walk is now one shared call (`STakeStack::nextCompEdgeAfter`),
  which is also what the compact painter and the model answer with, so the
  three cannot disagree about where a take begins.
- It cited the paint by LINE NUMBER (`sstdmixerview.cpp:2459`), which this
  repository's own rule forbids precisely because the numbers rot; the function
  is `SMVActualView::drawTakeLane`.

**And a note this section needs:** `nextCompEdgeAfter` deliberately ignores
CROSSFADES. A crossfade is a BLEND of two takes that a per-column envelope
cannot express and a waveform cannot show; `twCompColumn` splits runs at its
two ends because it has to MIX them, and the boundary a picture would draw is
QBX-157's cut mark. A painter that split there as well would claim a visible
edge the picture does not have.

## 5. Architecture

### 5.1 The planner interface

New, in `objects/cut` (`srecordplanner.h`), because that is where
`place-recording`, `add-take`, `split-clip` and the column helpers live and
where the layering checker allows it:

```cpp
namespace srecplan {

enum class Mode { Takes, TakesSplit, Overdub, Replace };

struct Child {            // one lane child, read once at plan time
    int        index;     // child index on the lane (the path's last step)
    offset_t   start;
    length_t   dur;
    enum { Plain, Column, MappedColumn, WrappedColumn, OtherKind, Lane } kind;
    int        nTakes, activeTake;
    twCompMap  map;
};
struct LaneSnapshot { QList<int> lanePath; QList<Child> children; };

struct Span {             // what the recorder hands over, AFTER the pass-wide snap
    QString  filePath;
    offset_t a, b;        // placed span, project frames
    offset_t srcOffset;   // signed; negative = leading silence
    bool     fromCycle;   // X8: a cycle take must not extend beyond [a,b)
};

struct Context {
    Mode  mode;           // resolved for THIS lane (§5.2)
    bool  armed;          // false for an un-armed edit-group member (Q5)
    SContentKind kind;    // Audio for the audio recorder, Event for MIDI
};

class Planner {
public:
    virtual ~Planner() = default;
    /// Appends the model actions for ONE lane to `out`, in apply order.
    /// Returns false and sets `why` when the lane cannot take the pass
    /// (a wrapped column, a system lane); nothing is appended then.
    virtual bool plan( const LaneSnapshot &, const Span &, const Context &,
                       SCompositeAction &out, QString *why ) const = 0;
};

const Planner &plannerFor( Mode );        // Takes, TakesSplit, Overdub, Replace
LaneSnapshot   snapshot( SObject *lane );  // the ONE reader of lane state
}  // namespace srecplan
```

**What P1a (QBX-171) built instead, in three places.**

- `snapshot()` takes **the lane path as well as the lane**. An `SObject`
  cannot name its own path: the tree is `SObject -> SLink -> SObject` and only
  the links point down, which is the same fact that makes `resolveMode` below
  and `ssolo::underSoloedAncestorRec` walk DOWNWARD from the root. The caller
  already holds the path — it is how it found the lane — so asking for it
  costs nothing, where deriving it would cost a walk of the whole tree per
  recording.
- `Child` carries a **`contentKind`** and `OtherKind` is decided by the PLAN,
  not by `snapshot()`. Which kind is "other" is not a property of the child but
  of the pass: an event clip is other-kind to an audio recording and is the
  whole point of a MIDI one.
- **A wrapped column is not refused by `LegacyTakes`.** G11 below says "refused
  by every planner" and §8's P1 row says "Flips: none"; both cannot hold, because
  today's planner hands a wrapped column to `add-take` like any other, so
  refusing it would BE a flip. The refusal is the new planners' rule;
  `LegacyTakes` keeps the old behaviour until P2 retires it.

Rules the interface carries:

- **A planner emits existing verbs** (`place-clip`, `add-take`, `split-clip
  broadcast=0`, `set-comp-map`, `resize-clip`, `remove-sample`,
  `merge-columns` §6.1) into one `SCompositeAction`. It never mutates the
  model itself, so every planner's undo is the composite's inverse chain and
  every planner is testable by the verbs it emits.
  > **AND THAT COMPOSITE MUST BE HELD BY THE ACTION, not built on the stack
  > (QBX-172).** Not foreseen here; found as a SIGBUS the first time `replace`
  > removed a take column. A child's inverse may BORROW the child:
  > `remove-sample` cannot rebuild a column from parameters, so it PINS the
  > content and returns an `SRestoreTakeColumnAction` whose only link back is
  > `owner_`, its own header calling it "holds the pin; borrowed". A stack
  > composite dies when `apply()` returns, deleting the child and dropping the
  > pin while the inverse sits on the undo stack pointing at freed memory.
  > `SPlaceRecordingAction::plan_` holds it instead, which is right by
  > construction because `SActionUndoCommand` owns the forward action and the
  > inverse together and destroys them together. It was LATENT on `main`, not
  > live: `remove-sample`'s only two call sites are the arranger's Delete and
  > `add-sample`'s inverse, both owned by the action history, so P1b is simply
  > the first caller to put one in a composite. Any later planner emitting a
  > verb with a pinned inverse inherits the same requirement.
- **Index bookkeeping is the planner's, in apply order (G10).** Lane child
  indices are INSERTION order, not time order (`SObject::childLinks()`;
  `wrapCutLinkIntoStack` needs `moveChildToIndex` for exactly that reason).
  `split-clip` and `place-clip` APPEND links; `add-take` preserves its
  operand's index; `merge-columns` keeps its FIRST operand's index and
  REMOVES the others, so every later index shifts down. The planner therefore
  keeps a simulated copy of the lane's child list, applies each emitted verb
  to it (append / remove / keep) and computes every later path from the
  simulation; removals inside one `merge-columns` are issued in DESCENDING
  index order. The old planner's "paths computed NOW stay valid" comment is
  false once a split or a merge is in the plan; the comment goes with the
  code. `unmerge-columns` restores every original child index with
  `moveChildToIndex`.
- **`place-recording` becomes a thin verb**: `mode=` attribute (one of the
  four; default = the lane's resolved mode, §5.2) and `armed=` (`"1"`
  default; `"0"` marks an un-armed edit-group member, for which the takes
  planners emit the structure-only plan of §3.4 and never place the file).
  It builds the snapshot, picks the planner, applies the composite.
  `place-midi-recording` takes the same attributes; its old `new-take` stays
  accepted as an alias of `takes`.
- **`snapshot()` CLASSIFIES and never mutates.** A child whose object
  `isLiveRecording()` is skipped outright (never a column, never an edge).
  `OtherKind` children are reported as R0 with a warning (B2). A wrapped column is reported as
  `WrappedColumn` (Kw) and refused by every planner (G11) — `snapshot()` does
  NOT call `stakes::normalizeColumns`: that function takes the whole
  `SProject&`, relinks and deletes wrappers outside any action, and is a
  post-load pass; running it inside an `apply()` would put a non-undoable
  mutation in the middle of an undo step. After the post-load pass any
  surviving wrapper is non-foldable anyway, so classification loses nothing.
- Future modes (Cubase "cycle history + replace", REAPER "new lane") are one
  more `Planner` and one more enum value; nothing in the recorder changes.

### 5.2 Where the mode lives (Q2)

| level | storage | verb | default |
|---|---|---|---|
| project | `SProjectProps::RecordMode` (string, one of the four) | `set-property key="recordMode" value='["takes"]'` (exists; NON-undoable — `SSetPropertyAction::apply` returns `{true, nullptr}` — and a qxa value is a JSON array, as `feel_flow_trained.qxa`'s `rangeStart` shows) | `SProjectProps::defaults()` says `takes`; the INI seed overrides it at creation (below); a LOADED file without the key reads `takes` |
| folder track ("track group", D2 default (a)) | `STrack::recordMode_` on a lane with `STrack::hasChildTracks()`, serialized only when set (the `monitorMode` pattern, `strack.cpp:122-123`) | `set-track-record-mode trackPath= mode=` (`""` = inherit), undoable | inherit |
| track | the same attribute on the track | the same verb | inherit |

Resolution for a lane (`srecplan::resolveMode( lane )`): its own attribute,
else the nearest ancestor with `hasChildTracks()` (innermost wins), else the
project's. `collectArmed` already recurses through folders, so the ancestor
chain is at hand when the recorder builds the per-lane `Context`.

**Where the INI seed runs.** Not in `SProjectProps::defaults()` — that is in
`model`, and `model` may not include `servicesui`, where `SOpt` lives
(`tools/check_layering.py`, `'model': set()`). It runs where the shell
constructs a project that will NOT be loaded into — `SMainWindow::fileNew()`,
`SMainWindow::newProject()` and the `close-project` verb's fresh project
(`scloseprojectaction.cpp`) — as one helper in `servicesui` that reads
`SOpt::MidiRecordMode` and calls `setProp( RecordMode, … )`: `new-take`→`takes`,
`overdub`→`overdub`, `replace`→`replace`; an absent or unknown value → `takes`.
**Never in `fileOpen()`**: its `new SProject()` is the empty shell the loader
fills, and property loading MERGES over `defaults()`
(`SProject::readPreChildrenAttributes`, `properties_.insert( key, value )` per
loaded key), so a seed written there would survive for every file that lacks
the key — i.e. every existing project would silently read the user's INI
value instead of `takes`. `record_mode_ini_seed.qxa` asserts the negative:
with the INI at `replace`, a loaded fixture without the key reads `takes`.

The transport bar gets a four-way selector bound to the PROJECT property; a
track whose resolved mode differs from the project's shows it in its head
(the way a non-Auto monitor mode is shown). `midi/recordMode` stays in the
INI as the new-project seed and is written by the same selector only through
an explicit "Use as default for new projects" item. **INI-key ownership:**
`set-option`'s rule is ONE owning case per key (`docs/ACTIONS.md:322`).
`record_mode_ini_seed.qxa` (§8 M0) becomes that owner; `midi_record_modes.qxa`
is rewritten to drive the PROJECT property only and touches no INI key.

Per-user preferences (not undoable, `SOpt` keys, `set-option`):
`transport/locatorAfterRecord = return | stay` (Q12, default `return`);
`takes/expandOnLanding = never | landing | armed` (Q11, default `landing`).
Project properties (travel with the song): `recordMode`, `autoFadeMs` (Q9,
default 3.0 — seeded at creation; a file without the key reads 0, see §10).

### 5.3 How the recorder hands over

The audio recorder's PHASE-1 planner (§5.4 — today's `commitPlacement_`
body up to the segment list `segs`, moved into `endCapture()`) keeps that
segment arithmetic, then: (1) applies §3.5 to each segment's `(a, b)` — for a
cycle or punch pass the markers are already exact; (2) in `commit()` (phase
3), for every armed lane resolves
the mode (§5.2) and submits ONE `place-recording` per lane per segment with
`mode=` set. `SMidiRecorder::commit_` does the same through
`place-midi-recording`. **From P4 on** (not before — §8 M5), in takes modes
it also enumerates every un-armed member of an armed lane's edit group and
submits the pass with `armed="0"`, which the two takes planners turn into the
structure-only plan of §3.4 (A: the `C_g` merge; B: the split at `a`/`b`) and
`overdub`/`replace` treat as nothing to do. Until P4 every planner, including
`LegacyTakes`, treats `armed="0"` as nothing to do.

### 5.4 Undo atomicity (B7, a contract change)

> **WHAT B7 ACTUALLY SHIPPED (QBX-165), and why the rest of this section is
> history.** The two-phase stop below — a per-recorder phase-1 plan, a
> `willCommit()` that reports its emptiness, and a conditional outer
> `beginMacro` — was not built. It is not needed: `SApplication` grew a
> **re-entrant record-commit scope** (`beginRecordCommit()` /
> `endRecordCommit()`, `recordCommitDepth_`) that opens the macro **lazily**,
> on the first `submitAction` inside it, so a pass that places nothing leaves
> the undo stack untouched without anyone having to predict in advance whether
> it will. That is strictly stronger than a conditional macro: it needs no
> `willCommit()`, so it cannot be wrong about its own emptiness, and it closed
> a hole the design had — `SAudioRecorder`'s punch-out calls its own `stop()`
> from a tick, never reaching `SApplication::stopRecording()`, so a macro
> opened only there would have left every punch-out placement unwrapped, one
> undo step per segment per track. The scope is re-entrant precisely so both
> recorders may open it.
>
> What survives from this section and is load-bearing for P1: **the growing
> clips leave in phase 1, before the plan and before the snap** (§3.5), which
> is where `removeGrowingClips_()` already sat; the accepted limit that a pass
> whose every `place-recording` is refused at apply leaves an empty macro; and
> the rollback rules in the last paragraph. The `willCommit()` design below is
> kept because it records the reasoning that the lazy scope replaced.

One press is one undo step, and **the stop becomes TWO-PHASE** so the macro
can be conditional without planning against a half-closed capture.
`SAudioRecorder::stop()` today runs `bridge_->endCapture()` FIRST ("the paths
place-recording is about to resolve are complete files"), then
`content_->freeze()`, then the no-anchor fallback (which rewrites
`placement_.p0` and sets `trimmed_ = 0`), and only then `commitPlacement_()`,
which reads the FINAL `total`; a segment list planned before `endCapture()`
would miss whatever arrived between the plan and the close and, on the
no-anchor path, plan against the wrong placement — i.e. lose the tail of
every take. So `SApplication::stopRecording` does, in order:

1. Phase 1 of EACH recorder begins exactly as its `stop()` does today —
   `active_ = false`, `timer_->stop()` — so `SApplication::isRecordingActive()`
   (the OR of the two, which gates the locator timer, the live-plan rebuild
   and the "recording in progress" overlay) is false from phase 1 on.
   `midiRecorder_->endCapture()` — the last drain and the last look at the
   clock, which must still happen while the transport runs
   (`SMidiRecorder::stop()`'s `pollFinal_()`), then its placement plan:
   today's `commit_()` is SPLIT at its `beginMacro` line — everything above
   it (the host→frame mapping through `clock_.frameAtHostNs`, the sort, the
   per-track-per-pass grouping) becomes the phase-1 plan whose emptiness
   `willCommit()` reports; only the submit loop and the `armed_`/message
   clearing remain in `commit()`. An implementer who splits at today's
   function boundary gets a `willCommit()` that cannot know its list.
   `audioRecorder_->endCapture()` — `bridge_->endCapture()`,
   `content_->freeze()`, the no-anchor fallback, **`removeGrowingClips_()`**,
   THEN the segment plan from the FINAL numbers with the §3.5 snap. The
   growing clips must leave BEFORE the snap, not in phase 3 where today's
   `commitPlacement_()` removes them: a growing clip is an ordinary lane child
   with a duration, its start at `recordStart_` and an end trailing the
   capture by up to one 100 ms tick, so a snap that saw it would find an
   agreeing candidate for `b` on EVERY armed lane and snap to it, silently and
   non-deterministically. Shell CONTRACT inv. 26's "removed FIRST, else
   `place-recording` would see them as covering columns" becomes "removed in
   phase 1, before the plan and the snap". Each recorder's
   `bool willCommit() const` answers "my planned list is NON-EMPTY" — a
   non-empty list rather than `placeable > 0`, because a punch region with
   `b <= a` yields an empty list and opens the inner macro anyway today.
2. The outer `beginMacro("Recording")` iff either `willCommit()`.
3. `midiRecorder_->commit()` then `audioRecorder_->commit()` submit the
   planned lists; their own `beginMacro` calls become nested macros
   (QUndoStack merges them into the enclosing step).
4. `endMacro` (if opened); the transport stop and the locator reset stay
   where they are today, at the end of the audio half.

One accepted limit: a pass whose planned lists are non-empty but whose EVERY
`place-recording` is refused at apply (every armed lane Kw; a project closed
under the recorder) still leaves an empty outer macro — `QUndoStack` cannot
pop a macro once opened, and today's inner macro has the identical hole, so
this is not a regression. `SApplication::submitAction` and
`SActionHistory::submit` are both `void`, so there is nothing to count from
their return: `commit()` reads the undo stack's `count()` and `index()`
before and after its submit loop and logs the all-refused case (no change in
either) at warn level beside the lane reports — one line, no API change.

Why conditional: `QUndoStack::beginMacro` pushes the
macro command at once and `endMacro` leaves an empty one on the stack, so an
unconditional outer macro would add a no-op "Recording" undo step to every
empty pass, and `record_punch`-style "undo → 0 clips" cases would consume it.
`record_mixed_one_undo.qxa` asserts the empty case too. The sentence
"one undo step per recorder" in `sapplication.h` (the `stopRecording()`
doc comment) and the matching line in `shell/CONTRACT.md` inv. 26 are
rewritten to the two-phase shape above; the sentence "the capture ends
FIRST" in `SAudioRecorder::stop()` stays true (it is phase 1), and
rewritten; §10 records that this reverses a documented decision.

Inside one lane's composite the rollback is the existing one
(`SCompositeAction::apply`: earlier children's inverses in reverse, best
effort). A lane whose plan is refused BEFORE apply (K7, system lane) is
skipped and reported; a lane whose composite FAILS during apply is rolled
back, reported, and its file listed as unused (Q13). Both messages go
through `SMainWindow::postHint` and the recording dialog, not only `TW_LOGW`
(B6).

## 6. New commands

### 6.1 `merge-columns` (Q14) — **SHIPPED as A1 (QBX-179)**, refined by P2

`merge-columns clips="0,1;0,2;0,4"` — one lane at a time; a selection
spanning lanes is run once per lane by the UI. Result: one column at the
first clip's child index, extent `[min start, max end)`; every plain clip
becomes a take at its extent; every column's takes transfer at their extents
shifted by `(colStart − newStart)`, its comp map likewise; the combined map
is each clip's previous answer over its extent, the LATER clip in the list
winning where two overlap; active take = the last clip's active take (or the
plain clip itself). Fades, names, volume, pan, link attributes (`timebase_`,
`startTicks_`, `eventChannelOverride_`) travel with each take — they are
properties of the window or its link, and the window is MOVED, not copied.
Refused: mixed content kinds; a wrapped column; a live recording; a clip
that is a lane. No audio is rendered or rewritten.

Inverse: `unmerge-columns`, live-only, holding the recipe (which take came
from which clip, at which index, with which link attributes and which map)
and re-linking the SAME window objects — identity, as plan 52 does for a
deleted column. The forward pins nothing extra: the takes are children of
the new column, and the inverse moves them back.

**The link-move protocol**, for every window moved from the lane into the
column (and reversed, step by step, by `unmerge-columns`) — it is the one
`stakes::wrapCutLinkIntoStack` / `collapseSingleTakeStack` use AFTER B1
(today both disconnect with the NARROW `SIGNAL( durationChanged )` form, which
leaves a lane's `eventsChanged` connection dangling when an `SMidiCut` is
wrapped by `add-midi-take`; B1 switches them to the broad form), and the first
implementation that skips a step leaks a connection or crashes in teardown: (1) the new take link takes
its reference BEFORE the old lane link dies (`new SLink( obj, nullptr )`
first, `delete oldLink` after); (2) the lane's object-level `durationChanged`
connection to the window is severed with the BROAD `QObject::disconnect( obj,
nullptr, lane, nullptr )` (plan 52 §3: the narrow `SIGNAL(durationChanged)`
form misses `eventsChanged` on an event take) — the window now reports to the
column, the column to the lane; (3) any link torn down gets `setParent(
nullptr )` before `delete` (the `~SLink` childRemoved use-after-free rule);
(4) the lane's `twTrackMix` clip entry leaves through the link delete and the
column's entry arrives through `setParent( lane )` → `childEvent` →
`trackChildWasAdded`, so nothing is wired by hand. The merged column is
placed at the first operand's child index with `moveChildToIndex`; the
removed operands are deleted in DESCENDING index order (§5.1).

`merge-columns` is also the primitive mode A's planner uses (§3.1 step 2), so
it is gated twice: once alone, once through a recording.

**Correction (m), from the build: "the forward pins nothing extra" is wrong,
and in the way plan 52 exists to catch.** The TAKES need no pin — that part is
right. But an operand COLUMN is orphaned by the merge (its takes move out, its
lane link dies), and an `STakeStack` is an `SObject`: it carries its own
`volume`, `pan`, `muted`, `solo` and `recordingChannels`
(`SObject::serializeSelfAttributes`), none of which lives on a take or on a
link. Rebuilding it from the recipe would silently drop them. So the forward
`addRef()`s each operand column and the inverse re-links the very same object.

**Correction (n): "fades, names, volume, pan ... are properties of the window
or its link" is true of a TAKE and not true of the PLACEMENT.** The mix reads
a placement's parameters through `SClipWindow::parametersOf`, which resolves a
DIRECT column to its ACTIVE TAKE — so a merged column has exactly ONE set of
clip parameters (gain curve, static volume, fade, pan) and
`STrack::refreshClipGainCurves` applies them to the WHOLE column. N operands
at different levels therefore CANNOT all be honoured, and §6.1's "no audio is
rendered or rewritten" does not by itself make the merge playback-preserving.

Since this row's acceptance criterion is that the render is IDENTICAL before
and after, A1 adds a REFUSAL §6.1 does not list: every operand must agree on
`getVolume()` and `getPan()`, and none may carry a `cut:Gain` envelope or a
clip fade — a curve and a fade are POSITIONAL (`twClipFade::gainAt` measures a
fade against the clip's own end), so even identical ones would mean something
different once the clip is longer. The alternative was to merge anyway and let
the lane re-mix itself silently, which is what the criterion forbids. A
non-default value is fine as long as every operand AGREES on it, which is what
the gate asserts by merging the same pair twice.

This is **QBX-185 in a second place**: per-take gain is applied ABOVE the
column, so it cannot vary per take. When QBX-185 is fixed this refusal can
narrow to what genuinely cannot be carried, and merging two clips at different
levels becomes expressible.

**Correction (o): the combined map needs the previous answer to RE-ENTER at
each operand's end**, which "the LATER clip in the list winning where two
overlap" does not say. Without it a later operand's answer runs on past its
own material and the earlier operand's span falls silent. The painting shape
is exactly §4.3's — "this, here, and what was there before everywhere else" —
and a column operand also needs its own `activeTake_` written down as a
segment at its extent start, because the merged column's `activeTake_` is a
different number.

### 6.2 Per-selection fade edit (Q9)

The auto-fade (§8 P5) is a project property applied in the mix at every clip
edge and every comp boundary whose `xfade` is 0 — except a SEAMLESS edge
(D3b default (a): the neighbour is a clip over the same `windowContent()`
whose source position continues exactly, at the same stretch; a `split-clip`
or a mode-B seam of one take is seamless, a punch over different material is
not). The per-item override is the
existing `set-clip-fade` (per clip, per take, absolute, undoable) plus a new
tri-state: `twClipFade` gains `autoIn`/`autoOut` (default true, serialized
only when false), and `set-clip-fade auto="0"` turns the auto-fade off at an
edge without declaring a fade. The UI command "Set fade for selection…" is a
dialog that submits one `set-clip-fade` per selected clip (per selected take
in expanded lanes) inside one macro. **SHIPPED (P5, QBX-183) as the Clip
Properties panel's FADE GROUP rather than a dialog** — that panel already edits
the selection and already submits one `SCompositeAction` per edit (proposal
31), so a dialog would have been a second copy of its multi-select semantics;
see §8's P5 row, correction (c), and main/timeline/CONTRACT.md inv. 67. The
group shows the exempt edges this section asks for, which is what the two
checkboxes cannot say on their own. For a comp boundary the existing
`set-comp-xfade` is the override and the auto-fade yields to any `xfade > 0`.

### 6.3 `cleanup-takes` (Q13)

`cleanup-takes scope="selection" | "project" files="0|1"`. What it removes,
precisely, in this order, as ONE undoable macro (the file half excepted):

1. **Unused takes:** in every column in scope, a take that is not the active
   take and that no comp segment selects over any position where it has
   material (`takeHasMaterialAt` ∧ `takeIndexAt`), **and that came from a
   RECORDING** (D5 default (a): a take whose name carries the recorder's
   stamp, §8 P4, or whose file matches the recorder's pattern below; a take
   that was the user's pre-existing clip — W1's original A — is EXEMPT unless
   the dialog's "include original clips" box is ticked). The confirmation
   lists takes by origin: "3 recordings, 1 original clip (kept)". Removal is
   `remove-take`, which must first gain a real inverse for non-file-backed
   takes (plan 52 §6 names the null-inverse hole; the pin pattern closes it)
   — otherwise a MIDI take removed by clean-up would not come back.
2. **Dangling comp segments:** segments past the column end or naming a
   removed take. `remove-take` renumbers the map once B10 (§7) lands —
   today neither `STakeStack::removeTake` nor `SRemoveTakeAction` touches
   `compMap_` — so this step is the `normalized()` pass written back through
   `set-comp-map` for whatever B10's renumbering leaves (segments past
   `duration_`).
3. **Collapse:** a column left with one full-extent take collapses (3′).
4. **Orphan recordings** (only with `files="1"`, and the UI asks first, naming
   every file): WAV files in the project directory matching the recorder's
   `<stamp>_track<n>.wav` pattern (`saudiorecorder.cpp:207`,
   `"%1/%2_track%3.wav"`) that no `SExternFile` in the project
   references, plus `SExternFile`s with zero placements that are not
   registered assets. They are moved to the OS trash (`QFile::moveToTrash`),
   never deleted in place, and this half is NOT on the undo stack — the
   confirmation dialog says so in one sentence.

The **"Unused recordings"** list (a dock, read-only) is the same scan without
the removal; it is also where a refused or failed lane's file (§5.4) appears
with the reason, so X7 ("lost with a qWarning") ends.

### 6.4 `set-track-record-mode`

§5.2. `trackPath`, `mode` = `""` (inherit) | one of the four. Undoable.
Refused on a system lane (it cannot be armed).

### 6.5 `select-take take="newest"` (Q10)

`take=` accepts `newest`: the take with the latest recording stamp (§8 P6
take names carry the stamp; a take without one is older than any with one).
The edit-group broadcast resolves `newest` PER MEMBER, so a lane with k+1
takes and one with 2 both land on the pass just recorded.

## 7. The bugs, re-verified and re-ranked for mode A as default

Every one is its own PR, lands before the phases, and is gated by a case that
was **watched failing on `main` before the fix** — a gate nobody has seen fail
is not evidence. Order is dependency order.

| # | one line | verified at `c682e944` | gate |
|---|---|---|---|
| **B1** (X5) | `wrapCutLinkIntoStack` / `collapseSingleTakeStack` copy `startTime` only (`stakehelpers.cpp:202-203`, `:227-228`); `timebase_`, `startTicks_`, `eventChannelOverride_` are lost and undo does not restore them; both also disconnect with the NARROW `SIGNAL( durationChanged )` form (`:199-200`, `:224-225`), missing `eventsChanged` on an event column (review 2 m6; review 3 R3-2: a duplicate invalidation, not an audible defect) | confirmed; lines moved from the ticket's `:196-197`/`:221-222` | `take_wrap_keeps_link_attrs.qxa`: `set-link-timebase beats` on a clip, `add-take`, `set-tempo`, `assert-clip-window` shows it moved; `undo`; `assert-clip-window` back; `verify-undo`. The broad-disconnect half is HYGIENE with no `.qxa` that can observe it: the stale `eventsChanged` connection fires into `STrack::trackEventClipChanged`, which resolves the sender against the lane's CURRENT child links and returns when nothing matches, and after the collapse re-links the clip `trackChildWasAdded` connects it again, so the slot runs twice and performs two IDENTICAL invalidations — never a second event, and `scope="feed"` has no notion of deliveries. The PR says so rather than inventing a gate; a unit-level slot-invocation count is acceptable if one is cheap. Prerequisite for every planner change: B's undo collapses through this path (G10), and `merge-columns` moves links |
| **B4** (X3/X15/X10) — **prerequisite for A** | `SAddTakeAction::apply` activates (`saddtakeaction.cpp:92-94`) without touching the map; `setActiveTake` never clears it (`stakestack.cpp:123-135`); `twCompMap::takeAt` is −1 only before `seg0` (`twcompmap.h:60-68`), so a map starting at 0 makes the new take fully inaudible while badged. Painter ignores the map (`:505-521`) | confirmed | `add_take_clears_comp.qxa`: `set-comp-segment at=0`, `add-take`, render → new take audible (closed form from `test_gapsaw.wav` as `comp_map_audible` does); `undo` → `assert-take-column comp=` shows the old map. Painter half: QBX-157 owns it or this does — stated in whichever PR lands first |
| **B3** (X11) — **prerequisite for A and B** | `split-clip` direct branch builds `stack2` without the map (`ssplitclipaction.cpp:70-101`); wrapped branch and `duplicate-clip` go through `cloneColumn`, which copies takes and active index only (`stakehelpers.cpp:162-177`); `unsplit-clip` cannot restore it | confirmed | `comp_map_survives_split.qxa`: comped column, `split-clip`, render → per-half RMS proves the tail still comps (shifted by −split) and the head keeps segments `< split`; `undo` restores both maps. `comp_map_survives_duplicate.qxa`: same through `duplicate-clip` and the view's Ctrl-drag |
| **B2** (X4) | `add-take` wraps first (`:50-58`), `insertTake` refuses a different kind (`stakestack.cpp:79-88`), `add-take` returns `{false}` deleting only `takeCut` (`:85-91`); the composite's rollback cannot reach the wrap (`scompositeaction.cpp:16-28`) → a one-take stack on the lane, nothing undoable, take lost | confirmed; reachability of a MIDI clip on an audio-input lane still unmeasured | `place_recording_kind_guard.qxa`: `insert-midi-clip`, `place-recording` over it → recording placed, `assert-take-column clip="0,0" shape="none"`, `verify-undo`. Fix in two places: `add-take` checks `win->contentKind()` BEFORE the wrap; `snapshot()` reports the child as `OtherKind` |
| **B5** (X6a) | the plan loop hands the same recording to a second column whose `start < cursor` (`splacerecordingaction.cpp:131-147`); gate (3) of `place_recording_over_earlier_clip.qxa:53-62` reaches the cell (0,2 = `[96000,288000)` also gets a take) and asserts only "no 0,3" | confirmed | fix gate (3): add `assert-take-column clip="0,2" shape="none"`. Interim fix: skip a column already consumed. Superseded by P1's Q7 rule; the assertion stays true under both |
| **B10** (new, review 1 #3b) | `STakeStack::removeTake` and `SRemoveTakeAction::apply` never touch `compMap_`: removing take 0 of a comped column leaves every segment pointing one take off; a segment naming the removed take is not dropped | confirmed (no `compMap` reference in either) | `remove_take_renumbers_comp.qxa`: comped two-take column (`comp_map_audible`'s fixture), `remove-take take="0"` → render is the surviving take, not silence or the wrong take; `assert-take-column comp=` shows the renumbered map; `undo` → the WHOLE previous map back. Fails on `main` today. Prerequisite of A0a and P6 |
| **B6** (X7, re-scoped) — **SHIPPED as QBX-164**, with two corrections below | a missing file at stop is `TW_LOGW` + `continue` (`saudiorecorder.cpp:574-578`); a lane whose plan is refused (§5.1) would be the same. **X17 is NOT reachable through `arm-track`**: `sliveinputactions.cpp` `resolveTrack()` calls `splacements::refuseSystemLane` for `arm-track`, `set-monitor-mode` and `set-track-input` (proposal 45 AC5.2; `docs/ACTIONS.md:58`), and `acceptsClips()` is true for every other lane. Review 1's gate-side note that "a system lane CAN be armed today" is wrong — see §13 | confirmed | the REPORTING half only: `record_lane_skipped_reported.qxa` drives `place-recording` on a lane with a Kw column (`takestack_wrap_wrapperpan.qxp`) once P1's refusal exists, and asserts the hint text and the unused-recordings entry; until P1 the case cannot be written and B6 ships the report path with the file-missing branch exercised manually, saying so. `placeGrowingClips_` switches to `placementLaneAt` for symmetry — a one-line guard with no reachable gate |
| **B7** (X9) | `stopRecording` calls the two `stop()`s in sequence (`sapplication.cpp:1438-1439`); each opens its own macro (`saudiorecorder.cpp:573`, `smidirecorder.cpp:454`); `sapplication.h` documents "one undo step per recorder"; the audio recorder returns before its macro when `placeable <= 0` | confirmed — a documented decision, so §5.4 is a contract edit | `record_mixed_one_undo.qxa`: arm one audio and one MIDI track, record, ONE `undo` → `assert-recorded-clip clips="0"` and `assert-midi-recorded clips="0"`; then a DETERMINISTIC empty pass — a punch region lying entirely behind the locator, so `b <= a` and the segment list is empty (`record-start` is REJECTED without an input, and the file backend always captures something, so "no input" is not a usable empty pass) — pushes NO undo entry: `save-project`, the empty pass, `assert-unsaved-changes expect="0"` (it reads `QUndoStack::isClean()`, so the save first is what makes the assertion mean anything) |
| **B8** | `docs/ACTIONS.md:312` says `set-comp-xfade` is "Model only until proposal 43 N3"; `twCompColumn` renders the equal-power crossfade and `comp_crossfade.qxa` gates it | confirmed stale | none; one line |
| **B9** | `record_stays_armed.qxa` is a running-transport case whose two takes land by jitter and deliberately asserts existence only (its header says so, lines 21-34) | confirmed | add one sentence to its header: it must not be read as evidence for the structure rule; under A both outcomes become one column, under B two clips — the case keeps asserting existence only |

Order: B1 → B4 → B3 → B10 → B2 → B5 → B6 → B7 → B8/B9. B4 and B3 moved up
because mode A plays through the map everywhere; the old order had them as
tidy-up.

> **B6 LANDED LAST, not sixth, and two of its row's claims were wrong
> (QBX-164).**
>
> The order above was followed except for B6, which needs a REFUSAL to report
> and no planner refused anything until P1 shipped one — the row says so itself
> ("until P1 the case cannot be written"). So the real order was
> B1 → B4 → B3 → B10 → B2 → B5 → B7 → B8/B9 → P1a → P1b → B6.
>
> **"depends on QBX-145's report path" was a misreading.** QBX-145 built a bool
> plus a `QString *error` out-param on `SApplication::startRender`, for one
> call site. It is not a reusable mechanism and there is no other: the one place
> that could turn any refusal into something a person sees is
> `SActionHistory::onRejected_`, whose entire body is still
> `// TODO: Log error and notify UI. // For Phase 1, just clean up.` — so
> **no refused action reaches the user anywhere in the app**, which is a far
> larger gap than this row and is now **QBX-176**. B6 therefore built a
> recording-specific report in `shell` rather than waiting, and samples
> `SActionHistory::rejectedCount()` around each lane's submit to learn of a
> refusal — the pattern that class's own header already documents for the test
> runner.
>
> **The gate goes through the RECORDER, not through `place-recording`.** The
> row says it "drives `place-recording` on a lane with a Kw column", and that
> cannot work: driving the verb directly never populates a recorder-side
> report, and routing the report through the action layer instead is QBX-176.
> `record_lane_skipped_reported` loads the same fixture, sets the lane's record
> mode to one that refuses a wrapped column, arms it, and records — and arms a
> SECOND, empty lane besides, because "a refusal costs only its own lane" is
> the half of this that a one-track case cannot state.
>
> It also needed two testkit additions the row did not foresee:
> `assert-recorded-clip contains=` (its MIDI sibling has had it since proposal
> 21 L4; the audio verb had none, so the recorder's own report of a pass was
> unassertable) and `nestedRejects=` (the runner infers an action's outcome from
> a global rejection counter, so a verb whose internals refuse on purpose reads
> as broken). B10 sits after B4 because both edit the same two functions' map
handling and the second PR should see the first's shape.

## 8. Phases

Each phase is one PR sized for one agent; each names its gates and which
existing gates FLIP (an expected flip is edited in the same PR, with the
reason in the case header, never before). The rule from §7 applies to every
gate here too: run it against `main` first and watch it fail for the right
reason.

**Order, and why it changed after review 1.** Mode B, `overdub` and `replace`
need none of §4 (invariant 1 holds as written for them), so the planner
FRAMEWORK lands first with those three modes and proves B1/B3/B4 under load;
§4's model change lands next in two halves, soaks, and the default mode A
lands LAST on a framework that has already recorded real takes. Until mode A
lands, the `Takes` slot is served by the LEGACY planner (today's
`place-recording` body, renamed `LegacyTakes`), so a new project's default
keeps today's behaviour and nothing in §9 flips before the mode-A PR.

### P1 — planner framework; `takes-split`, `overdub`, `replace` — depends on B1–B6, M0

> **Filed as TWO tickets**, because this row is the only L in the B/M group and
> splits cleanly at the point where a planner first needs the simulated child
> list for anything but appends: **QBX-171** is the framework, `LegacyTakes`,
> `overdub`, the thin verb and the recorder's snap; **QBX-172** is
> `takes-split`, `replace` and G12. Everything below is split between them in
> that order, and neither flips anything.
>
> QBX-171 also had to widen `assert-clip-window`: it dropped every NEGATIVE
> `startOffset` expectation as if it were the "not checked" sentinel, so the
> one verb that reads a clip's slip could not assert the one slip value this
> plan needs (`place_clip_negative_offset` below passed vacuously without it).
> `-1` stays unset; any other negative value is now an expectation.

Build: `srecplan` (§5.1) with the simulated lane child list; `place-recording`
as the thin verb with `mode=` and `armed=`; the recorder's pass-wide snap
(§3.5) and hand-over (§5.3); `Overdub` (trivial), `TakesSplit` (§3.2, with
the appended-index bookkeeping), `Replace` (§3.3, over `resize-clip`,
`split-clip` and `remove-sample`'s pinned inverse); `LegacyTakes` behind the
`takes` value; G12 — the transport shows "cycle overrides punch" while both
are set.

Gates — one case per R value, Kp/Kc/Km on three tracks of one project,
driven through `place-recording mode="takes-split"` so every number is
arithmetic (`test_sawtooth.wav`, 192000 frames, distinct RMS per second):
`rec_split_r1.qxa` … `rec_split_r6.qxa` (child count and edges per lane, the
inside column's map cleared, the tail column's map shifted, `verify-undo` —
where B1 and B3 are proven under load); `rec_split_w1.qxa`;
`rec_split_cycle_inside.qxa` (two columns per cycle, no take beyond the
cycle); `rec_replace_cells.qxa` (R1 trimmed, R2 split, R3 removed and undone
by identity, R4 left-trimmed with the source anchor advanced);
`record_mode_overdub_audio.qxa` (plain cut over the clip, both sum,
closed form); `record_snap_pass_wide.qxa` and `record_snap_disagree.qxa` —
both from a STOPPED transport (the landing is then pinned to the locator
exactly, `saudiorecorder.cpp:336-338`), with the locator set 144 frames
AFTER an edge shared by two armed lanes (snap earlier) and, in a second
take, 144 frames BEFORE it (snap later); the disagree case puts the second
lane's edge 288 frames away. A running transport is useless here: its
landing jitter is thousands of frames, far more than N. The snap-earlier
case asserts the RECORDING by its own path, not through `assert-recorded-clip`
— that verb checks `clips.front()`, the lane's FIRST child with a duration,
against the recorder's latest file (`srecordtestactions.cpp`; `record_stays_armed.qxa`'s
header says the same), and on this fixture the first child is the
pre-existing edge clip (or the column it became), so it would fail for the
wrong reason or pass vacuously. The shared edge is the END of a clip
`[x, 388800)` on both lanes — were it a clip START the recording would be
R3/R4 and become a take on the column at `0,0` — so under `takes-split` and
`LegacyTakes` a recording that merely TOUCHES the edge clip (R0) is appended
and its path is `0,1`: `assert-clip-window clip="0,1" startTime="388800"` (the edge), a
48000-frame `assert-audio-energy` window at the head of a render (the file
backend's fixture has distinct per-second RMS, so the head's source second
is identifiable), and `assert-log contains="[REC] snap a"`. It does NOT
claim a negative `startOffset` — from a
stopped transport the frames before the transport start are TRIMMED
(`trimmed_ > 0`), so snapping earlier by 144 frames consumes the handle and
the offset stays ≥ 0 unless `trimmed_ < 144`, which a case cannot force. A
negative `startOffset` through `place-clip` gets its own arithmetic case,
`place_clip_negative_offset.qxa`, driven directly. `rec_split_r*` headers
state that on `main` `mode=` is ignored by `readXml`, so the old planner runs
and the shape is wrong — the right reason to fail.

> **THAT PREDICTION EXPIRED WITH M0, and the headers say so instead (QBX-172).**
> Since QBX-171 `mode=` IS read and validated, and `takes-split`/`replace` were
> shipped as planners that REFUSE loudly — so on `main` the `place-recording`
> action is REJECTED and the case fails there, not on a wrong shape. Both are
> failures but only one is the one named here, and a case header that claims
> the wrong mechanism is a case nobody can re-verify. The assertions were kept
> strong enough to discriminate the MODE rather than weakened to "the verb
> succeeded": `LegacyTakes` gives every one of these lanes a different child
> count and a different shape, so a silent fallback to `takes` would be caught
> by the assertions and not merely by the refusal.

**Un-armed members: P1 does NOT enumerate them** (review 2 M5). Every P1
planner, `LegacyTakes` included, treats `armed="0"` as nothing to do; the
recorder submits for armed lanes only. The member enumeration and both takes
planners' un-armed behaviour are P4's.

Flips: none. `takes` still runs the legacy planner.

### M0 — the record-mode property (§5.2) — independent; lands before P1

Build: `SProjectProps::RecordMode`; the INI seed at the three new-project
seams and `close-project`; `STrack::recordMode_`; `set-track-record-mode`;
`srecplan::resolveMode( lane )`; `mode=` on `place-recording` and
`place-midi-recording` (parsed and resolved; EVERY value still runs the old
planners in this PR — M0 is plumbing); the transport selector; the per-user
keys `transport/locatorAfterRecord`, `takes/expandOnLanding`.

Gates: `record_mode_resolution.qxa` (project `takes-split`, folder `replace`,
track `overdub`: the resolved value per lane through a new
`assert-record-mode trackPath= resolved=`; `set-property key="recordMode"
value='["takes-split"]'` — the JSON-array spelling); `record_mode_ini_seed.qxa`
(`set-option midi/recordMode replace`, `close-project` — which creates the
fresh project through the real seam — `assert-record-mode` reads `replace`,
then `set-option` back; this case is the ONE owner of the `midi/recordMode`
INI key from now on); `midi_record_modes.qxa` rewritten to drive the project
property and to touch no INI key.

### A0a — take extents, model only (§4.1) — depends on B1, B3, B4, B10 — **SHIPPED (QBX-177)**

The six corrections from building it are in §4.1. Gated clean; the five gates below exist and were each watched failing on a `main` baseline built at `1bd252e7`, and `take_extents_invalidate` additionally bites under sabotage of the per-take forwarding alone.

Build: `STakeStack::duration_` (stored, derived on load, written only when it
carries information); extents in `insertTake` and the loader;
`hasPartialTake()` and the resolution rule (a column with any partial take
resolves through `twCompColumn`); `twCompColumn::setInputExtent` and the
no-dependency rule; the `clampedXfade_nolock` extent clamp; forwarding for
every take and the shifted `mapChildRangesToSelf`; `split-clip`'s per-half
membership, re-indexing, remapping and identity inverse; `remove-take`'s
extent-carrying inverse; `add-take at=/length=`; `cloneColumn` with extents;
`assert-take-column extents= duration=`; `FORMAT_VERSION 3`. The PR MEASURES
and lists every consumer of take geometry it touched (the proposal 42
discipline), including the nine delegates §4.1 names.

Gates: `take_extents_model.qxa` (`add-take at= length=`, save/load
round-trip, `assert-take-column extents= duration=`; a full-extent column
saves with NO `duration=` attribute — `assert-file-contains absent="true"`,
an attribute the verb has (`sassertfilecontainsaction.cpp`) but
`docs/ACTIONS.md` does not list; A0a adds the row); `take_extents_audible.qxa`
(two takes, take 0 full, take 1 over the second half only. Active = take 0,
map `96000→1`: take 0 renders over `[0, 96000)` and take 1 over `[96000,
192000)` at its own source position. Then `select-take take="1"` with the
map cleared — active = the PARTIAL take, empty map: `[0, 96000)` renders
SILENCE, because an empty map means the active take everywhere and a take
has no material outside its extent; there is NO fall-back to another take,
§3.1 step 5. Both halves of this case are the resolution rule);
`take_extents_split.qxa` (the W1 shape split at 4 s: head `{A, R}`, tail
`{B, R}`, maps re-indexed, `assert-take-column activeTake=` on BOTH halves
per §4.1's rule, render identical before and after, `undo` restores the
moved takes by identity and the crossing takes by re-growth, with
`drain-pending-deletes`);
`take_extents_invalidate.qxa` (a comped INACTIVE take slipped by
`resize-clip take=` → the render changes; fails on `main`, where the slip
bumps nothing); `take_extents_remove_undo.qxa` (`remove-take` on a partial
take, `undo` → the extent is back, not a full-extent take).

Flips: `load_unknown_object_survives.qxa:44-47` (`formatVersion='2'` →
`'3'`), edited here with the reason. Every other column gate stays green —
full-extent takes are the degenerate case; if one moves, the degenerate path
is broken and the PR stops.

### A0b — take extents, edit surface and view (§4.1–4.4) — **SHIPPED (QBX-178)**

Build: extent-aware `select-take` (§4.3); `resize-clip`'s clamp and
EMPTY-refusal; the take-lane slip and `resize-clip take=` reading extents;
both painters' extent windows and the hatched no-material span.

Plus TWO ITEMS A0a HANDED OVER BY NAME, both recorded at their own sites:

  * `STakeStack::hasPreview`/`getPreview` are still the active take's,
    UNWINDOWED. A preview is what a painter reads, so windowing it while both
    painters still draw across the whole column produces a half-windowed
    picture: an envelope that stops at the extent inside a waveform that does
    not. The pair moves together, here.
  * `SResizeClipAction`'s LEFT-EDGE branch (no take named) writes `duration_`
    onto every take, which is invariant 1 speaking after `applyWindowAll` has
    already clamped correctly under 1′. Unreachable today — the only producers
    of a partial take are `add-take at=/length=` from a script and
    `split-clip`, which preserves them — and the edit surface that would reach
    it is this row's own subject.

Gates: `select_take_extent_aware.qxa` (`select-take take="1"` on a partial
take 1 leaves take 0 audible over `[0, 96000)`; on a full-extent take it is
today's clear — asserted by `assert-take-column comp=`);
`take_extents_resize.qxa` (a right-edge trim clamps, an extend grows only the
clamped take, a trim past a take's start is REFUSED with `assert-log`);
`take_extents_hatch.qxa` (a `remove-take` that leaves a selected span
without material renders silence there — the picture half is QBX-157's pixel
gate).

**Correction (i), measured: the third gate as prescribed would not have failed
on the baseline at all.** "A `remove-take` that leaves a selected span without
material renders silence there" is **A0a's resolution rule**, and it is already
green on a baseline built at A0a's own merge. The renders in
`take_extents_hatch.qxa` are therefore REGRESSION COVER for A0a, not a gate on
this row; what bites is the **ENVELOPE** half — the comp result and one take's
lane row over the same span, which disagree exactly where a take is partial.
A0b accordingly added `assert-envelope take=` (there was no way for a script to
read the per-take collect at all) and the case asserts both pictures.

**Correction (j):** a gate may not rest on a newly added ATTRIBUTE to fail on a
baseline. A0a recorded that `slip-clip take=` was "rejected outright" on main;
measured here, an unknown attribute on a verb that DOES declare
`knownAttributes()` is **warned about and IGNORED** — the runner fails on it
only under `SMARAGD_STRICT_ATTRS=1`. So the attribute silently degrades to its
default and the assertion runs in its old mode, which can PASS. Every gate must
also assert through something the baseline understands.

**Correction (l), measured by sabotage: A0b gates NO PIXELS, and the suite
cannot.** With the compact painter's comp-result branch reverted to the old
active-take-only draw AND the take lane's extent-edge walk reverted to the
map-only loop, the WHOLE suite passes — 449/449. Both are pure paint, and no
verb in the suite can read either surface (`assert-lane-overlay` reads a TRACK
lane's folder-sum overlay, not a take lane). So §4.4's picture is gated by
QBX-157 alone, as §4.4 itself allocates, and A0b gates the ENVELOPE both
painters read plus the AUDIO. `draw` and `collectEnvelope` share one run walk
so they cannot drift while only one is gated — an argument, not a gate, and
recorded at both sites for whoever lands QBX-157.

**Correction (k):** emptiness is not enough. Two `expectEmpty="true"` envelope
assertions passed throughout while the collect was taking an entirely wrong
path, because an all-zero array cannot tell a windowing bug from a correct
answer. The bug (a `takeIndex < 0` resolved to the active take rather than
passed through to the comp result, which silently rerouted EVERY ordinary clip
envelope read on a take column) was caught only by a non-empty IDENTITY
assertion beside them — `snapshot`/`compareTo` against the take that does carry
the span.

### A1 — `merge-columns` / `unmerge-columns` (§6.1) — **SHIPPED (QBX-179)**

Gates: `merge_columns.qxa` (two plain clips with a gap + one comped column →
one column, extents and combined map asserted, render identical before and
after by `assert-file-identical` — merging must not change what plays; the
lane's remaining children keep their order, asserted by `assert-clip-window`
per QBX-149); `merge_columns_undo_identity.qxa` (fades, names, pan, link
timebase on the inputs; `undo` → every one back at its original child index,
`drain-pending-deletes` after the merge and after the redo, as plan 52's gate
does); `merge_columns_refused.qxa` (mixed kinds, a live recording, a wrapped
column: each refused and announced).

**Correction (p): the three prescribed gates all fail on a baseline for the
WEAKEST possible reason** — `merge-columns` is not a registered action name
there, so the runner refuses to LOAD the script at all. That proves only that
the verb is new; it proves nothing about whether any fixture or assertion
bites, and it is the same trap as A0b's correction (j). So A1's verification
is SABOTAGE on the built tree, one broken piece at a time, recorded in the PR
— the baseline comparison is kept only as evidence that the verb is new.

The five sabotages, measured, and TWO OF THEM FOUND THE GATES VACUOUS at first:

| broken piece | case | first run | after the fixture was fixed |
|---|---|---|---|
| the extent shift `(colStart − newStart)` | `merge_columns` | fails | fails |
| the previous answer's RE-ENTRY at an operand's end | `merge_columns` | **PASSED** | fails |
| `moveChildToIndex` in the inverse | `..._undo_identity` | **PASSED** | fails |
| the operand column's `addRef()` pin | `..._undo_identity` | SEGFAULT | SEGFAULT |
| the mix-parameter refusal | `..._refused` | fails | fails |

The two that passed were both fixture holes, and each is worth recording
because the shape is general:

- **The re-entry rule only bites on OVERLAPPING operands.** The first fixture
  was disjoint (A, gap, B, C), and there every operand is immediately followed
  by the next one's own segment, which overwrites the re-entry anyway. A gate
  for "the previous answer resumes" needs an operand that ENDS INSIDE another
  one. Note that half cannot assert `assert-file-identical`: two overlapping
  clips SUM on a lane and a column plays one take per position, so the render
  legitimately changes there — D1 (a) and §3.4's "the SUM they produce is NOT
  preserved". Identity belongs to the disjoint half.
- **The child-index restore only bites with a SURVIVOR after the operands.**
  Restoring in ascending original index already reproduces the original order
  when the operands are the lane's only children, so an inverse that simply
  APPENDED was indistinguishable. With a survivor at a higher index, appending
  leaves it at index 0.
- The pin's sabotage SEGFAULTS rather than failing an assertion: the drain
  deletes the orphaned column and the inverse then touches freed memory. That
  is a gate (ctest fails on a crash) and it is the strongest evidence the pin
  is load-bearing, but it is worth knowing that no assertion names it.

**Correction (q): the undo-identity gate cannot use the properties §6.1 names.**
It asked for "fades, names, volume, pan and link timebase on the inputs", but
correction (n)'s refusal rejects a fade outright and rejects DIFFERING volume
or pan. The gate therefore gives every operand the SAME non-default volume and
pan, which the verb accepts and which still proves they travel and come back;
the refusals are `merge_columns_refused.qxa`'s. And identity is asserted
through THE RENDER — `assert-file-identical` between a render before the merge
and one after the undo — because the suite has no `assert-clip-volume` /
`assert-clip-pan` / `assert-clip-name` verb, and one render covers every
audible property at once rather than the few a verb list would name.

**Correction (r): the live-recording refusal needs its own case.** A growing
clip exists only while a pass runs, so it needs a file input backend, a
capture speed and `RUN_SERIAL` on the CTest entry — which the structural
refusals case has no other use for. It is `merge_columns_refused_live.qxa`,
with its own CMake entry beside the other record cases. `assert-recorded-clip`
cannot be used there: it inspects the lane's FIRST clip, which in that fixture
is the static one the recording is merged WITH.

### P2 — mode A (`takes`, the default) — depends on P1, A0b, A1 — **SHIPPED (QBX-180)**

Build: the `Takes` planner (§3.1) replaces `LegacyTakes`; the `fromCycle`
whole-cycle extent (§3.1 step 5); D1 default (a).

Gates — one case per R value, Kp/Kc/Km on three tracks, through
`place-recording mode="takes"`: `rec_union_r1.qxa` … `rec_union_r6.qxa`,
each asserting `assert-take-column extents= comp= activeTake=`, a render
against the closed form, `select-take` back to the old material over its
extent, `verify-undo`. Plus: `rec_union_w1.qxa` (the user's case, two clips,
one column, `3/3`, heard R then B); `rec_union_overlap_legacy.qxa` (D1: two
overlapping clips → one column, topmost outside `[a,b)`, and the sum is gone
— asserted, so the choice is visible); `rec_union_cycle_inside.qxa` (C8
live: `cycle-enable`, clip at 6 s inside a 16 s cycle, two passes with
`SMARAGD_AUDIO_INPUT_BACKEND=file:` → ONE column `[0, 20 s)`; every RECORDED
take's extent is exactly the cycle `[0, 16 s)`, none beyond it; the old
clip keeps its own extent `[6, 20 s)` and plays after 16 s). Headers state
that on `main` the legacy planner runs
and the lane holds two children — the right reason to fail.

Flips, edited in this PR with the reason in each header (the full list is §9):
`takes_recording_placement.qxa:40-51`; `place_recording_over_earlier_clip.qxa`
(1), (2) and (3)'s `clip="0,1"` assertion; `record_stays_armed.qxa:73`.

> **SHIPPED (QBX-180). What this section got wrong, beyond §3.1's (s)–(w).**
>
> **(x) THE FLIP LIST WAS WRONG IN BOTH DIRECTIONS, and both are corrected in
> §9.** `record_stays_armed` does NOT flip — QBX-171 had already moved its
> second pass out of reach — and `record_snap_disagree` DOES, which no section
> predicted. Measured: the whole suite on the mode A build has exactly three
> failures, and those three are the whole flip list. Every other §9 row that
> says "unchanged" was verified unchanged by that run rather than by reasoning.
>
> **(y) TWO GATES MORE THAN PRESCRIBED.** The first is the cycle extent,
> because the prescribed set cannot reach it. `rec_union_cycle_inside` is live, so the LAST pass's take
> index depends on how many passes the box got through and its extent cannot be
> asserted at all — only floors can. `rec_union_cycle_extent` is a scripted
> sibling that drives `place-recording extent=` directly and pins the rule
> exactly; the sabotage (dropping `extent=`) was measured and moves four
> assertions, including the audible one.
>
> The second is `rec_union_params_degrade`, for correction (u): nothing in the
> prescribed set has a clip carrying a fade or a non-default level, so the
> degrade path had no gate at all. Three lanes with the identical clip and the
> identical pass, differing only in a fade, a −6 dB trim and nothing — the
> first two degrade and the third merges. It is gated on the ANNOUNCEMENT and
> not on the structure, because a lane left holding two children is also what
> every build before mode A produced; and on the ABSENCE of
> `merge-columns: REFUSED`, because the point is that the verb is never
> emitted and so nothing is rolled back.
>
> **(z) `assert-recorded-clip`'s UNASKED "one take per pass" CHECK DOES NOT
> SURVIVE MODE A unaided**, and that is a testkit change this section did not
> foresee. The equality assumes every take on the column came from a pass,
> which is only true on a lane that was EMPTY; under mode A the material the
> pass was played over is a take as well. Relaxing it to `takes >= passes`
> would give up the very bug it was written for ("one time from the beginning
> plus the tail" produced MORE takes than passes), so the verb gained
> `extraTakes` (default 0) and the case declares how many takes did not come
> from a pass. Every case written before mode A means what it always meant.
>
> **(aa) THE DEGENERATE-MAP RULE belongs to `merge-columns`, not to the
> planner** — see the note under §3.3. A planner can only emit verbs, and
> predicting the merged map well enough to clear it with `set-comp-map` would
> mean re-implementing §6.1's map painter in the caller.

### P3 — MIDI parity and X16 — depends on P2

Build: `place-midi-recording` goes through the same `snapshot()`/planner
split with `Context.kind = Event`; `eventColumns`' "first column only" rule
(`smidirecordactions.cpp:341`) and its lack of a predates rule are replaced
by the mode A union over `SMidiCut` windows; `add-midi-take` gains
`atTicks=`/`extentTicks=` — NOT the existing `lengthTicks=`, which is the
SEQUENCE's length ("0 = derive from the column") and a different number
(D6 default (a): the take's extent is stored in
TICKS and converted through the tempo map at the column, so `set-tempo`
keeps the take over the same notes); `resolveEventClip` extent-shifted;
`takes-split` splits event columns; `overdub`/`replace` keep their MIDI
meaning (§3.3) and the documentation says so beside the audio meaning.

Gates: `midi_rec_union_r2.qxa` (X16: an event clip `[0, 32 bars)`, a pass
over bars 8–16 → one column; `assert-midi-events scope="clip" clip="0,0"
take="0"` still holds the old notes and a render through an instrument shows
them audible outside the pass — `scope=column` does not exist and is not
added); `midi_rec_union_tempo.qxa` (`set-tempo` after the pass: every take
still covers the same notes); `midi_rec_split_r2.qxa`; `midi_record_modes.qxa`
extended with `takes-split`; `record_mixed_band_punch.qxa` (guitar audio +
keys MIDI, `[8,16)` punch under `takes`: one union column on each lane).

> **SHIPPED as P3 (QBX-181). FIVE CORRECTIONS from building it**, two of them
> defects without which the row's own gates could not be written.
>
> **(a) `overdub` and `replace` DO NOT go through the planner for events**, and
> this paragraph's "`place-midi-recording` goes through the same
> `snapshot()`/planner split" is true of `takes` and `takes-split` only. §3.3
> already said so twice — "For MIDI it keeps its existing meaning" under each
> of those two modes — and the two readings are genuinely different operations,
> not two spellings of one: audio `overdub` lays a plain clip on top and lets
> both sound, which notes need no second clip for because they SUM; audio
> `replace` is tape, where a note-level replace that keeps the CCs is finer.
> `SEventPassVerbs::removeChild` is therefore unreachable by construction
> (`Replace` is the only planner that removes) and refuses loudly rather than
> guessing at a removal nobody asked for.
>
> **(b) TWO STORED FRAME VALUES ON AN EVENT COLUMN HAD NO MUSICAL ANCHOR**, and
> D6 named neither. The take extents follow the bar for free; the COLUMN's own
> `duration_` and the COMP MAP's segment positions did not. Measured on `main`
> at `set-tempo bpm="60"` over a one-bar union column:
> `extents=[0-192000;0-192000] duration=96000` — the takes reach 192000, the
> column says 96000, and invariant 1′ is false on every tempo change over an
> event column. And on a 32-bar one the extents doubled while the map still
> named the old frames, so the recording sounded over the bars BEFORE its own
> and over none of them. Both fixed, EVENT-ONLY: `duration_` follows
> `derivedDuration()` in `onTakeCutChanged`, and the map gains `compTicks_` —
> `SLink::startTicks_` for a comp map — re-derived by a new
> `SObject::rederiveMusicalPositions()` that `set-tempo` calls in the same loop
> that re-derives the links. The asymmetry is the design: A0a deliberately lets
> an AUDIO column outlast its takes, and an audio comp boundary is a position
> in a recording that must not move when the tempo does.
>
> **(c) THE MIDI RECORDER HAD NO PUNCH, and that lost the whole take** —
> which is why `record_mixed_band_punch` could not be written without fixing
> it. The audio half ends itself at the out point, stopping the TRANSPORT, while
> the MIDI half ran on to the `record-stop`; its final poll then re-anchored
> `SPlayheadClock` from a dead clock, every captured host time mapped to a
> NEGATIVE frame and `stopFrame_` read 0, so `passEnd` was 0, every note was
> outside its own pass and nothing was placed (`msgs=8 placements=0 passes=0
> notes=0 clamped=4`, on every run). It now punches out on its own tick,
> through its own `punchRegion_` — the same two readings of one marker pair the
> audio recorder gives them — and under a punch the pass window is the MARKERS
> exactly, not `stopFrame_`. **A punched MIXED press costs TWO undo steps**,
> because each recorder ends on its own clock and the two commits never share a
> scope; B7's one-step claim holds for a `record-stop` press and the punch case
> is QBX-190.
>
> **(d) THE MODE VOCABULARIES ARE RECONCILED, and the recorder's authority
> moved.** `SMidiRecorder` read `SOpt::MidiRecordMode` raw, in proposal 21 L4's
> three values, so the MIDI half of a press could disagree with the audio half
> about the mode of the same gesture and `takes-split` was unreachable for MIDI
> altogether. It resolves through `srecmode::resolveMode` now, per armed lane,
> like the audio recorder; `new-take` survives as a synonym of `takes` in
> `place-midi-recording`, in `assert-midi-recorded` and in the INI seed, so
> nothing written before plan 53 changes meaning. `midi_record_modes.qxa`
> therefore drives `set-property recordMode` and no longer owns the INI key.
>
> **(e) THE SHARED PLANNER IS ITS OWN LANDED PREREQUISITE (QBX-188).** This row
> was scoped as "wiring rather than a second planner set", and it could not
> have been: `objects/midi` may name nothing in `objects/cut` (proposal 37
> §3.5), where the planner and `merge-columns` lived. The planner moved to
> `app/objects/recplan` at the rank of both window/content pairs, with
> `srecplan::PassVerbs` as the seam; `split-clip` and `merge-columns`, which are
> kind-agnostic already, come through `SActionRegistry` by name on the event
> side — the precedent `makeAddTrack()` set in that same module. One more
> lifetime fix came with it: the plan must be HELD on the action, because
> `merge-columns`' inverse reads its operand recipe back out of the merge
> action. A stack-local composite was correct for `place-midi-recording` until
> this row and segfaulted on the first `undo` after it.
>
> **WHAT P3 DOES NOT DELIVER, and X16's own gate says so.** An event column
> resolves through its ACTIVE take only, so everywhere that take has no
> material the column plays SILENCE rather than the take the comp map names —
> a mode A recording over the middle of a part leaves the part's own bars quiet.
> `resolveEventClip` is extent-SHIFTED here, which is this row's text and is
> what makes the pass sound at bar 8 instead of bar 0, but the comp half cannot
> go through that seam at all: `twEventClipResolved` is one sequence per clip
> and two takes are two sequences. §4.1 already said "the event path has no
> `twCompColumn` to do that in". **QBX-189**, with the fix sketched.

### P4 — groups — depends on P2

Build: Q5 un-armed members with D4 default (a): the recorder's member
enumeration (§5.3), the `armed="0"` path in BOTH takes planners (A: the `C_g`
merge over every member including the armed ones; B: the split at `a`/`b`),
member children split at `C_g`'s edges; 3′ from A0a; `record_mode_replace_group.qxa`
(an un-armed member is NOT trimmed under `replace` — G9; it lives here, not
in P1, where it would be vacuous because P1 never submits for a member);
`select-take take="newest"` (§6.5); take names from the recorder's stamp
(`set-clip-name take=` exists; the recorder sets "Take <n> · HH:MM" on the
new take, which is also D5's origin marker).

Gates: `takes_group_record_unarmed.qxa` (two tracks in a group, one armed,
record over identical clips → both lanes hold a column of the same extent,
asserted by `assert-take-column` on both; `select-take take="newest"
broadcast=1` picks the recording on the armed lane and is a no-op on the
other; `undo` restores both lanes); `takes_group_record_crossing.qxa` (D4: a
member clip crossing `C_g` is split at its edge, the inside piece becomes the
placeholder column, the outside piece stays plain); `takes_group_broadcast.qxa`
extended with `newest` on columns of different take counts (T2c). The
`newest` gate header says that on `main` `readXml` parses `take` with
`toInt()`, so `"newest"` silently selects take 0 — the right reason to fail.

> **SHIPPED as P4 (QBX-182), for AUDIO. THREE CORRECTIONS.**
>
> **(a) `C_g` NEEDED A VERB THAT COULD SAY "THIS IS THE COLUMN'S EXTENT".**
> §3.4 asks for a member's column to be exactly `C_g`, and on a member holding
> less material than the armed lane that is WIDER than anything the merge has
> to fold. `merge-columns` computed `[min start, max end)` over its operands
> and refused fewer than two of them, so neither half of the un-armed plan was
> expressible: not the extent, and not the one-operand case that is the common
> one (a member holding the single clip the armed lane was played against). It
> gained `start=`/`length=`, with `length < 0` keeping the operands' union —
> every pre-P4 call unchanged — and ONE operand accepted only when an extent is
> given, because one operand with no extent produces a column identical to the
> clip it wraps, which is the shape invariant 3 forbids. An extent that does
> not CONTAIN the operands is refused rather than clipped: clipping would drop
> a take's material and `unmerge-columns`' recipe records extents, not
> truncated copies of them.
>
> **(b) THE PLANNER COMPOSES THE TAKE'S NAME, because only the plan knows the
> take's NUMBER.** §8's P4 row says "the recorder sets `"Take <n> · HH:MM"` on
> the new take", and the recorder cannot: it does not know where the pass
> landed (`merge-columns` keeps the first operand's index), nor which take
> index the pass became, nor — the part that is genuinely the plan's — that
> `n` is one past what the operands contributed, counting a column operand's
> takes individually. So the recorder passes `takeStamp="HH:MM"` and the
> planner emits one `nameTake` per placement, as a `set-clip-name take="-1"`
> inside the same composite: it resolves "the active take" at apply time, so no
> index travels and the name is undone with the pass. The FORMAT lives in
> `app/model/stakenames.h`, because `place-midi-recording` names an event take
> the same way and `objects/midi` may not depend on `objects/cut`.
>
> **(c) `newest` ORDERS BY THE TAKE'S INDEX, NOT BY THE STAMP IT READS.** §6.5
> says "the take with the latest recording stamp", and `HH:MM` cannot carry
> that order: it has no date, and two passes in one minute tie. What the stamp
> can say is WHETHER a take came from a recording, which is §6.5's own second
> sentence and is also decision D5's origin marker — so `newest` is "the
> highest-index take whose name carries the stamp, else the highest index of
> all". That is exact rather than approximate, because a recorded take is
> always APPENDED (`merge-columns` names the pass as its final operand,
> `add-take` appends), so a column's take order IS its recording order. The
> stamp says whether, the index says which.
>
> **NOT SHIPPED: the MIDI recorder's member enumeration (QBX-191).**
> `place-midi-recording` has no `armed=` and no group extent, so a MIDI pass on
> a grouped lane leaves its members out of step. The planner needs nothing for
> it — none of the group handling branches on `Context::kind` — so what is
> missing is the verb's attributes and a copy of the recorder's block. The
> `takeStamp` half did land for both kinds, so a recorded MIDI take carries
> D5's origin marker and `select-take take="newest"` works on an event column.
>
> **Also shipped, and §3.5 had already asked for it:** the pass-wide snap now
> collects child edges from an armed lane's group MEMBERS as well, in a takes
> mode, because P4 is what makes the pass edit them.

### P5 — seam auto-fade (Q9, D3) — depends on A0a (the xfade clamp), B8

Build: `SProjectProps::AutoFadeMs` default 3.0, seeded at creation (D3a (a));
`twTrackMix` applies `min( autoFade, clipLen/2 )` at every clip edge whose
`twClipFade` is `none()` with `autoIn`/`autoOut` true — EXCEPT a seamless
edge (D3b (a): the neighbour on that side is a clip over the same
`windowContent()` with `srcEnd(left) == srcStart(right)` exactly and the same
stretch; computed in `STrack::refreshClipGainCurves`, which already walks
the lane's links in order); `twCompColumn` applies the same length as a
crossfade at every boundary whose `xfade == 0`, clamped to both takes'
extents; the tri-state on `set-clip-fade` (§6.2); the "Set fade for
selection…" dialog showing exempt edges.

Gates: `auto_fade_seam.qxa` (two butt-joined clips of DIFFERENT sources,
render with `autoFadeMs=3` vs `0`: the 144-frame windows at the seam differ
by the closed-form linear ramp, the rest is `assert-file-identical`);
`auto_fade_seamless_exempt.qxa` (one clip `split-clip` in two under
`autoFadeMs=3` renders `assert-file-identical` to the unsplit clip — the
notch D3b forbids); `auto_fade_override.qxa` (`set-clip-fade auto="0"` on
one clip restores the hard edge there only; `set-clip-fade fadeIn=4800`
replaces the auto-fade); `auto_fade_comp_boundary.qxa` (a comp boundary with
`xfade=0` under `autoFadeMs=3` renders the equal-power sum
`comp_crossfade.qxa` already pins for an explicit `xfade`).

Flips: none — existing files have no `autoFadeMs` key and read 0 (D3a).

> **SHIPPED as P5 (QBX-183). THREE CORRECTIONS**, in increasing order of how
> much they change what the row above says.
>
> **(a) "EVERY CLIP EDGE WHOSE `twClipFade` IS `none()`" IS APPLIED PER EDGE.**
> `none()` is a property of the whole fade — it is true only when BOTH lengths
> are 0 — so read literally the row says that drawing a fade-IN turns the
> auto-fade off at the clip's OUT edge as well. That is not what a user who
> drew one fade asked for; they asked for a fade at one end, not for a click at
> the other. The rule as built is per edge: an edge is left alone when it has
> an explicit length, when its `auto` flag is false, or when the join is
> seamless. Per edge is a SUPERSET of this row's reading, and
> `auto_fade_override.qxa` pins both halves (`autoOut="0"` leaves the in edge
> fading; an explicit `fadeIn` leaves the out edge fading).
>
> **(b) `STrack::refreshClipGainCurves` DOES NOT "ALREADY WALK THE LANE'S
> LINKS IN ORDER".** This is simply wrong, and it is the one claim here that
> would have produced a bug rather than a difference of reading.
> `SObject::childLinks()` is the container's own child order;
> `SObject::windowStep`'s contract says in as many words that "the caller then
> walks childLinks() by start time". Relying on the row's claim would have made
> the seamless exemption depend on the order clips happened to be ADDED in —
> which reproduces on a reload and not on the edit, the worst shape a bug of
> this kind can have. `sautofade::laneSeams` sorts, and
> `auto_fade_seamless_exempt.qxa`'s third control is what makes the sort
> load-bearing: `split-clip` APPENDS, so splitting the head of an
> already-split clip leaves the newest piece LAST in child order while it sits
> in the MIDDLE in time, and unsorted NEITHER neighbour pair abuts. Measured:
> with the sort deleted that case fails and nothing else does, because every
> other arrangement in these gates was built in time order and the two orders
> coincide.
>
> Two smaller things followed from where the walk had to go. The seamless
> question needs the LANE (which clip abuts which) and the window test needs
> only `SClipWindow` (content, loop state, stretch, source position), so the
> whole of it moved to a new `model` module, `app/model/sautofade.h`: this
> feature has two consumers that may not see each other — `objects/track` at
> clip edges and `objects/cut` at comp boundaries — and the panel is a third
> reader of the same answer. And two placement-level preconditions had to be
> added that this row does not mention: the clips must abut EXACTLY (a gap is
> a gap and an overlap is not a continuation), and the window must span the
> whole placement, because since A0a `SClipWindow::parametersOf` may answer
> with a take that covers LESS than its column and its source position at its
> own end is then not the placement's. Refusing there means the edge gets its
> fade, which is the safe direction.
>
> **(c) THE "SET FADE FOR SELECTION…" DIALOG IS THE CLIP PROPERTIES PANEL'S
> NEW FADE GROUP.** §6.2 is edited to match. Proposal 31 had already moved the
> per-clip properties out of the clip context menu into that panel, precisely
> so that one place would edit the SELECTION with the absolute-set multi-select
> semantics, the mixed-value blanking and the one-composite-per-edit undo
> worked out; a dialog would be a second copy of all of it, and a modal
> `exec()` reachable from a scripted run has to be made to return immediately
> under `--test-case`. The group also carries the thing §6.2 asks for and a
> dialog would have had to invent: a status line naming which of the
> selection's edges are EXEMPT, read from `sautofade::laneSeams` — the same
> function the mix reads, never a second implementation, because a status line
> that disagreed with the audio looks exactly like one that is right.
>
> One thing the row asks for that is NOT in this change: nothing was done about
> the auto-fade in the Options UI. The property is reachable from a script
> (`set-property key="autoFadeMs"`) and from a project file, and a new project
> gets 3 ms, but there is no control anywhere that sets it, and the 3 ms is a
> hard-coded literal rather than an INI key — so a user cannot change what new
> projects get either. §6.2 does not ask for a control and this row does not
> either.
>
> **FILED AS QBX-192**, and the gap turned out to be wider than the auto-fade:
> `SOpt::MidiRecordMode` (P2's seed), `SOpt::LocatorAfterRecord` (Q12) and
> `SOpt::TakesExpandOnLanding` (Q11) are all in the same position — an INI key
> with a documented default and no control on any Options page — and the last
> two are what **P6** consumes. So the ticket builds ONE "Recording" page for
> all four rather than letting three changes touch the same page in turn. The
> per-PROJECT value is left as a decision there, because this app has no
> project-settings surface at all and P5 was not the place to invent one.

### P6 — landing feedback, locator, unused recordings, clean-up — depends on P2, A0b, B10

Build: auto-expand on landing (Q11: when ≤ 2 tracks gained a take,
`setTakesExpanded(true)` on them; otherwise a `postHint` "Recorded into N
columns on M tracks (T to show takes)"); `transport/locatorAfterRecord`
honoured in `SAudioRecorder::stop()` (Q12 — `stay` leaves the locator where
the take stopped; the transport still stops, the known limitation in
`shell/CONTRACT.md` is unchanged); the "Unused recordings" dock and
`cleanup-takes` (§6.3, D5 default (a)) including the `remove-take` real
inverse for non-file-backed takes.

Gates: `takes_auto_expand.qxa` (`assert-lane-view` reads `takesExpanded` on
one track after a pass; a three-track pass leaves all three collapsed and
`assert-log` has the hint); `record_locator_stay.qxa` (`set-option
transport/locatorAfterRecord stay`, record, `assert-locator`); `cleanup_takes.qxa`
(a column with an unused RECORDED take, a dangling segment, a used inactive
take and an unused ORIGINAL take: only the unused recording goes, the used
and the original stay — asserted by render identity before and after and by
`assert-take-column takes=`; `undo` brings it back; a MIDI take goes through
the same and comes back, closing plan 52 §6's hole);
`unused_recordings_list.qxa` (record, `undo`, the dock lists the file
through a new `assert-unused-recordings count=`; `files="1"` is NOT
exercised headlessly — moving to the OS trash in a `-j4` suite is not
something a case may do, and the PR says so).

### P7 — QBX-157 backdrop and cut marks — depends on A0b, B4

Its own ticket; its acceptance is written there. It depends on A0 because the
collected envelope must honour extents (a take's envelope outside its extent
is nothing, not the source's continuation).

## 9. What each existing gate does when A lands (the flip list, in one place)

Nothing flips in B1–B10, M0 or P1. The flips are A0a's version bump and P2's
behaviour change, and each is edited in the PR that causes it.

| gate | today pins | after | in |
|---|---|---|---|
| `load_unknown_object_survives.qxa:44-47` | a legacy file re-saves as `formatVersion='2'` | `'3'` | A0a — **DONE**, and it was the whole flip list: nothing else in the suite moved |
| `takes_recording_placement.qxa:40-51` | silence over 192000–288000 | old material's source sec 2–3 (RMS 0.26–0.33, 0.37–0.44); one lane child | P2 |
| `place_recording_over_earlier_clip.qxa` (1),(2) | the earlier clip is overlapped, recording is a plain cut at `0,1`/`0,2` | one union column at `0,0`; audibility asserted instead of child count | P2 |
| `place_recording_over_earlier_clip.qxa` (3), line 61 | `assert-clip-window clip="0,1" startTime="48000"` | cannot hold once (1),(2) made one column: rewritten to `assert-take-column clip="0,0"`; B5's `clip="0,2" shape="none"` assertion is replaced by `takes=` on the one column | P2 (B5's own edit is to the gate as it stands on `main`) |
| `record_stays_armed.qxa:73` | `assert-clip-window clip="0,1"` exists | ~~the two jittered takes overlap whatever the jitter and form ONE column~~ — **THIS ROW IS WRONG and P2 measured it so (QBX-180).** It was written when both takes started from locator 0; QBX-171 moved the second to 480000 to get it out of the pass-wide snap's range, which is 10 s from the first pass's clip and disjoint from it under any jitter. Both passes are R0, each is its own plain child, and the existence assertion holds exactly as written. The case gained a header paragraph saying so; nothing else | P2 — **DOES NOT FLIP** |
| `record_snap_disagree.qxa:71` | `assert-clip-window clip="1,1" startTime="388944"` | **A FLIP THIS SECTION DID NOT PREDICT (QBX-180).** The landing at 388944 is past lane 0's clip end (388800) but INSIDE lane 1's clip (which runs to 389088), so lane 0 is R0 and lane 1 is R1: only lane 1 folds, and `1,1` stops resolving. The alignment claim — both lanes stayed on the same frame — is remade on lane 1 through the comp map, and more sharply: the column starts at 197088, so two `takeAtPos` assertions at 191855 / 191856 pin the landing to the exact frame where `startTime=` pinned it to a clip's start | P2 |
| `record_loop_takes.qxa`, `record_loop_late_start.qxa`, `record_loop_overshoot*.qxa` | one column at `cycleIn`, one take per pass, late start = leading silence | unchanged: an empty lane under cycle is R0 then R3 on every pass, the union IS the cycle, and §3.1 step 5 keeps the whole-cycle extent for `fromCycle` | — |
| `record_punch.qxa` | exact `[in, out)` on an empty lane, `undo` → 0 clips | unchanged; the B7 outer macro is opened only when a recorder commits, so an empty pass pushes no undo entry (`record_mixed_one_undo.qxa` asserts that too) | — |
| `midi_record_modes.qxa` | three modes through the INI | rewritten in M0 (project property), extended in P3 | M0, P3 |
| `takes_group_broadcast.qxa` | positional fan-out | unchanged; extended in P4 | P4 |
| `comp_map_model.qxa`, `comp_map_audible.qxa`, `comp_crossfade.qxa`, `clip_fade*.qxa` | the map and the fades | unchanged through A0a/A0b (full-extent takes are the degenerate case) | — |
| `take_extents_audible.qxa`, `take_extents_model.qxa`, `take_extents_remove_undo.qxa`, `take_extents_split.qxa` | A0a's resolution rule and its restores | **ALL FOUR FLIPPED BY A0b** — one cause, and A0a's own headers predicted it. Each reached "an empty map over a partial ACTIVE take" (or isolated one take to listen to it) by CLICKING the take, because `select-take` then meant "this take everywhere, map cleared". §4.3 made the click extent-aware, so it now keeps the other takes audible where the selected one has nothing — which is the opposite of isolating one, and means the click can no longer REACH that state at all. Each case now states its premise directly with `set-comp-map` (no segments); the rules they gate are unchanged word for word. **This is the cost of §4.3 that the plan did not price: a side effect four gates were built on.** | A0b (done) |
| `load_unknown_object_survives.qxa` | `formatVersion` | flipped 2 → 3 by A0a; unchanged by A0b, which adds no serialized field | — |

## 10. Risks and open points

Only the genuinely open ones, and every place a decision collides with another
or with the code. The six that need the USER are in §0a (D1–D6) and are only
pointed to here.

1. **Q1 (A as default) re-opens proposal 43 §7.** Per-take extents are the
   capabilities 5 and 6 that 43 excluded by design. A0a is a model change on
   the hottest object in the codebase; proposal 42 found 43 consumers of the
   column's SHAPE and ten defects among them. §4.1 now names the nine
   delegates and five consequences review 1 found missing, but the PR must
   still COUNT its consumers, and A0a lands alone and soaks before P2 — which
   is why the framework and the non-A modes (P1) go first.
2. **D1** — Q7 under A removes the SUM of legacy overlapping clips. See §0a.
3. **Q5 under A needs invariant 3′** (a legal one-take column with a partial
   take) to give an un-armed member a positional twin when it has a single
   touched child. Without 3′ the member gets no twin and group comping finds
   nothing to fan out to. 3′ is also what `cleanup-takes` and `remove-take`
   leave behind. Flagged because it widens what a column can be; D4 governs
   the geometry.
3a. **§4.3 REMOVED THE ONLY "HEAR ONE TAKE ALONE" GESTURE, and under mode A
   that will be every take.** Found while building A0b (QBX-178): four A0a
   gates were built on `select-take`'s map-CLEARING side effect, because
   clicking a take was the only way to make one take the whole column. §4.3 is
   right to change it — clearing the map on a PARTIAL take silences every span
   that take does not cover — but it leaves nothing in its place. For a
   full-extent take the click still clears, so today's projects are unaffected;
   under mode A (§3.1) takes are PARTIAL BY CONSTRUCTION, so from P2 on a
   take-lane click will NEVER isolate a take, and the gesture a comping user
   reaches for first — "let me just hear that one" — has no spelling at all.
   The suite's own workaround is `set-comp-map` with no segments, which is a
   scripting verb and not a gesture.

   This is NOT A0b's to fix: §4.3 is what the plan specifies and it shipped as
   specified. It is **P2's or QBX-157's**, and it needs a decision rather than
   code — a modifier-click, a separate "solo take" control, or an explicit
   "this take for the whole column" command distinct from `select-take`.
   Filed as **QBX-186** so P2 cannot land without an answer.

4. **Kw (a wrapped column `normalizeColumns` refuses to fold) is refused in
   every mode.** The alternative — treating the wrapper as an opaque take —
   nests a column in a column, which proposal 42 M3 forbids. A refused lane's
   file goes to the unused list; the user re-records or unwraps by hand.
   Measured in the field: `takestack_wrap_wrapperpan.qxp` and
   `takestack_wrap_looping.qxp` are such columns.
5. **B7 reverses a documented decision** (`sapplication.h`: "one undo step per
   recorder"). The reason it was written — MIDI commits while the transport
   runs — still holds and is unaffected by an enclosing macro. Contract edit.
   The macro is conditional (§5.4) or every empty pass would push an undo step.
6. **D3a / D3b** — the auto-fade's default scope and its behaviour at
   seamless joins. See §0a. Under D3b (a) the seamless test is computed per
   lane walk in `refreshClipGainCurves`; a clip whose neighbour changes must
   re-run it, which that funnel already does on every lane edit.
7. **D2** — "track group" = folder track. See §0a.
8. **The recorder, not the verb, owns the snap (§3.5)**, so a scripted
   `place-recording` never snaps. That is right (a script says what it means)
   but it means the snap gates are LIVE cases with the file input backend,
   from a STOPPED transport (review 1 #8: a running one jitters by far more
   than N), which run serially. Two such cases are named in P1; they are the
   only ones there that are not pure arithmetic.
9. **`select-take` changes meaning (§4.3)** for columns with partial takes.
   The ACTIONS entry and the cut CONTRACT sentence "`select-take` CLEARS the
   map" are rewritten to "selects the take wherever it has material, which
   on a full-extent take is the clear". The take-lane click is unchanged.
10. **D6** — MIDI extents in ticks. See §0a. P3 implements ticks and writes
    the `set-tempo` gate.
11. **B5's interim fix and Q7's final rule differ** (skip the second column
    vs merge it); the gate assertion written in B5 stays true on `main` and
    is REPLACED by P2 (§9), so nothing flips twice, but an implementer must
    not "finish" B5 by implementing Q7 early — Q7 needs A0a.
12. **Q11 toggles `STrack::takesExpanded_`**, a serialized attribute since
    proposal 46 M3 that does not dirty the project and is not undoable. A
    recording therefore changes a saved view attribute silently. Acceptable
    for view state; stated so nobody reports it as a bug.
13. **`cleanup-takes files="1"` is not undoable and not gated headlessly.**
    The trash move is recoverable outside the app; the dialog says so. D5
    governs which takes it may touch at all.
14. **A failed inverse inside a composite is silent** (plan 52 §8's two
    limits). The planners emit composites of up to a dozen children; a
    `merge-columns` inverse that fails during a rollback loses nothing (the
    windows are still somewhere on the lane) but may leave the lane in the
    merged state with no undo entry. The gate `merge_columns_undo_identity`
    covers the normal path only; the rollback path is stated as untested,
    exactly as plan 52 did.
15. **`LegacyTakes` is a deliberate interim** (§8): between P1 and P2 the
    default mode of a new project is today's planner behind the new name.
    Nothing in §9 flips until P2, and a user on a build between the two sees
    exactly today's recording behaviour plus three new modes.

## 11. References that moved since the working documents

Re-read at `c682e944`; everything else cited in QBX-156 is at the line the
ticket gives, within one or two lines.

| ticket said | now | note |
|---|---|---|
| `stakehelpers.cpp:196-197` (wrap copies `startTime` only) | `:202-203` | the disconnect comment grew |
| `stakehelpers.cpp:221-222` (collapse) | `:227-228` | same shift |
| `stakehelpers.cpp:156-171` (`cloneColumn`) | `:162-177` | same shift |
| `ssplitclipaction.cpp:70-101` | `:70-101` | unchanged; the wrapped branch's `cloneColumn` call is at `:160-163` |
| `splacerecordingaction.cpp:80-154` | unchanged | — |
| `saddtakeaction.cpp:50-58, 79-81, 85-91, 92-94` | unchanged | — |
| `stakestack.cpp:123-135, 137-148, 505-521` | unchanged | — |
| `saudiorecorder.cpp:331-338, 519-563, 573-584, 574-578` | unchanged | `devLat` at `:319` |
| `sapplication.cpp:1438-1439`, `sapplication.h:218-219` | unchanged | — |
| `smidirecordactions.cpp:59-78, 295-299, 341, 373-377, 393-403` | unchanged | — |
| `soptions.h:151` | unchanged; the file is `servicesui/include/app/servicesui/soptions.h`, not `model/` | the review's path was wrong |
| `docs/ACTIONS.md:291, 296, 312` | unchanged | B8 still open |
| `takes_recording_placement.qxa:40-51`, `place_recording_over_earlier_clip.qxa:53-62` | unchanged | — |
| plan 52 (QBX-146 step a) | landed in `c682e944` | `remove-sample` on a DIRECT column has a pinned inverse; the wrapped shape still refuses (QBX-155). `replace` (§3.3) and `cleanup-takes` (§6.3) lean on that |

## 12. Ticket split

One row per ticket to file; "depends on" names rows in this table or
existing tickets. Order within a group is landing order.

| ticket | one line | depends on | size |
|---|---|---|---|
| B1 | wrap/collapse carry link attributes; undo restores them | — | S |
| B4 | `add-take activate=1` clears the comp map (inverse restores); painter marks comped columns (or defers to QBX-157) | — | S |
| B3 | `split-clip`, `duplicate-clip`, view duplicate carry the comp map; `unsplit` restores both | — | M |
| B10 | `remove-take` renumbers the comp map, drops segments naming the removed take; inverse restores the whole previous map | — | S |
| B2 | kind guard before the wrap in `add-take`; `place-recording` treats other-kind children as R0 | — | S |
| B5 | one recording is never handed to two overlapping columns; gate (3) asserts it | — | S |
| B6 | a skipped or failed lane is reported in the UI and listed as unused; `placeGrowingClips_` uses `placementLaneAt` | QBX-145's report path | S |
| B7 | one undo step for an audio+MIDI press, opened only when a recorder commits (contract edit) | — | S |
| B8 | `ACTIONS.md:312` stale crossfade note | — | XS |
| B9 | `record_stays_armed.qxa` header: not evidence for the structure rule | B5 | XS |
| M0 | record-mode property: project / folder / track, `set-track-record-mode`, INI seed at the new-project seams, `mode=` plumbing, transport selector, two per-user prefs; `midi_record_modes.qxa` rewritten; `record_mode_ini_seed.qxa` owns the INI key | D2 (default: folder) | M |
| P1a (**QBX-171**) | planner framework with the simulated child list (`srecplan`); `LegacyTakes` behind `takes` (no flip); `overdub`; `place-recording` as the thin verb with `mode=`/`armed=`; the recorder's pass-wide snap and hand-over | B1–B5, M0, QBX-146 step a (landed) | M |
| P1b (**QBX-172**) | `takes-split` (§3.2, appended-index bookkeeping) and `replace` (§3.3) over `split-clip`/`remove-sample` — NOT `resize-clip`, see §3.3; the held-plan lifetime fix §5.1 needed; G12 recorded as unrepresentable and gated as the PRECEDENCE in `record_loop_takes` | P1a | M |
| A0a **(SHIPPED, QBX-177)** | take extents, model only: `duration_`, extents in `insertTake`/loader, resolution rule, `twCompColumn` extents + xfade clamp, invalidation, split per-half/re-index/identity inverse, `remove-take` extent inverse, `add-take at=/length=`, `FORMAT_VERSION 3` + its flip | B1, B3, B4, B10 | L |
| A0b | **SHIPPED (QBX-178).** Take extents, edit surface and view: extent-aware `select-take`, `resize-clip` clamp/refusal, take-lane slip, hatch in both painters, **plus two items A0a handed over by name**: `hasPreview`/`getPreview`' extent window (it moves with the painters, or the picture is half-windowed) and `resize-clip`'s LEFT-EDGE loop, which still writes the column's duration onto every take — unreachable today, noted at the site. Added `assert-envelope take=` (a script had no way to read the per-take collect) and `STakeStack::nextCompEdgeAfter`, the one comp-edge walk the model, the compact painter and the take lane all answer with. **The HATCH's pixels stay QBX-157's**; A0b gates the silence and the envelope beneath them | A0a | M |
| A1 | **SHIPPED (QBX-179).** `merge-columns` / `unmerge-columns` with the link-move protocol. Added a refusal §6.1 does not list — operands must agree on volume and pan and carry no gain curve or fade — because a merged column has ONE set of clip parameters and the row's criterion is that the render does not change (QBX-185 in a second place). The forward also PINS each operand column, which §6.1 says is unnecessary and `SObject`'s own serialized fields say otherwise | A0a, B1 | M |
| P2 (**QBX-180**) | mode A (`takes`) replaces `LegacyTakes`; the §9 flips — **SHIPPED** | P1, A0b, A1, D1 (default (a)) | L |
| P3 (**QBX-181**) | MIDI parity: union/split over event columns, tick extents (D6 (a)), X16, mode semantics documented per kind — **SHIPPED**, on QBX-188's shared planner module. Five corrections in §8's P3 row, of which two are defects it had to fix to be writable at all: an event column's `duration_` and comp map had no musical anchor, and the MIDI recorder had no punch (which silently dropped the whole take of a punched press) | P2, QBX-188 | M |
| P4 (**QBX-182**) | un-armed group members with group extent (D4 (a)); `select-take take="newest"`; take names — **SHIPPED** for AUDIO. `armed="0"` gained meaning and a gate on the same day; `merge-columns` gained an explicit extent, which is what lets a member's column be the GROUP's and not its own clip's. The MIDI recorder does not enumerate members (QBX-191); the take NAME landed for both kinds. Three corrections in §8's P4 row | P2 | M |
| P5 (**QBX-183**) | playback auto-fade at clip edges and comp boundaries, 3 ms seeded (D3a (a)), seamless edges exempt (D3b (a)); `set-clip-fade auto=`; the selection fade surface — **SHIPPED**. The whole policy is one new module, `app/model/sautofade.h`, because the feature has two consumers in slices that may not see each other (`objects/track` at clip edges, `objects/cut` at comp boundaries) and a third READER in the panel. At a boundary it is a LENGTH handed to the crossfade that was already there, in `clampedXfade_nolock` — so a 3 ms auto-fade renders byte-identically to an explicit `xfade=144`. Three corrections in §8's P5 row, one of which is a plain error in this plan's own text | A0a, B8 | M |
| P6 | auto-expand on landing (≤ 2 tracks), locator-after-record pref, Unused recordings dock, `cleanup-takes` (D5 (a)) with the `remove-take` real inverse | P2, A0b, B10 | M |
| QBX-157 | laned-track backdrop = comp result, cut marks | A0b, B4 | M |
| QBX-155 | wrapped column re-link crash (already filed) | — | **LANDED** (#241, #244), so this no longer blocks anything: `remove-sample` pins both shapes. `replace` still refuses a Kw lane, by G11 policy rather than by this limitation — see §3.3 |

## 13. Review 1 dispositions

Review 1 (2026-10-11, AMBER) — every finding re-verified at `c682e944` before
it was acted on. "Fixed" names the section that now carries the answer.

| # | finding | disposition |
|---|---|---|
| 1 | the column has no stored duration; A0 never said where `columnDuration` lives | **fixed** §4.1 (`STakeStack::duration_`, derived on load, written only when it carries information; the honest reason for the version bump) |
| 2 | the delegation surface was not enumerated; the empty-map path plays a partial take at frame 0 | **fixed** §4.1 (resolution rule: any partial take → `twCompColumn`; the nine delegates and what each does) |
| 3a | take-link `startTime` is discarded by the loader (`insertTake` forces 0) | **fixed** §4.1 (`insertTake` extent parameters; loader reads them) |
| 3b | "`remove-take` already renumbers" was false | **fixed** — new bug **B10** (§7) with `remove_take_renumbers_comp.qxa`, prerequisite of A0a and P6 |
| 3c | `remove-take`'s inverse carries no extent | **fixed** §4.1, gate `take_extents_remove_undo.qxa` (A0a) |
| 4 | `split-clip` on a column with partial takes undefined; the refusal rule would refuse it | **fixed** §4.1 (per-half membership, re-index, remap, identity inverse; refusal is `resize-clip`-only), gate `take_extents_split.qxa` |
| 5 | invalidation/resync ignore inactive takes | **fixed** §4.1 (forward for every take; shifted `mapChildRangesToSelf`), gate `take_extents_invalidate.qxa` |
| 6 | index bookkeeping covered appends only; `merge-columns` removes children | **fixed** §5.1 (simulated lane child list; removals descending; `unmerge` restores indices with `moveChildToIndex`) |
| 7 | `snapshot()` must not run `normalizeColumns` (project-wide, mutating, not an action) | **fixed** §5.1 (classify only) |
| 8 | snap gates used an uncontrollable mechanism (running-transport jitter ≫ N) | **fixed** §8 P1 (stopped transport, locator ±144 frames from the edge) |
| 9 | `record_stays_armed.qxa` DOES flip (`clip="0,1"` at line 73) | **fixed** §9, §8 P2 (rewritten to `assert-take-column clip="0,0" takes="2"`) |
| 10 | gate (3) of `place_recording_over_earlier_clip.qxa` also asserts `0,1` | **fixed** §9 |
| 11 | `FORMAT_VERSION 3` flips `load_unknown_object_survives.qxa:44-47` | **fixed** §4.1, §8 A0a, §9 |
| 12 | the INI seed's seam was unspecified and `model` may not include `servicesui`; `set-property` is non-undoable; qxa value is a JSON array | **fixed** §5.2 (seams named: `SMainWindow::fileNew`/`newProject` and `close-project` — review 2 M1 removed `fileOpen()`'s path from the list; a loaded file without the key reads `takes`), §8 M0 (spelling) |
| 13 | INI-key ownership contradicted itself; the seed gate as described cannot run | **fixed** §5.2, §8 M0 (`record_mode_ini_seed.qxa` is the one owner; `set-option` → `close-project` → assert → restore) |
| 14 | Q5 "same extent" ambiguous when a member's clips do not fit `C` | **fixed** — D4 (§0a) with default (a); §3.4 and §8 P4 implement the group extent `C_g` |
| 15 | cycle passes contradicted §3.1 step 5 | **fixed** §3.1 step 5 (`fromCycle` keeps the whole-cycle extent and its leading silence) |
| 16 | the B7 outer macro pushes an empty undo entry when nothing is placed | **fixed** §5.4, §7 B7 (opened only when a recorder reports it will commit; gated) |
| 17 | `assert-midi-events scope=column` does not exist | **fixed** §8 P3 (`scope="clip" take=` plus an audible render) |
| 18 | auto-fade at a butt join of one source is a notch; the xfade clamp ignores extents | **fixed** — D3b (§0a) default (a) exempts seamless edges, gate `auto_fade_seamless_exempt.qxa`; the extent clamp is in §4.1 and A0a |
| 19 | `cleanup-takes` would remove the user's pre-existing clips as "unused" | **fixed** — D5 (§0a) default (a) exempts original takes; §6.3 and `cleanup_takes.qxa` assert it |
| 20 | `merge-columns` must state the link-move protocol | **fixed** §6.1 (the `wrapCutLinkIntoStack` protocol, four steps, reversed by `unmerge`) |
| 21 | "older build plays the take over the whole column" imprecise | **fixed** §4.1 (the column's length follows the active take's) |
| 22 | `place-clip` negative `startOffset` ungated | **fixed** §8 P1 — now its own arithmetic case `place_clip_negative_offset.qxa` (review 2 m1 showed the live snap case cannot force a negative offset) |
| 23 | the un-armed member's verb attribute unnamed; MIDI member of an audio group | **fixed** §5.1 (`armed="0"`), §3.4 (kind filter makes it a no-op) |
| 24 | `select-take take="newest"` on `main` parses to take 0 | **fixed** §8 P4 (stated in the gate header as the right reason to fail) |
| 25 | name the folder predicate | **fixed** §5.2 (`STrack::hasChildTracks()`) |
| 26 | `setInputExtent` locking and the no-dependency rule | **fixed** §4.1 |
| 27 | the undo text is not logged | **fixed** §8 P4 (clause dropped; both lanes asserted directly) |
| 28 | cite the recorder's file pattern line | **fixed** §6.3 (`saudiorecorder.cpp:207`) |
| sizing | split A0 into A0a/A0b; land the framework + B/overdub/replace before A0; B10 after B4 | **adopted** §8, §12. One consequence the review did not name: until mode A lands, `takes` must keep running the legacy planner (`LegacyTakes`), or the default mode of every new project would be refused — §8 says so |
| gate-side | "a system lane CAN be armed today — no role check in `sliveinputactions.cpp`" | **rebutted.** `sliveinputactions.cpp` `resolveTrack()` calls `splacements::refuseSystemLane( track, verb )` for `arm-track`, `set-monitor-mode` and `set-track-input` (proposal 45 AC5.2), and `docs/ACTIONS.md:58` lists `arm-track` among the verbs a system lane refuses. So X17 is NOT reachable through `arm-track`, the first draft's gate `record_start_refuses_system_lane.qxa` could never have run, and B6 is re-scoped in §7 to the REPORTING half plus a one-line guard with no reachable gate |
| D-list | D1–D6 | **adopted** as §0a, each with options, a recommendation, and the phase that implements the default |

### Review 2 dispositions

Review 2 (same day, AMBER; the `arm-track` rebuttal accepted). Each claim
re-verified at `c682e944` before acting on it.

| # | finding | disposition |
|---|---|---|
| M1 | the INI seed must not run at `fileOpen()`'s `new SProject()` — property loading merges over `defaults()`, so every existing project would read the INI value | **fixed** §5.2 (seams = `fileNew`, `newProject`, `close-project`; "never in `fileOpen()`" with the reason; `record_mode_ini_seed.qxa` asserts a loaded fixture reads `takes`), §13 #12 |
| M2 | `take_extents_audible.qxa` encoded a fall-back to another take where the active one has no material, which §3.1 step 5 forbids | **fixed** §8 A0a (active = full take 0 with `96000→1`; then active = partial take with an empty map renders SILENCE over the uncovered span) |
| M3 | the split inverse "restores by identity" was half true (crossing takes are clones); each half's `activeTake_` undefined | **fixed** §4.1 (mixed inverse: moved takes by identity, crossing takes by delete-clone/re-grow; maps and order restored; per-half active rule), gate asserts `activeTake=` on both halves |
| M4 | `rec_union_cycle_inside.qxa` asserted "every take extent the whole cycle", contradicting §3.4 C8's old clip `[6,20)` | **fixed** §8 P2 (recorded takes = the cycle; the old clip keeps its extent) and §3.1 step 5 (X8 is about recorded passes) |
| M5 | which phase submits `armed="0"` passes was open; P1's planners had no un-armed behaviour | **fixed** §5.3, §8 P1 (P1 does not enumerate members; every P1 planner treats `armed="0"` as nothing to do), §8 P4 (enumeration + both takes planners; `record_mode_replace_group.qxa` moved there) |
| M6 | extending an armed lane's column to `C_g` left R0 children inside `C_g` undefined | **fixed** §3.4 (one rule for every member: touched set = children overlapping `C_g`, split at its edges; they become takes) |
| m1 | the snap-earlier case cannot force a negative `startOffset` (the handle is trimmed from a stopped transport) | **fixed** §8 P1 (assert the snapped start and head content; `place_clip_negative_offset.qxa` as its own arithmetic case), §13 #22 |
| m2 | `willCommit()` must be computed before `stop()`; a `b <= a` punch yields empty segments yet opens the macro; the "no input" empty pass is not deterministic; `assert-unsaved-changes` needs a `save-project` | **fixed** §5.4 (segments planned once, before `stop()`; "non-empty segment list"), §7 B7 gate (punch region behind the locator; `save-project` first) |
| m3 | `add-midi-take lengthTicks=` already exists and means the sequence's length | **fixed** §8 P3 (`atTicks=`/`extentTicks=`) |
| m4 | `assert-file-contains absent=` exists but is undocumented | **fixed** §8 A0a (spelled `absent="true"`; A0a adds the ACTIONS row) |
| m5 | `hasStartTime()` is unconditionally true, so a take link's `startTime` is already always written | **fixed** §4.1 wording |
| m6 | `wrapCutLinkIntoStack`/`collapseSingleTakeStack` use the NARROW disconnect; §6.1 called the protocol theirs; a live defect for event columns | **fixed** §6.1 ("the protocol … AFTER B1"), §7 B1 (switches both to the broad form; gate extended with an `eventsChanged` double-delivery assertion through `assert-midi-events scope="feed"`) |

### Review 3 dispositions

Review 3 (same day, AMBER; three majors introduced by round-2 edits). Each
re-verified at `c682e944`.

| # | finding | disposition |
|---|---|---|
| R3-1 | "segments planned once, before `stop()`" would plan against a non-final capture total (and the no-anchor fallback rewrites `p0`/`trimmed_` inside `stop()`) — the tail of every take lost | **fixed** §5.4: TWO-PHASE stop — each recorder `endCapture()`s and plans from the final numbers (MIDI's last drain still while the transport runs), `willCommit()` = non-empty planned list, outer macro iff either, then `commit()` both, transport stop/locator unchanged; the CONTRACT sentence "the capture ends FIRST" stays true as phase 1 |
| R3-2 | the B1 `eventsChanged` qxa clause cannot fail on `main`: `trackEventClipChanged` resolves the sender against current links (no-op while wrapped), and after re-link the duplicate connection yields two identical invalidations, never a second event; `scope="feed"` sees no deliveries | **fixed** §7 B1: clause dropped; stated as hygiene with no observable `.qxa`, unit-level slot count acceptable if cheap |
| R3-3 | the snap gate's `assert-recorded-clip sourceAtStartFrame=` reads `clips.front()`, which on this fixture is the pre-existing edge clip | **fixed** §8 P1: assert the recording at its own appended path `0,1` (`assert-clip-window startTime=`), a 48000-frame `assert-audio-energy` head window on a render, and `assert-log` on the `[REC] snap` line |
| m1 | §4.1's serialization bullet still said `startTime` is written only when informative | **fixed** §4.1 ("written as today (always), now non-zero for a partial take") |
| m2 | a take moved wholly into the tail needs its `tStart` shifted by `−inObjOffset` (and back) | **fixed** §4.1 |
| m3 | §3.1 step 1 defines `C` per lane without pointing at `C_g` | **fixed** §3.1 step 1 (pointer to §3.4, "from P4 on") |
| m4 | P1 uses B6's report path but did not list B6 | **fixed** §8 P1 heading (B1–B6) |
| m5 | the B7 empty-pass gate: `save-project` first makes `expect="0"` sufficient; no change needed | **acknowledged**; the sentence stays as written |

### Review 4 dispositions

Review 4 (same day, AMBER; one major introduced by the two-phase stop). Each
re-verified at `c682e944` (`removeGrowingClips_()` inside `commitPlacement_`;
`SCut::isLiveRecording()`; `isRecordingActive()` gating the locator timer and
live-plan rebuild; `pollFinal_()` separate from `commit_()`, whose mapping and
grouping sit above its `beginMacro` line).

| # | finding | disposition |
|---|---|---|
| R4-1 | §5.4 never said when the growing clips leave; phase 1's snap would see them as agreeing `b` candidates on every lane and snap by up to a tick | **fixed** §5.4 step 1 (`removeGrowingClips_()` in phase 1, before the plan and the snap; inv. 26 rewritten), §3.5 step 1 and §5.1 `snapshot()` (skip `isLiveRecording()` children, belt-and-braces) |
| m1 | which phase clears `active_` / stops `timer_` | **fixed** §5.4 step 1 (phase 1 begins exactly as `stop()` does today, so `isRecordingActive()` is false from phase 1 on) |
| m2 | an all-refused pass still pushes an empty outer macro | **fixed** §5.4 (stated as an accepted limit with the reason; `commit()` detects the all-refused case from the undo stack's `count()`/`index()` — review 5 minor 1 — and logs it) |
| m3 | the MIDI plan/commit split point was not at a function boundary that exists | **fixed** §5.4 step 1 (`commit_()` split at its `beginMacro` line; what moves to phase 1 and what stays) |
| m4 | the snap gate's `0,1` path holds only when the shared edge is a clip END | **fixed** §8 P1 |

### Review 5 dispositions

Review 5 (same day, **GREEN**; two minors).

| # | finding | disposition |
|---|---|---|
| 1 | "`commit()` counts applied actions" has nothing to count — `SApplication::submitAction` and `SActionHistory::submit` are `void` | **fixed** §5.4 (the recorder compares the undo stack's `count()`/`index()` before and after its submit loop; no API change) |
| 2 | §3.5's preamble and §5.3 still named `commitPlacement_` as where the snap and the plan live | **fixed** §3.5, §5.3 (both now say phase 1 of the stop and defer to §5.4 as the authoritative description) |
