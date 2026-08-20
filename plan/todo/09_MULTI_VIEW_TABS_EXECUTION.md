# Plan 09 (execution): Multi-view tabs + arrangement extraction

Execution plan for `plan/proposed/09_MULTI_VIEW_TABS.md` — specifically its
**Revision 3** (§9–§13), which is where the driving use case and the nineteen
decisions (D1–D19) live. That document holds the *design and the challenges*;
this one holds *the milestone order, the acceptance criteria, and the gate for
each*.

> **Status (2026-08-20): M0, M1, M5, M6, M7 and the SHELL half of M8 are
> COMPLETE.** The headless half of the use case works and is gated; the central
> widget is now a tab shell with the master as tab 0.
> **M2/M3/M4 are RETIRED by D21** (paths carry their root).
>
> **THE REMAINING WORK IS RE-ORDERED, and it is worth knowing why.** Arrangement
> TABS are not shipped with the shell, because an arrangement tab is only safe
> to edit in once every path-taking verb carries its root. The arranger mints
> paths at **32 sites** feeding **20 distinct action types** (and the track head
> and detail panel add more), and a PARTIAL migration is not a partial feature —
> it is a partial silent corruption, since index {0} exists in every tree and a
> mis-rooted action SUCCEEDS. So:
>
> - **M8a (done): the tab shell, master only.** Zero UX change.
> - **M8b (next): the D21 verb migration** — every path-taking action accepts a
>   qualified path, and the arranger submits its own root. Mechanical and
>   compiler-assisted, but it is the whole verb set.
> - **M8c: arrangement tabs + the extraction gesture**, safe once M8b lands.
>
> M9 (`<render>` follows the addressed root), M10 (local audition) and M11
> (persist open tabs) are unchanged.

> **This plan has been through one adversarial review (2026-08-20) and every
> correction is folded in.** Five of its findings were independently re-verified
> against the code before being accepted. Two things an agent must know before
> reading further:
>
> - **M0 and M1 are DONE.** M0 turned out to be two defects, not one, and both
>   descriptions in the first draft of this plan were wrong in ways only
>   measurement caught — read its note before touching invalidation or the
>   container capture.
> - **Everything from M2 on is open**, and M2 (the `editRoot` seam) has been
>   RETIRED by D21: paths carry their root. See §14 of the proposal.
> - **`assert-track-count` cannot carry the gates an earlier draft of this plan
>   gave it.** It is an end-of-script `<assertions>` kind, it reads `equals=`
>   not `count=`, and it is master-rooted. M1 must build a real verb first. Any
>   agent that finds itself writing `<assert-track-count count="…">` mid-script
>   is following a superseded draft.

**The user-visible outcome, in one sentence:** select a few tracks and a ruler
range, choose *"Extract to new arrangement…"*, and those tracks leave the master
into a tab of their own while one clip — an asset windowing the range — takes
their place in the song; that tab can be edited, auditioned, placed again
elsewhere, and dissolved back.

---

## How to execute this

**One milestone per branch, one PR per branch, in the numbered order.** They are
ordered by dependency, not by preference; M(n) assumes M(n−1) has landed.

```bash
git worktree add .claude/worktrees/arr-m<N> -b feat/QBX-arr-m<N>-<slug> main
cd .claude/worktrees/arr-m<N> && ./build.sh
```

Every PR pays the full gate set from `CLAUDE.md` — there is no CI, so this is the
entire safety net:

```bash
./build.sh                                    # the re-configure is load-bearing (CONFIGURE_DEPENDS)
python tools/check_layering.py
python tools/check_logging.py
python tools/check_editroot.py                # NEW in M3 — mandatory from M3 onward
ctest --test-dir smaragd/build -j4 --output-on-failure
```

Then **reconcile the count** (registered vs run vs disabled, `ctest -N` against
the run's own summary) and quote what you measured — never a figure from a
document. For the record, measured during review on 2026-08-20: **200 `.qxa`
cases**, **125** of them containing `<render>`, **42** `RUN_SERIAL`. `CLAUDE.md`
says ~90 cases and 147 renders; both are stale.

Five house rules that apply to every milestone, and that an agent skipping them
will produce a green suite that proves nothing:

1. **Demonstrate every new gate FAILING first.** Run it against the binary
   *before* the change, watch it fail, record the output in the PR body. A gate
   nobody has watched fail is not evidence — and check it sits on the same side
   of the seam as the thing it claims to catch (the `preview_volume_independent`
   lesson: a script-level assertion reading through the same helper as the bug is
   blind to it by construction).
2. **A new `.qxa` needs a configure pass** or `ctest` reports all-green having
   never run it.
3. **Run new `.qxa` cases from `smaragd/tests/cases/`** — fixture paths are
   CWD-relative.
4. **Every new verb gets a row in `docs/ACTIONS.md`**, a `knownAttributes()`
   declaration, and an `action_roundtrip_test` entry.
5. **The PR body says what was gated AND what was not.**

**STOP and report rather than improvising** if: an AC turns out unreachable as
written; a gate you were told to write **passes** on the pre-change binary (it is
on the wrong side of the seam — say so, do not weaken it); a **golden moves**
(that means something reached the master sum); or `ctest` regresses outside the
files you touched.

---

## Key files (verified 2026-08-20; line numbers re-checked under review)

| Thing | Where |
|---|---|
| The project, its flat child dump, `rootId`, the property store | `main/model/src/sproject.cpp` (`serialize` at `:45-66`, asset-pin release `:524-536`) |
| The asset registry (runtime-only; **one** call site) | `SProject::registerAsset`, called only from `main/objects/mixer/src/screateassetaction.cpp:66` |
| "Give me the root" for action code — **~99 call sites, 69 files** | `main/model/include/app/model/splacements.h:27-30` (`rootContainer`) |
| Index-path addressing (already takes an explicit root; names no `SProject`) | `main/model/include/app/model/sobjectpath.h:20-36` |
| The invalidation walk — **M0's defect** | `main/model/src/sobject.cpp:974-1000` and `:1053-1066`; the dependent edge at `:1108-1127`, registered from `main/objects/cut/src/scut.cpp:1269` |
| The single action funnel | `main/actions/src/sactionhistory.cpp` (`submit`, `drain_`; `SActionUndoCommand` constructed once, at `:89`) |
| Undo-command merge | `main/actions/src/sactionundocommand.cpp:47-72`; the queue-level merge at `sactionqueue.cpp:8-16` is **dead code** (synchronous drain empties the queue before every enqueue) |
| The summing root class | `main/objects/mixer/src/sstdmixer.cpp` (ctor `:489-519`; per-`this` solo `:195-196`, `:223`, `:251`) |
| Solo resolved against the **ambient** root | `main/objects/track/src/strack.cpp:305-306`, `:1367-1369`, `:1536`; `main/shell/src/smidioutpump.cpp:231-232`, `:444-445`; `main/timeline/src/strackdetailpanel.cpp:229` |
| Track move between containers (copy its `addRef` pin discipline) | `main/objects/mixer/src/sreparenttrackaction.cpp` |
| Asset create / place / **remove (`{}` hazard, D17)** | `main/objects/mixer/src/s{create,place,remove}asset*.cpp`; the hazard at `sremoveassetaction.cpp:33-39` |
| Container capture — synchronous, UI thread, unbounded | `main/objects/cut/src/scut.cpp:319-322`, `:425-460`, `:553-560`; early-return `:325`; `rebuildReader` `:175-177`, `:1110` |
| The arranger (128 `model_` occurrences / 113 lines / 4901 lines) | `main/timeline/src/sstdmixerview.cpp`; **path-vs-root mismatch** at `:548`, `:629`/`:650`, `:689`/`:706`, `:4447`; ruler duration `:4761` |
| The asset-from-range gesture (single-track outlier) | `SMVActualView::ctCreateAssetFromTrack`, `sstdmixerview.cpp:2110-2127`; the multi-track norm is `pruneNestedTargets(selectionTargets(...))` at `:1239`, `:1276`, `:1295`, `:1345`, `:3413` |
| The detail-editor factory (class-name-string keyed) | `main/model/src/sdetaileditors.cpp:9-25`; registration `sstdmixerview.cpp:4896-4901` |
| The central widget + the arranger funnel (**private**, 29 call sites, all in-file) | `main/shell/src/smainwindow.cpp` (`ensureArranger_` `:2149`; construction `:424`, `:499`, `:650`; casts `:234`, `:2979`, `:2985`) |
| The engine/render/speaker root funnel | `SApplication::rootComponent()` `sapplication.cpp:791`; `rewireSpeaker()` `:117`; `beginRun()` `:810`; master meter re-tap `:452-461` |
| **The speaker mints a fresh engine every start** (why D9 is cheap) | `tw303a/playback/src/twspeaker.cc:114-132`; `~AudioEngine` `audio_engine.cc:63-70`, dropped demand `:757-761` |
| The render root — reads neither master nor edit root | `main/actions/src/srenderaction.cpp:67-69`, duration default `:94` |
| Loader: single `rootId`, deferred resolvers, handle-link teardown | `main/persistence/src/sprojectloader.cpp:64`, `:254-268`, `:274`, `:403-416` |
| The testkit runner's two `<assertions>` kinds | `main/testkit/src/sactionrunner.cpp:150-160`, `:173-195` (`equals`, master-rooted), `:196-235` |
| In-sequence assertion verbs to model new ones on | `main/testkit/src/strackselectionactions.cpp` (`assert-track-name`, `-volume`, `-selection`) |

**Fixtures.** `tests/test_sawtooth.wav` — verified 2 ch / 48000 Hz / 16-bit /
192000 frames, per-second RMS **0.067 / 0.176 / 0.291 / 0.405**, **channels
byte-identical** (so it can never gate a channel claim; use `tests/test_stereo.wav`).
48 kHz ⇒ 1 s = 48000 frames.

---

## M0 — Cross-root invalidation (**BLOCKING; verified defect**)

> **M0 IS COMPLETE (2026-08-20). Both halves are implemented and gated**, and
> what was measured differs from what was read — twice. Record for whoever
> touches invalidation next:
>
> 1. **An `SCut`'s content link IS a child link** (`scut.cpp:1253-1254`
>    `setParent(this)`; `SObject::childEvent` `:864-871` appends any
>    `qobject_cast`-able `SLink` to `childOrder_`). So
>    `invalidateRenderChainsContaining` — which iterates `childLinks()`
>    unconditionally, unlike `findPathRec`, which skips non-containers —
>    **descends THROUGH a placement into its content**. A *placed* arrangement
>    is reachable from the master walk after all, and an in-place asset is
>    unaffected entirely (control case: 3/3 deterministic). The review's "every
>    structural edit, everywhere, silently inaudible" was too broad.
> 2. **The first fix was necessary and insufficient.** Walking every registered
>    root took the case from failing every run to failing 3-in-6. What remained
>    was a genuine RACE, and it is worth knowing its shape because nothing in
>    the code suggests it: **the container capture is rebuilt on a revalidator
>    worker while the edit bumps the content epoch on the main thread.** A
>    rebuild that starts a few hundred microseconds early reads the PRE-EDIT
>    pages, publishes them, and sets `readerTried_` — after which
>    `ensureReader()` returns immediately and the stale snapshot is what the
>    next render hears, until some later edit happens to invalidate again.
>    Caught by logging the epoch and a sample value inside the capture loop: the
>    passing runs build at epoch 6, the failing ones at epoch 4 with the epoch-6
>    build arriving *after* the render.
> 3. **Two wrong theories were tested and discarded before that**, both cheaply,
>    both by experiment rather than argument: `everHadCapture_` gating
>    `onArrangementChanged` (removing the gate left it just as racy), and a
>    queued-connection delay (the connect is direct).
>
> **The fix is a STAMP, not an ordering** (`SCut::captureContentEpoch_`): a
> capture records the content epoch it was built from, and a mismatch is a MISS
> that rebuilds on next access. That makes it self-healing under any ordering
> instead of forcing one — the same discipline as proposal 36 §4.5's width
> check on a cached page. Measured: **20/20 green fixed** (and 5/5 at each of
> `SMARAGD_REVAL_WORKERS` 1/4/8/16), **6 failures in 12 runs broken**.
>
> **The gate is PROBABILISTIC and says so.** `arrangement_edit_audible.qxa`
> runs four independent edit/render pairs precisely to raise detection; one
> pair detected 4-in-12, four pairs detect 6-in-12. It cannot be made
> deterministic without test-only sequencing hooks in production code, which
> was judged the worse trade.

## The mechanism

`invalidateRenderPath()` (`sobject.cpp:987-1000`) and `invalidateRenderPathRange()`
(`:1053-1066`) both walk down from `project->getRootComponent()` and bump the
epoch of every chain that *contains* the target. Under a detached root the walk
finds nothing, `contains` is false everywhere, and **nothing is bumped — not even
the object's own epoch**, because the fallback is guarded on `root == nullptr`,
not on "not found". The epoch is the *sole* staleness signal in the page cache,
so the first freeze of a position is correct and every later edit is ignored: no
log line, no rejected action. `SCut::buildCapture_` early-returns while a
snapshot exists (`scut.cpp:325`), so the asset's first snapshot plays forever.

The split is diagonal, which is worse than uniform failure —
`notifyDependentsChanged()` (`:1108-1127`) *does* cross the reference edge
(`scut.cpp:1269`), but has four callers, all property setters (`:29`, `:39`,
`:49`, `:88`):

| Edit inside a detached arrangement | Reaches the placement? |
|---|---|
| mute / solo / volume | **Yes** |
| add / move / split / remove a clip; plugin insert, bypass, param; automation; take switch | **No.** Silent, permanent |

**Build (D13, D14):**

- `invalidateRenderPath()` and `invalidateRenderPathRange()` consult the master
  root **and every root in M1's arrangement registry**, bumping through whichever
  contains the object, and fall back to `bumpRenderChainEpoch()` when none does —
  strictly better than today's silent nothing. A loop over a registry M1 already
  builds; one function each.
- **Ordering note:** this needs M1's registry to exist. Land M1 first *or* land
  M0's mechanism against a registry stub in the same PR — but M0's **gate** needs
  M1's verbs, so in practice: M1, then M0, then nothing else until M0 is green.
  Whoever sequences it must not let an audio AC from M2 onward be written before
  M0 lands.
- Range-scoped edits map through the cut's window on the way out; `SCut` already
  owns that arithmetic (proposal 18 Phase 5's `mapChildRangesToSelf`). Reuse it.
- **D14:** `buildCapture_` must **refuse** to publish when it broke out of the
  page loop on a defused page, rather than publishing the zero-fill and setting
  `everHadCapture_` (`scut.cpp:425`, `:446-448`, `:553-560`). An extracted
  arrangement's asset is a container capture by construction, so a path that is
  hard to reach today becomes the normal one.

### ACs

| # | Acceptance criterion | Checked by |
|---|---|---|
| 0.1 | Adding a clip inside a detached arrangement changes what the placement plays | `arrangement_edit_audible` |
| 0.2 | Move, split and remove inside it do the same | same |
| 0.3 | A plugin insert / bypass / param change inside it does the same | same (`tw.test.clap.gain`) |
| 0.4 | Mute / volume inside it still works — the path that works today must not regress | same |
| 0.5 | A range-scoped edit invalidates only the mapped range of the placement | `arrangement_edit_range` |
| 0.6 | The master case is bit-for-bit unchanged | both goldens + full suite |
| 0.7 | A container capture never publishes silence from a defused page | `assert-log` + `arrangement_edit_audible` |

### Gates

- **New:** `arrangement_edit_audible.qxa` — build the shape with M1's verbs
  (`create-arrangement`, `use-arrangement`, `add-track`, `add-sample`,
  `create-asset` over the arrangement, `place-asset` on a master lane); render;
  make each edit above **inside** the arrangement; render again; assert the audio
  **changed** as the edit implies (per-second RMS against the ladder), and for the
  clip-add case assert the two renders **differ**.
- **New:** `arrangement_edit_range.qxa` — the unedited seconds must be untouched,
  which is what separates a correct range hop from "stale everything".
- **This gate is guaranteed to fail before the fix**, so there is no excuse for
  not demonstrating it. Record the pre-fix output verbatim.

### Not gated

Cost of the registry loop per edit (main-thread model walk; measure if it shows
up); an arrangement placed inside another arrangement (the hop composes and M4
bounds the depth — write the case if cheap, else say it is untested); range
precision through two window hops.

### Docs

`main/model/CONTRACT.md` — a new invariant, written as one: *"an edit is
invalidated from the master root and from every registered arrangement root; at
an arrangement boundary it hops the `dependentLinks_` edge."* Plus a `CLAUDE.md`
line in the "the obvious design is wrong" register.

---

## M1 — An arrangement is a named, detached, persistent root — and a root-aware count verb

**Model + testkit only.** Two things, because M0's and M2's gates cannot be
written without the second.

**Build (D2, D3):**

- `SProject`: `arrangementNames()`, `arrangement(name)`, `arrangementNameOf(root)`,
  `registerArrangement`, `unregisterArrangement`. One reference pinned per entry,
  released in the block that already releases the `assetDict_` pins
  (`sproject.cpp:524-536`) — read it first, it is the shape to copy.
- Serialization: **one additive attribute** on the existing `<SStdMixer>`,
  `arrangementName='…'`. No new element, no new section.
- Loading: `SStdMixer::instantiateFromDomElement` reads it; re-registration from
  a **`deferResolve`** (which runs inside `createObjects()` at
  `sprojectloader.cpp:274`, before `~SProjectLoader` deletes the handle links at
  `:403-416`). **Without the pin the arrangement is deleted at end of load** —
  note the death is `deleteLater()` (`sobject.cpp:685-705`), so it will not look
  like a crash. Write the pin last and watch the round-trip gate fail.
- Verbs `create-arrangement name=` ↔ `remove-arrangement name=`, mutual inverses.
  `remove-arrangement` **refuses** while any placement of any asset over that root
  exists.
- **Also fix the asset registry's own round-trip gap:** an `assetName='…'`
  attribute on the asset's `<SCut>`, re-registered from the same `deferResolve`.
  Same defect, same place; leaving it means an extracted arrangement loads with
  no asset name.

**Build (testkit — required by M0, M2, M3, M5):**

- A **root-aware, in-sequence** `assert-track-count`:
  `<assert-track-count arrangement="Drums" count="2"/>`, `arrangement=""` = master.
  It must be a real `SAction` — model it on `assert-track-name` /
  `assert-track-volume` in `main/testkit/src/strackselectionactions.cpp` — because
  the existing `<assertions>` kind is evaluated **after the whole action list**
  (`sactionrunner.cpp:150-160`), reads attribute **`equals`** (`:186`, so
  `count="2"` parses as `-1` and fails unconditionally), and resolves
  `project->getRootComponent()` with a `dynamic_cast<SStdMixer*>` (`:177-180`).
  Keep the old `<assertions>` kind working for the cases that use it; do not
  migrate them in this PR.
- `assert-arrangements names="Drums,Bass" trackCounts="2,1"` — likewise a **verb**,
  not an `<assertions>` kind, or it inherits the same end-of-script limitation.

### ACs

| # | Acceptance criterion | Checked by |
|---|---|---|
| 1.1 | A project can hold N named arrangements besides the master | `arrangement_create_roundtrip` |
| 1.2 | An arrangement with **zero placements** survives save → load with its name, tracks and clips | same |
| 1.3 | An arrangement contributes **nothing** to the master render | `arrangement_not_summed` |
| 1.4 | `create-` / `remove-arrangement` are exactly undoable; remove refuses while a placement exists | `arrangement_create_roundtrip`, `arrangement_remove_refused` |
| 1.5 | A `create-asset`'s name survives save → load | `asset_name_roundtrip` |
| 1.6 | Pre-existing project files are unaffected (the attribute is additive and absent) | full suite + both goldens |
| 1.7 | The new count verb runs **mid-script** and can address a non-master root | it is what 1.2 is written with; verify it failing against the old `<assertions>` spelling |

### Gates

- **New:** `arrangement_create_roundtrip.qxa` — `create-arrangement name="Drums"`;
  `assert-arrangements names="Drums"`; `save-project`; `load-project`;
  `assert-arrangements names="Drums"`; `undo` → `assert-arrangements names=""`.
- **New:** `arrangement_not_summed.qxa` — one master track with
  `test_sawtooth.wav`; `render durationSec="4"`; `create-arrangement`; render
  again; `assert-file-identical`. **Legitimately a byte gate** — an arrangement
  contributing nothing cannot change one sample — and in-process render→render
  `assert-file-identical` is established practice in 24 cases (e.g.
  `tests/cases/automation_clip_gain.qxa:87-88`). This is the gate that catches an
  accidental `insertTrack` into the master later.
- **New:** `arrangement_remove_refused.qxa`, `asset_name_roundtrip.qxa`.
- `action_roundtrip_test` rows for all five new verbs.

### Not gated

Duplicate arrangement names (rejected + warned at `registerArrangement`, no
case); `assert-project-matches` **cannot** be pressed into service here — it
exists (`sactionrunner.cpp:196-235`) but is used by **zero** of the 200 cases and
is unusable because ids are pointer values (`sobject.cpp:133`).

### Docs

`docs/ACTIONS.md` (5 rows), `main/model/CONTRACT.md`,
`main/objects/mixer/CONTRACT.md`, `main/testkit/CONTRACT.md`, `plan/STATE.md`.

---

## M2 — `editRoot` and the `rootContainer` redefinition

**Split from the old M2 deliberately: this milestone is the semantic change
across ~99 call sites. The tool and the verb are M3.**

**Build (D4/D16):**

- `SProject::editRoot()` / `setEditRoot()`, defaulting to the master and **reset
  to it on project load/close**. `editRootName()` returns `""` for the master.
- `splacements::rootContainer(project)` returns `editRoot()`. **Be honest about
  what this is:** one line that re-points **~99 call sites across 69 files with no
  per-site review**. That is the opposite of an audit, and it is why the
  exemptions below are load-bearing rather than tidy.
- **Pin the sites that mean the master**, whichever spelling they use:
  `SApplication::rewireSpeaker` (`sapplication.cpp:117`), `rootComponent()`
  (`:791`), `beginRun()` (`:810` — and note its instrument barrier walks the
  master root, so proposal 37 D4's render-determinism barrier is absent for
  arrangement instrument tracks; either fix it here or record it),
  `SAudioRecorder::collectArmed` (`saudiorecorder.cpp:126`) **and
  `placeGrowingClips_` (`:270`, which resolves through `rootContainer` and would
  otherwise place a take into the wrong tree)**, `SMidiRecorder::collectArmedMidi`
  (`smidirecorder.cpp:122`), `SLiveMonitor::mixer` (`slivemonitor.cpp:95`),
  `SMidiOutPump` (`smidioutpump.cpp:230`, `:444`), `SRenderAction`
  (`srenderaction.cpp:67-69`, until M10 changes it deliberately),
  `smainwindow.cpp:2182/2194/2441/2464/2492/2730`, `sapplication.cpp:732`,
  `smediadrop.cpp:122`, `splugineffectstrip.cpp:178`.
- **The view passes its own root (D16).** `sstdmixerview.cpp:548`, `:629`/`:650`,
  `:689`/`:706`, `:4447` mint paths with `pathOf(model_, …)` and resolve them
  against the ambient root. Four call sites; fixing them removes the largest
  ambient-root failure surface in the feature and makes M8's corruption window
  impossible.
- **Record, do not fix:** `STrack::applyChildTrackAudibility` (`strack.cpp:1367`)
  reads the ambient root from a *model refresh*, not an `apply()`, so D5's
  capture cannot cover it. Same for the non-`submit` `apply()` callers
  (`smainwindow.cpp:316`, `:478`; `scompositeaction.cpp:18`, `:22`;
  `ssettrackvolumeaction.cpp:35`). Name them in `main/model/CONTRACT.md` as known
  leaks with the conditions under which they bite.

**The corrected call-site table** (an earlier draft quoted the `rootContainer`
column while labelling it `getRootComponent`):

| directory | `project->getRootComponent()` | `rootContainer(` |
|---|---|---|
| `main/objects/cut/src` | 0 | 23 |
| `main/objects/track/src` | 3 | 15 |
| `main/objects/mixer/src` | 4 | 5 |
| `main/objects/midi/src` | 0 | 10 |
| `main/selection/src` | 4 | 0 |
| `main/timeline/src` | 0 | 10 |
| `main/eventui/src` | 0 | 4 |
| `main/testkit/src` | 3 | 18 |
| `main/shell/src` | **20** | 10 |
| `main/media/src` / `main/pluginui/src` | 0 | 1 / 1 |

### ACs

| # | Acceptance criterion | Checked by |
|---|---|---|
| 2.1 | The path-addressed verbs act **inside** the arrangement named by `use-arrangement` (M3) | `arrangement_edit_inside` (M3) |
| 2.2 | The master render stays **byte-identical** across arrangement edits | same |
| 2.3 | The engine / render / speaker / recorder / monitor half still asks for the master | the pinned list above + full suite |
| 2.4 | A recording started while an arrangement is the edit root still lands in the master | `record_offset_zero` and friends, unchanged |
| 2.5 | MIDI-out still collects master tracks | the `midi_out_*` cases, unchanged |
| 2.6 | View-originated edits carry the view's own root | `arrangement_edit_inside` + M8's case |

### Gates

The **existing suite, green, count reconciled** is the real gate here — in
particular the 42 `RUN_SERIAL` record / monitor / MIDI-out cases, which are
exactly what the exemption list protects. New cases land in M3, where
`use-arrangement` exists to drive them.

### Not gated

Which of the ~99 sites are semantically right. There is no gate for that; the
exemption list plus the suite is the whole protection, and a site that means
"master" but follows the edit root will only surface once a non-master root is
active — i.e. in M5's cases. **Read, do not sweep.**

---

## M3 — `use-arrangement`, and the tool that polices the seam

**Build:**

- Verb `use-arrangement name=""` — sets the edit root; `""` = master; refuses an
  unknown name. **Not undoable** (a context switch, not a project edit — the
  reasoning that keeps `set-count-in` off the undo stack).
- `tools/check_editroot.py`: police **`rootContainer(` call sites, not just
  `getRootComponent()`** — the latter is blind to the change the tool exists for.
  Scope: `main/objects/**`, `main/timeline/**`, `main/selection/**`,
  `main/eventui/**`, **plus `main/actions/**` and `main/model/**`**. An explicit,
  per-site **commented** allowlist for M2's pinned list (a comment field, not a
  rubber stamp). Same shape and exit convention as `tools/check_logging.py`.

### ACs

| # | Acceptance criterion | Checked by |
|---|---|---|
| 3.1 | `add-track` / `add-sample` / `set-track-volume` / `split-clip` act inside the named arrangement | `arrangement_edit_inside` |
| 3.2 | The master is untouched and its render byte-identical | same (`assert-file-identical`) |
| 3.3 | `use-arrangement name=""` returns to the master; unknown name refused | same |
| 3.4 | The tool fails on a deliberately reverted call site | run it and record the output |

### Gates

- **New:** `arrangement_edit_inside.qxa` — master track + sample; baseline render;
  `create-arrangement`; `use-arrangement name="Drums"`; two `add-track`s;
  `add-sample` on each; `assert-track-count arrangement="Drums" count="2"`;
  `use-arrangement name=""`; `assert-track-count arrangement="" count="1"`;
  render; `assert-file-identical` against the baseline. **Verify it failing
  first** with `rootContainer` still returning the master: the two `add-track`s
  land in the master and the count reads 3.
- `tools/check_editroot.py` clean, **and verified to fail** on a reverted site.

---

## M4 — The action's recorded root, and the merge guard

The hazard from §11.7 and §11.13: an index-path resolved in the wrong tree is a
silent edit, not a failed action. Fix it **once** (D5).

**Build:**

- `SActionHistory::submit()` captures `editRootName()` with the forward action;
  `drain_()` sets the edit root to it for the duration of `apply()` and restores
  after. `SActionUndoCommand::undo()/redo()` use the value captured at
  construction (one site, `sactionhistory.cpp:89`).
- **State plainly why this works**, because "captured in `SActionHistory` alone"
  reads as though it cannot: `apply()` bodies read the root through
  `rootContainer(project)`, so setting it around apply/undo/redo genuinely reaches
  all ~99 of them.
- **The cross-root merge guard is an AC, not a "not gated" line.**
  `SActionUndoCommand::id()`/`mergeWith` (`sactionundocommand.cpp:47-72`) compares
  only `trackPath_`/`slotIndex_`/`paramId_`, and `mergeKey` is a path relative to
  an unidentified root (`ssettrackvolumeaction.cpp:68-74`). Two faders at `{0}` in
  two roots produce **identical keys**; the surviving command carries one captured
  root, so one undo restores the wrong root's fader. Refuse a merge across
  captured roots. Note the queue-level merge (`sactionqueue.cpp:8-16`) is **dead
  code** — the synchronous drain empties the queue before every enqueue, whatever
  its comment claims — so the guard must go on `SActionUndoCommand::mergeWith`.
- **Correct the macro model:** real macros here are `QUndoStack::beginMacro`
  spans over loops of independent `submit`s (`saudiorecorder.cpp:503-516`,
  `smidirecorder.cpp:450-481`, `ssmvmixercontrol.cpp:408-432`), where each member
  captures its own root and is **already correct**. `SCompositeAction` is the case
  that needs the capture at its boundary.

### ACs

| # | Acceptance criterion | Checked by |
|---|---|---|
| 4.1 | An edit in A, undone while the master is the edit root, undoes **in A** | `arrangement_undo_scope` |
| 4.2 | The master is not modified by that undo | same |
| 4.3 | Redo lands in A | same |
| 4.4 | A `SCompositeAction` undoes entirely in the arrangement it was applied in | same |
| 4.5 | Two volume changes on same-index tracks in different roots **do not merge** | `arrangement_merge_guard` |

### Gates

- **New:** `arrangement_undo_scope.qxa` — master gets 2 tracks;
  `create-arrangement`; `use-arrangement name="Drums"`; `add-track`;
  `assert-track-count arrangement="Drums" count="1"`; `use-arrangement name=""`;
  `undo`; **`assert-track-count arrangement="" count="2"`** — the assertion that
  fails on the pre-fix binary, where undo removes a *master* track and reports 1;
  then `assert-track-count arrangement="Drums" count="0"`; `redo`; back to 1.
  This is the one gate in the plan whose job is to catch silent corruption; it is
  written entirely in the M1 verb and **could not have been written at all** in
  the old `<assertions>` spelling.
- **New:** `arrangement_merge_guard.qxa` — `set-track-volume` on `{0}` of the
  master and `{0}` of Drums, then one `undo`, asserting the *other* fader did not
  move.

### Not gated

Asynchronous drain (it is synchronous today); the leaks M2 recorded
(`applyChildTrackAudibility`, non-`submit` `apply()` callers) — named, not fixed.

---

## M5 — One cycle guard, over the whole graph

**Build (D11):** `main/model/include/app/model/sarrangements.h` —
`bool reaches(SObject *from, SObject *to)`, a memoised DFS over `childLinks()`
**and** window content, so "A places an asset of B, B places an asset of A" is
visible. `SPlaceAssetAction` calls it *in addition to* its existing guards (the
whole-mixer refusal is still correct for an in-place asset). M6 uses the same call.

### ACs

| # | AC | Checked by |
|---|---|---|
| 5.1 | A two-hop cross-arrangement cycle is refused | `arrangement_cycle_refused` |
| 5.2 | A three-hop cycle A→B→C→A is refused | same |
| 5.3 | The 5 existing `asset_*` cases (and the 8 using `create-asset`) pass **unmodified** | full suite |
| 5.4 | A refusal is a rejected action with a log line — never a crash or a hang | `assert-log` |

**If an existing asset case needs editing, the guard is over-broad. STOP.**

---

## M6 — `extract-arrangement`

```xml
<extract-arrangement trackPaths="0;2,1" rangeStart="0" rangeEnd="192000"
                     name="Drums" window="range" placeAt="0"/>
```

`trackPaths` is **semicolon-separated** (`pathToString` uses commas *within* a
path). `window` ∈ `range | extent`. `placeAt` defaults to `rangeStart`.

**Build (D6, D7, D15):** validate (≥1 path, all `STrack`, none an ancestor of
another, `rangeEnd > rangeStart`, `reaches` clean, name free) → create and
register the root (M1) → **move each track whole**, preserving order and nesting,
reusing `SReparentTrackAction`'s discipline **verbatim** including the `addRef()`
pin across the move (`removeRef()` → `deleteLater()` is irreversible, so the count
must never touch zero) and the `QObject::disconnect(track, nullptr, oldParent,
nullptr)` that `removeTrack(SLink&)` does not do → register the asset `SCut` →
insert **one** master lane at the topmost extracted track's index and place the
asset at `placeAt` → `applyAudibility()` + `notifyTreeChanged()` on **both** roots
+ `rewireSpeaker()` → **report** (D7, D15): the frames outside the window, **and
the fact that the project's render length now follows the asset window**
(§11.12 — `getDurationFrames()` measures the master, `SRenderAction` defaults to
it and *"narrows a render, it never widens one"*).

**`window="extent"` needs a size warning of its own**: `buildCapture_` renders
the container into an in-memory `channels × frames` float buffer, **synchronously
on the UI thread** (`scut.cpp:425-460`, `:175-177`, `:1110`), and its own "tens of
milliseconds" estimate (`:319-322`) is orders of magnitude out for a whole
arrangement.

### ACs

| # | AC | Checked by |
|---|---|---|
| 6.1 | Extracting N tracks over `[t0,t1)` placed at `t0` leaves the audio in that window **byte-identical** | `extract_arrangement_audio` — see the gate, this is a **byte** gate |
| 6.2 | The tracks left the master sum: placed elsewhere, the original window is **silent** | `extract_arrangement_detached` |
| 6.3 | The master has N−1 fewer lanes; the new lane sits where the topmost extracted track was | `assert-track-count` + `assert-track-name` |
| 6.4 | The arrangement holds the N tracks in order, with clips, volumes and plugin chains | `use-arrangement` + `assert-track-count` / `-volume` / `assert-plugin-strip` |
| 6.5 | **One** undo restores the project exactly | `extract_arrangement_undo` (`assert-file-identical` vs the pre-extraction render) |
| 6.6 | Redo re-extracts identically | same |
| 6.7 | Extract → save → load → render is identical to extract → render | `extract_arrangement_roundtrip` |
| 6.8 | The asset places a second time, and an edit inside is heard at **both** | `extract_arrangement_reuse` (**needs M0**) |
| 6.9 | Material outside the window warns, naming the frame count **and the new render length** | `assert-log` |
| 6.10 | Refusals: overlapping paths, taken name, empty range, cycle | `extract_arrangement_refused` |
| 6.11 | A track holding a live recording is **refused** (`SRecordingContent` has no loader registration) | same |

### Gates

- **New:** `extract_arrangement_audio.qxa` — **a byte gate, via a
  structurally-muted twin.** An earlier draft used a ±2 % RMS band on the grounds
  that extraction adds an `SCut` capture to the path. True, but a compare between
  *two paths that both go through that capture* is exact: group the N tracks into
  a folder, `create-asset` over the folder, structurally mute it, `place-asset`,
  and `assert-file-identical` against the extraction. Both run identical DSP. The
  precedent is `tests/cases/automation_clip_gain.qxa:119-140`. A ±2 % band cannot
  see a windowing off-by-one, a channel-map slip or a placement offset error; this
  can.
- **New:** `extract_arrangement_detached.qxa` — `placeAt="384000"` with
  `render durationSec="12"`; **silence in `[0,192000)`**, the ladder in
  `[384000,576000)`. This proves D8 and is what fails if the new root is left
  summed into the master. **Pass `durationSec` explicitly** — §11.12.
- **New:** `extract_arrangement_undo.qxa`, `_roundtrip.qxa`, `_reuse.qxa`,
  `_refused.qxa`.
- `action_roundtrip_test` row.

### Not gated

Extraction of a **folder** track with children (same whole-track move — write the
case if cheap, else say untested); a track with an event clip and an instrument —
**and note M2's `beginRun()` finding**: the instrument barrier walks the master
root, so proposal 37 D4's render-determinism barrier is absent for arrangement
instrument tracks until that is fixed; channel widths other than the project's.

---

## M7 — `dissolve-arrangement`, and the `{}` hazard behind it

Split from M6 because it is where two real defects live.

**Build (D17, D18):**

- `dissolve-arrangement name=` — remove the placement(s), unregister the asset,
  move the tracks back to their recorded parents and indices, unregister and
  delete the empty root. A first-class user verb ("bring these tracks back into
  the song"), not a private undo helper.
- **D17, and this one is a live bug independent of tabs:** `SRemoveAssetAction`
  computes `pathOf(project->getRootComponent(), &cut->getContent())`
  (`sremoveassetaction.cpp:33-39`). For a detached root that returns `{}`, and
  `{}` means **the root itself** — so undoing the removal of an arrangement asset
  registers an `SCut` **windowing the entire master**. Address the content by
  arrangement **name** when it is a registered root, and **refuse** rather than
  fall back to `{}`. `dissolve` routes through this. This file appears in no
  other milestone.
- **D18:** `extract` and `dissolve` are only mutual inverses at **one** placement
  — `extract` creates one. Either refuse `dissolve` above one placement (matching
  `remove-arrangement`, which does refuse) or make its inverse a composite that
  restores every placement. Pick one and say which; leaving both claims standing
  is a contradiction inside one milestone.

### ACs

| # | AC | Checked by |
|---|---|---|
| 7.1 | `dissolve` does what undo does, and is itself undoable | `extract_arrangement_dissolve` |
| 7.2 | Undoing a `remove-asset` on an **arrangement** asset restores that asset — not one over the master | `asset_remove_undo_arrangement` |
| 7.3 | The chosen D18 answer holds at two placements | `extract_arrangement_dissolve` |

### Gates

**New:** `extract_arrangement_dissolve.qxa`; **new:**
`asset_remove_undo_arrangement.qxa` — assert the restored asset's window and
content, which on the pre-fix binary is the whole master.

---

## M8 — The tab shell, and arrangement tabs (**M7+M8 of the old plan, merged**)

**Merged deliberately.** Old M7 shipped arrangement tabs and old M8 shipped "tab
activation sets the edit root". Between them an arrangement tab is on screen and
interactive while the edit root is still the master: a click mints a path against
`model_` and submits it against the master (`sstdmixerview.cpp:548`, `:629`/`:650`,
`:689`/`:706`, `:4447`), index `{0}` exists in both trees, the action **succeeds**,
and the wrong arrangement is edited — §11.7's silent corruption, shipped for the
duration of one PR, with the extraction gesture putting the user in that very tab.
M2's D16 fix makes it impossible; merging removes the window regardless.

**Build:**

- `main/shell/`: `SViewTabs`, a thin `QTabWidget` owning `(root, editor)` pairs.
  Master tab index 0, **not closeable**. All three construction sites
  (`smainwindow.cpp:424`, `:499`, `:650`) and both teardown sites build/tear the
  shell; `ensureArranger_()` (`:2149`) returns the **active** tab's view, and the
  other casts (`:234`, `:2979`, `:2985`) go through it. Note `:143` is a
  `qobject_cast<SStdMixer*>` on the project root, **not** a view cast — leave it.
- Open / focus / close, **dedup keyed on the root `SObject*`**, **auto-close** on
  destruction or `gotUnreferenced` — wired from the first commit, per §8.
- **On activation: `setEditRoot(tab's root)`** and the tab publishes itself as the
  selection context.
- Verbs `open-arrangement-tab name=` / `close-arrangement-tab name=`,
  `assert-tab-set names="master,Drums" active="Drums"`.
- `extract-arrangement` opens and focuses the new tab.
- **The gesture:** next to "Create &asset from range", a new **"Extract to new
  arrangement…"**, enabled only with a range, acting on
  `pruneNestedTargets(selectionTargets(lastClickTrack_))` — the norm at
  `sstdmixerview.cpp:1239`, `:1276`, `:1295`, `:1345`, `:3413`, of which
  `ctCreateAssetFromTrack` (`:2110`) is the single outlier. A dialog with the
  name, a *"whole extent instead of the range"* checkbox **carrying a size
  warning** (M6), and a summary stating how many tracks move, how much material
  falls outside, **and that the project's render length will follow the window**.
  Also say the §11.4 sentence: if you will not place this more than once, a folder
  track is simpler — and, per §11.14, cheaper.
- Fix `ctCreateAssetFromTrack` while there: either act on the selection or relabel
  it "from this track". Pick one, say which, gate the choice.
- **Consider the fourth option (§11.14):** an optional *"place the outside
  material too"* — head `[0,t0)` and tail `[t1,end)` windows of the same asset on
  the same lane. Two extra `place-asset` calls, nothing lost in audio, no track
  splitting. Cheap enough that leaving it out should be a decision, not an
  oversight.
- Also: `SSelectionManager` resolves against the project root
  (`sselectionmanager.cpp:10`, `:26`, `:62`) and `svirtualkeyboarddock.cpp:67`/`:78`
  read `getSelectedTrack()` off it — both must become root-aware or AC 8.6 cannot
  be expressed.

### ACs

| # | AC | Checked by |
|---|---|---|
| 8.1 | One tab, and the app behaves exactly as before | the whole suite, unchanged |
| 8.2 | New / open / close / swap build and tear the shell with no dangling editor | `tabs_master_only` |
| 8.3 | Opening an arrangement twice focuses one tab | `tabs_open_dedup` |
| 8.4 | `extract-arrangement` leaves the new tab open and active | same |
| 8.5 | Undo of the extraction, and `dissolve`, **close** the tab | `tabs_autoclose_on_undo` |
| 8.6 | A clip selected in A is not deleted by a delete driven from the master tab | `tabs_selection_scoped` |
| 8.7 | Switching tabs switches the edit root; a later `add-track` lands in the active arrangement | same |
| 8.8 | Switching tabs does **not** stop or move the transport | same (`assert-locator`) |
| 8.9 | Closing a tab does not touch the model | `assert-arrangements` after close |

### Gates

**New:** `tabs_master_only.qxa`, `tabs_open_dedup.qxa`,
`tabs_autoclose_on_undo.qxa`, `tabs_selection_scoped.qxa`. The **existing suite
green with the count reconciled** is the real gate for 8.1.

### Not gated

Tab-bar pixels and ergonomics; the docked/floating window-state round trip (Qt's
opaque `ui/windowState` blob — manual, as for the media dock); the context menu
and its dialog — **there is no testkit verb for a context menu anywhere in this
repo and none is invented here**; what is gated is the action the menu submits.

---

## M9 — `<render>` follows the edit root

Split from the audition swap: a different edit, in a different file, and the cheap
half. **Write this before M10.**

**Build:** `SRenderAction` (`srenderaction.cpp:67-69`) takes
`project->getRootComponent()->getRootComponent()` — **neither** of D4's two roots.
Point it at the edit root, and add `main/actions/**` to `check_editroot.py`'s
scope (M3). Every existing case leaves the edit root at the master, so all 125
`<render>` sites are unchanged in meaning; a sub-arrangement case must pass
`durationSec` explicitly, because `getDurationFrames()` measures the **master**
root (§11.12).

### ACs / Gates

`arrangement_render.qxa` — after `use-arrangement`, the render is the
arrangement's own audio from its own zero, per-second RMS against the ladder; the
master render afterwards is unchanged. Full gate set.

---

## M10 — Local audition: the playing root

**Name three roots, not two** (D4 + D9): **master**, **edit**, **playing**.

**Build (D9, D19):**

- `SApplication::rootComponent()` returns the **playing** arrangement's root. The
  playing tab is distinct from the active tab — §4's whole point.
- **This is cheaper than §11.6 feared** and the reason is worth knowing:
  `twSpeaker::startOutput()` re-reads `context_->rootComponent()` and **mints a
  fresh `AudioEngine` on every start** (`twspeaker.cc:114-132`), with
  `releaseEngine()` joining the readahead thread first; and the master meter
  re-taps every call (`sapplication.cpp:452-461`). So the swap is a
  transport-boundary re-read, not a re-plumbing.
- Refuse the swap during a render, while recording, and while a live lane is up —
  **except a metronome-only closure (D19)**. `metronomeWanted`
  (`sliveplanbuilder.cpp:35-51`) raises a live lane whenever the click is on and
  the transport rolls, and `:212-214` explicitly permits a metronome-only one; the
  metronome switch is a **project property that travels with the file**. So a
  blanket refusal bans audition for every project saved with the click on — and a
  gate written against test projects would not notice, because they default it
  off. **Write the gate with the click ON.**
- Arming a track in a sub-arrangement **refuses with a message** (D10).
- Read `SLiveMonitor::transportAboutToChange` first: proposal 21's ordering traps
  (release ownership *before* the re-wire; rebuild *before* `startOutput()`) are
  the shape of the hazards here.

### ACs

| # | AC | Checked by |
|---|---|---|
| 10.1 | Playing an arrangement tab is audible through the capture backend, from its own zero | `arrangement_audition` |
| 10.2 | Playing the master afterwards is audible and unchanged | same |
| 10.3 | The swap is refused while armed / recording / rendering, and playback continues on the previous root | `arrangement_audition_refused` |
| 10.4 | The swap **succeeds** with the metronome on | `arrangement_audition` with the click ON |
| 10.5 | Arming a sub-arrangement track is refused with a message | `arrangement_audition_refused` |
| 10.6 | Exactly one playhead exists at any time | `assert-locator` in both |

### Gates

`arrangement_audition.qxa` and `arrangement_audition_refused.qxa`, both
`RUN_SERIAL` at `SMARAGD_CAPTURE_SPEED=1`, in the shape of `metronome_click` /
`monitor_*` — **copy one of those CTest entries; do not invent the environment.**
Flake-check with `ctest -R "^qxa.arrangement_audition$"` ×20; note
`repeat_test.sh` cannot drive live-shaped cases (it does not set their
environment and judges by stdout, not exit code).

### Not gated

Real device latency across a swap; the swap under load; the **first-audition
latency spike** — `~AudioEngine` joins only the readahead thread and never
cancels `pendingDemand_` (`audio_engine.cc:63-70`, and `:757-761`: *"The stale
handle is simply dropped — its nodes finish and publish pages"*), so the outgoing
root keeps freezing at priority 9 ahead of the incoming one.
`CaptureRevalidator::retireComponents` exists for this and is not called on the
Play/Stop path. Measured, not bounded — say so.

---

## M11 — Persist open tabs, and surface arrangements

**Build:** open tabs persist **by arrangement name** in `SProject`'s property
store — **not** by object id (they are pointers, `sobject.cpp:133`) — plus the
active tab's name; a name that no longer resolves degrades to "that tab does not
reopen", never a load failure. The resources dock (`SExternFileList`) lists
arrangements beside assets and samples and opens the tab on double-click,
following the existing `asset:<name>` drag-payload shape. Documentation sweep:
`CLAUDE.md` gets a section in the house style (what to know / why, the traps, the
gates, an explicit NOT-gated list); `docs/ARCHITECTURE.md` the new files;
`plan/proposed/09_MULTI_VIEW_TABS.md` an execution-status header; `plan/STATE.md`
the chronological entry.

### ACs / Gates

`tabs_persist_roundtrip.qxa` — open tabs and the active tab survive save → load;
a hand-edited stale name is dropped and the project still loads. The dock's
double-click is hand-verified (no verb for a tree-widget activation).

---

## Risk register

| Risk | Where it bites | Mitigation |
|---|---|---|
| **An edit in an arrangement is inaudible** | M0 — verified, not hypothetical | M0 is blocking; its gate cannot pass before the fix |
| **A gate that cannot express its AC** | M1 — `assert-track-count` is end-of-script, `equals=`, master-rooted | M1 builds the verb before anything needs it |
| **An index-path resolved in the wrong tree** | M4 (undo), M8 (the view) | M4's capture + M2's D16 view fix; `arrangement_undo_scope` fails loudly pre-fix |
| **Redefining `rootContainer` re-points 99 sites unreviewed** | M2; it reaches the recorder, the MIDI-out pump and the growing-clip placement | the pinned exemption list; `check_editroot.py` polices `rootContainer`, not `getRootComponent` |
| **`remove-asset`'s undo makes an asset over the whole master** | M7; a live bug independent of tabs | D17 — address by name, refuse rather than fall back to `{}` |
| **A detached root garbage-collected on load** | M1; it vanishes on the first save/load, via `deleteLater()` so it looks like nothing | the registry pin from `deferResolve`; AC 1.2 |
| **The new root accidentally summed into the master** | M1/M6; the song is heard twice | `arrangement_not_summed` (byte) and `extract_arrangement_detached` (silence) |
| **`removeRef()` reaching zero mid-move** | M6; `deleteLater()` is irreversible | copy `SReparentTrackAction`'s pin verbatim |
| **A container capture publishing silence, permanently** | M0 D14; the normal path after extraction | refuse the publish; AC 0.7 |
| **The metronome banning audition for every project with the click on** | M10 | D19; gate with the click ON |
| **`window="extent"` rendering an arrangement synchronously on the UI thread** | M6/M8 | a size warning in the dialog; the folder-track alternative is cheaper (§11.14) |
| **A dangling editor after its root dies** | M8 | auto-close wired from the first commit |
| **Scope creep into folder-track tabs** | any | D2 defers them; the 113 `model_` lines are their problem, not this plan's |

## Explicitly out of scope

Folder-track tabs and the `SStdMixerView` generalization (§1 route A, deferred by
D2); non-arrangement editors as tabs; splitting clips at the range boundary
(§11.3 option 1, rejected); recording, monitoring and MIDI-out **inside** a
sub-arrangement (D10); a per-arrangement undo stack (one stack, root-scoped
actions — D5); a root-qualified `SObjectPath` migration (D16 records it as the
target state and defers the ~99-site change); nested arrangements deeper than
three hops, which are permitted but only *gated* to three (M5 AC 5.2).
