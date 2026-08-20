# Concept: Multi-View Tabs (multiple arranger views on one project)

Draft. Structure and placement first; the UX polish and the asset-authoring flow
are deferred. Answers the two questions that decide everything else: **what is a
tab rooted at**, and **how do many views stay consistent on one model**.

## TL;DR

- A **tab is a detail-edit view rooted at one `SObject`** of the current project —
  the master mixer, a group track, or (later) a named "live asset" sub-arrangement.
  Like sheets in one workbook: different windows onto the *same* data, not copies.
- The model already makes this nearly free. `SObject`s are **reference-counted and
  shared via `SLink`**, and the render path is a **live pull every buffer**, so
  "edit once, reflect everywhere" is automatic. The arranger (`SStdMixerView`)
  already walks a **generic `SObject` container tree** (`appendRowsFor(SObject*,
  depth)`); it is only *typed* to `SStdMixer*` at its boundary.
- Two real changes are required, and only two:
  1. **Generalize the arranger to root at any container `SObject`**, not just
     `SStdMixer` (§1). `STrack::getDetailEditWidget` returns NULL today — this is
     what makes "drill into a group/asset in its own tab" possible.
  2. **Replace the single central widget with a `QTabWidget`** that hosts one
     editor per open root, with open/focus/close + dedup-by-identity (§2).
- Everything else is consequence: **selection becomes tab-scoped** (§3), exactly
  **one tab plays at a time** so there is a single playhead (§4 — *decided*), and
  **which tabs are open is persisted** in the project via the existing object
  registry + property store (§5).
- **Locked decisions** (this revision): one playhead, owned by the single
  *playing* tab; a tab being *drawn from* by another is **not** playing (§4). The
  head of an **arrangement** tab is **always `SStdMixerView`** (§1). The reference
  graph is **acyclic** — tabs cannot form cyclic dependencies (§6).
- This is the natural home for the **recursive / cross-referenced clip** work:
  a tab editing a referenced sub-arrangement updates every instance of it. It
  **depends on proposal 05** (live region assets + intrinsic track processing)
  for non-master tabs to carry meaningful, self-contained output — but the tab
  *shell* (master + group-track tabs) can land before 05.

---

## 0. What the model already gives us

The "multiple sheets, one workbook" semantics are not something we build — they
already hold, because the object graph was designed reference-first:

- **Shared, reference-counted objects.** An `SObject` is referenced by zero or
  more `SLink`s (`addRef`/`removeRef`, `getNReferences`, `gotUnreferenced`). The
  same drum-loop object can be linked into ten places; there is exactly **one**
  object behind them. A tab editing that object edits the one truth.
- **Live pull, not snapshots.** `twTrackMix::calcOutputTo` pulls each child's
  `getRootComponent().calcOutputTo()` every buffer and does not care whether the
  child is a clip, a file, or another track. So a change made in one view is
  heard and seen everywhere on the next repaint/buffer — no re-render, no sync.
- **Views already observe the model by signal.** `childObjectAdded` /
  `childObjectRemoved` / `durationChanged` / the property signals drive the
  arranger today. A second view on the same subtree subscribes to the same
  signals and stays consistent **for free** — this is the whole reason tabs are
  cheap here.
- **The arranger is almost root-agnostic already.** `SStdMixerView` builds its
  lane list by `appendRowsFor( SObject *container, int depth )`, recursing into
  child containers. The only thing tying it to the master is the constructor
  (`SStdMixerView( parent, SStdMixer* )`) and `getModel()`'s return type.
- **Stable object identity exists in the wire format.** The loader resolves
  shared objects through a name-keyed `sObjectRegistry_`, i.e. every persisted
  object already has a stable id used to round-trip references. That id is the
  natural key for "which objects have a tab open".

What is *missing* is purely presentational: a way to put more than one of these
views on screen, and a way to root a view somewhere other than the master.

---

## 1. Change #1 — root the arranger at any container `SObject`

**Today:** `SStdMixer::getDetailEditWidget` returns `new SStdMixerView(parent,
this)`; `STrack::getDetailEditWidget` returns `NULL`. Only the master can be
arranged.

**Change:** decouple the arranger from `SStdMixer`. Two equivalent routes:

- **(A) Generalize in place.** Change `SStdMixerView` to take an `SObject &root`
  (any container) instead of `SStdMixer *`. `rebuildRows()` already starts from a
  container and recurses; it just needs its starting container parameterized. The
  master case becomes "root == the mixer". `getModel()` returns `SObject*`.
- **(B) Extract `SArrangementView`.** Lift the generic tree-walking arranger into
  a base that roots at `SObject&`; leave `SStdMixerView` as a thin master-specific
  subclass if any master-only chrome (the global ruler, master controls) needs to
  stay master-only.

**Decided: route (A).** Smaller diff, the view is already generic internally, and
— per the locked decision — **the head of any *arrangement* tab is always
`SStdMixerView`**. There is one canonical arrangement editor; rooting it at the
master, a group track, or a live asset is the *only* difference between those
tabs. (Split out a base only if master-only chrome forces it — see §8.) So:

- `STrack::getDetailEditWidget` returns an `SStdMixerView` rooted at that track,
  instead of NULL — a group track opens *its own* arranger showing its child
  lanes. The master tab is the same view rooted at the mixer.
- **Non-arrangement SObjects are the future extension, not the first cut.** Because
  every `SObject` already vends `getDetailEditWidget`, the tab shell (§2) only
  needs an `SObject*` and a `QWidget*` — it does not care which editor it got. So
  later a different *kind* of source — a **Tracker**, or another audio source with
  its own full-screen editor — can become a tab by simply returning its own widget
  from `getDetailEditWidget`. Arrangement tabs are `SStdMixerView`; these others
  bring their own head. No shell change needed when that day comes.

**Caveat:** a *group track* only makes sense as a self-contained tab once its own
gain/mute/solo are intrinsic to its output (proposal 05 §0). Before that refactor,
a group-track tab still *displays and edits* correctly (lanes, clips, timing) —
its **standalone audition** is just pre-gain. So the shell can ship first; full
fidelity of non-master tabs tracks proposal 05.

---

## 2. Change #2 — the tab shell in the main window

**Today:** `SMainWindow` holds one `projectRootWidget_` set via
`setCentralWidget`, rebuilt on new/open/close.

**Change:** the central widget becomes a `QTabWidget` (a small `SViewTabs`
wrapper). Each tab owns one `(SObject *root, QWidget *editor)` pair.

| Concern | Behaviour |
|---|---|
| **Master tab** | Always present, **not closeable**, always index 0 — the root component's arranger. Replaces today's single central widget 1:1, so the no-extra-tabs experience is byte-for-byte the current UX. |
| **Opening a tab** | Double-click a group track / asset clip → "Edit in new tab"; right-click → "Open in new tab"; (optional) a tab-bar **+** or a project browser. The editor is just `root->getDetailEditWidget(tabs)`. |
| **Dedup by identity** | Opening an object that already has a tab **focuses the existing tab** — keyed by the `SObject*` (one object ⇒ at most one tab). This is what keeps "the same loop in two places" from spawning two fighting editors. |
| **Closing** | Secondary tabs are closeable. Closing destroys only the *view*; the model object is untouched (it is still referenced elsewhere). |
| **Object deleted while open** | A tab listens to its root's destruction / `gotUnreferenced`; if the underlying object goes away (deleted, undo of its creation), the tab **auto-closes**. This is the one lifetime hazard and must be wired from the start. |
| **Project close / swap** | All tabs torn down with the project, exactly as `projectRootWidget_` is today. |

`SMainWindow`'s existing reach-throughs (`groupTrack()` etc. that
`dynamic_cast<SStdMixerView*>(projectRootWidget_)`) re-point at "the **active**
tab's editor". The just-added **status-bar mode indicator** needs no change —
only one tab is active at a time, so the active editor's hover mode is the one
shown.

---

## 3. Selection becomes tab-scoped

**Today:** `SApplication` holds a single global selection
(`getCurrentSelectedSLink`, `getSelectionList`). With several arrangers visible,
one global selection is wrong: selecting a clip in the drums tab must not
highlight (or get deleted by) an action driven from the vocals tab.

**Change:** selection is owned **per view**, and the **active tab's selection is
the app-wide one** the transport/menus act on. Concretely: each editor keeps its
own selection; on tab activation it publishes itself as the current selection
context. Undo/redo and clip actions already target objects by path, so they keep
working — they just read the active tab's selection.

This is the largest *semantic* ripple and should be its own phase. Until it
lands, multi-tab is read-mostly-safe but edits should be confined to the master
(or selection explicitly cleared on tab switch).

---

## 4. Playhead: exactly one tab plays at a time (decided)

The mental model is locked, and it is simpler than the earlier fork:

> **There is one playhead, and it belongs to the single *playing* tab.** A tab
> being *drawn from* by the playing tab is **not** itself playing — it is just its
> mixer being pulled as a node in the playing tab's render graph.

So when the master tab plays the whole song, the drum-loop and vocal tabs you
also have open are **not playing**: their arrangements are *referenced* by the
master and pulled into its output, but they carry no playhead and no transport
state. "Playing" is a property of **one** tab; "being heard" is a property of the
render graph, and the two are independent.

Consequences, all of which fit the existing engine:

- **The transport binds to one tab — the playing tab.** Pressing Play makes the
  current tab the playing tab: its root component becomes what the speaker pulls,
  and its playhead runs from its own zero. Starting play on another tab **moves**
  the playhead there and stops the previous one (only one playhead ever exists).
- **This is literally `rewireSpeaker` repointing.** `SApplication::rewireSpeaker`
  already points the speaker at a chosen component's output root. "Play this tab"
  = rewire the speaker to the playing tab's root, then run transport. Master
  playing → speaker = master root (the full song). Drum-loop tab playing → speaker
  = that loop's root, auditioned standalone. No new engine concept; the playhead
  is single and global, just *attributed* to whichever tab owns it now.
- **Drawn-from tabs need no playhead drawing.** Because they are not playing, there
  is no ambiguous "N ghost playheads where the asset is placed N times" problem to
  solve — that question simply does not arise under this model.
- **Switching tabs ≠ stopping.** Looking at the drum tab while the master plays
  leaves the master as the playing tab; the drum tab is just the *viewed* tab. The
  playing tab and the active/viewed tab are distinct, and only the playing one
  shows a running playhead.

---

## 5. Persisting which tabs are open

The set of open tabs is per-project workspace state, like open sheets in a
workbook. It rides existing machinery — **no new serialization format**:

- Persist a list of **object ids** (the loader's registry names) for the open
  secondary tabs, plus the active tab index, in `SProject`'s **property store**
  (`setProp`/`prop`, already JSON-serialized). The master tab is implicit.
- On load, after the object graph is rebuilt and the registry is populated,
  resolve each saved id back to its `SObject` and reopen its tab; drop ids that
  no longer resolve (object was deleted in a prior session).
- This needs objects addressable by their stable id at runtime, which the loader
  already establishes during read — expose that mapping (or a small "find object
  by id" on the project) so it is available post-load, not just mid-parse.

Open tabs are *workspace*, not *content*: saving/reopening them is a convenience,
and a missing/renamed id degrades to "that tab just doesn't reopen" — never a load
failure.

---

## 6. Relationship to recursive / cross-referenced clips (why this matters now)

The motivating feature — "work on this drum loop, that vocal phrase, the uncut
drums arrangement, each in its own tab" — *is* the visible half of the recursive
clip model:

- A **live asset** (proposal 05 §(b)) is a sub-arrangement `SObject` referenced by
  `SLink`/`SCut` wherever it is used. **Editing it in its own tab edits every
  instance** — the cross-reference payoff, with zero extra wiring because of the
  live-pull model (§0).
- "Recursive" = an asset whose arrangement itself places other assets. The tab
  shell handles this with no special case: drilling into a placed asset just
  opens *its* root in another tab (dedup keeps a single view per object).
- **The reference graph is acyclic — a defined constraint, not an option.** An
  asset may not (transitively) place itself; doing so would be an infinite render
  (the live pull would recurse forever) as much as an infinite view. So the
  *placement* operation — `SLink`-ing one arrangement into another — must **reject
  any link that would close a cycle** (a reachability check from the target back
  to the source before linking). This belongs in the model, not the tab layer: it
  protects the engine first and the UI for free. With cycles impossible, "drill
  into a placed asset" can never root a tab inside its own ancestor, so the tab
  shell needs no separate guard.
- So this proposal is the **UI surface** for 05's asset model. Build order: the
  tab shell (master + group-track tabs, §1–§2) is independent and can come first;
  **asset tabs** (a named region promoted to a shareable object, edited in a tab)
  land **after** 05's asset object + intrinsic-processing refactor.

---

## 7. Suggested phasing

1. **Generalize the arranger to root at any `SObject` container** (§1 route A).
   Master tab still the only view; **zero UX change**. Proves the view is truly
   root-agnostic. *(No dep.)*
2. **Tab shell with a single master tab** (§2). `QTabWidget` replaces the single
   central widget; still one tab, identical experience. Pure scaffolding. *(No
   dep.)*
3. **Open group tracks in new tabs** — double-click / context menu, dedup by
   identity, auto-close on delete (§2). First user-visible multi-tab. *(No dep;
   standalone-audio fidelity tracks 05 §0.)*
4. **Tab-scoped selection** (§3). Active tab owns the app selection; undo/redo and
   clip actions follow it. *(Depends on 3.)*
5. **Local audition transport** for sub-tabs (§4, P1). *(Depends on 3.)*
6. **Persist open tabs** across save/load (§5). *(Depends on a runtime find-by-id;
   small loader exposure.)*
7. **Asset tabs** — promote a region to a named live asset and edit it in its own
   tab; the recursive/cross-reference payoff (§6). *(Depends on proposal 05.)*

Phases 1–3 are the structural commitment and deliver the requested "sheets"
experience for group tracks. Phases 4–6 make it edit-safe and persistent. Phase 7
is where it meets the recursive-clip roadmap.

---

## 8. Risks & open questions

- **Selection globalness (§3)** is the sharpest ripple — several call sites read
  `SApplication`'s single selection directly. Audit them before phase 4; the fix
  is "active tab publishes selection", not "every site learns about tabs".
- **Lifetime (§2, deleted-while-open)** must be wired from phase 3 or a closed/
  undone object leaves a dangling editor. Lean on `gotUnreferenced` /
  QObject-destroyed.
- **Master-only chrome (§1)** — if the global ruler / master strip cannot live in
  an `SStdMixerView` rooted at a non-master container, that forces extracting a
  base out of `SStdMixerView`. Discover this in phase 1, not later — it is the one
  thing that could complicate the otherwise-decided route (A).
- **Acyclic enforcement (§6)** is now a model invariant — the reachability check
  must be in place *before* asset placement ships (phase 7), and ideally exercised
  by the same code path group-track nesting already uses, so there is one cycle
  guard, not two.

*Resolved this revision:* the playhead model (§4, one playing tab) and cycle
policy (§6, acyclic) are decided, and the arrangement-tab head is fixed to
`SStdMixerView` (§1) — they are no longer open questions.

---

# Revision 3 (2026-08-20) — arrangement extraction is the driving use case

Revisions 1–2 above answered "what is a tab rooted at" in the abstract. This
revision has a **concrete driving use case**, and molding it in changes three of
the decided answers. It is written as: the use case verbatim (§9), the facts as
they actually are in the tree today (§10), what is wrong or under-specified about
the use case (§11), and the decisions that result (§12). The milestone plan that
executes it is `plan/todo/09_MULTI_VIEW_TABS_EXECUTION.md`.

## 9. The use case

> I want to create a new asset ("clip") from a selected time range on a selected
> number of tracks. I can do that today using "Create asset from…". I would like
> as part of that operation that the mentioned tracks are transferred into a Tab
> of their own with their arrangement, so that we neither technically nor UI-wise
> confuse the concept by having objects with logically different roots displayed
> in the same arrangement.
>
> For the time being I suggest these other tabs, when editing in them, output to
> a root of their own as opposed to the sink of the tracks in their former
> arrangement.

Read structurally, that is three requests: (i) the asset-authoring gesture is
multi-track and range-scoped; (ii) it is **destructive to the master** — the
tracks leave it; (iii) the extracted arrangement has **its own root**, i.e. it is
not summed into the master and is heard only where an asset of it is placed.

(iii) is the interesting one. It is a stronger statement than §1–§6 ever made,
and — see §11.5 — it is the *safer* model, not merely a simpler one.

## 10. What is actually in the tree today (verified 2026-08-20)

Because §0's "the model already gives us this" is true in outline and wrong in
several load-bearing particulars.

| Claim | Reality |
|---|---|
| "Create asset from range" is the existing gesture | `SMVActualView::ctCreateAssetFromTrack` acts on **`lastClickTrack_` alone** — one track, never the multi-track selection, although `SStdMixerView::selectionTargets()` is right there and every other track operation uses it. |
| An asset is non-destructive | `SCreateAssetAction` registers an `SCut` **windowing a container in place**. Nothing moves; the container keeps summing into the master; the placement is a **second** audible copy. The user's operation is a different operation, not a parameter of this one. |
| Assets round-trip | **They do not.** `SProject::registerAsset()` has exactly **one** call site — `screateassetaction.cpp:66` — and **nothing in `main/persistence/` touches the registry**. It is runtime-only; 05 §2.9 named the additive format change and it was never made. (`splaceassetaction.cpp:67-72` says so explicitly: *"No pin is taken here. registerAsset() already holds one reference for the registry."*) |
| The wire format admits only one root | **True on write, false on read.** `SProject::serialize()` (`sproject.cpp:45-66`) dumps every child — but every `SObject` is a QObject child of the project (`sobject.cpp:912`), so it is a *flat* dump, not a list of arrangement-level roots. The read side admits exactly one: `sprojectloader.cpp:64` is a single `QString rootId`, consumed once at `:254-268`, and a missing root is the one fatal load error. So a second `<SStdMixer>` **is emitted** with no format change, and is then **garbage-collected on load** unless a registry pin is re-established. |
| Object ids are a stable key (§5) | They are `reinterpret_cast<std::uintptr_t>(&object)` — **pointer values**, minted fresh on every save. They round-trip *within* one document and are worthless as persisted state. §5's "persist a list of object ids" cannot work as written. |
| An unreferenced top-level object survives a load | It does not. `~SProjectLoader` deletes the handle `SLink`s, and anything no real link or registry pinned goes to zero references and is deleted. **A detached arrangement root with zero placements needs a registry pin that the loader re-establishes**, or it silently vanishes on the first save/load. |
| The arranger is "almost root-agnostic" | `rebuildRows()`/`appendRowsFor()` are. The class is not: **128 `model_` occurrences on 113 lines** of a 4901-line file. But note what it does **not** do: `insertTrack`, `reorderTrack`, `applyAudibility` and `notifyTreeChanged` are never called by the view — only by action classes, which re-derive the mixer from the **project** (`sremovetrackaction.cpp:46`, `:100-101`). What the view really holds is `model_` as a walk root plus the selection API. See §11.2, which overstated this. |
| The arranger is constructed by the model | Not since proposal 14 Phase 6. `SStdMixer::getDetailEditWidget` goes through `sdetaileditors::create()`, a **class-name-keyed factory** that `main/timeline` registers. A second root of the same class gets the same editor for free. |
| Solo is global | **Half true, and the half that is false is load-bearing.** `SStdMixer` does pass `this` (`sstdmixer.cpp:195-196`, `:223`, `:251`). The *other* summing container does not: `STrack::applyChildTrackAudibility` asks `splacements::rootContainer(getProjectSafe())` (`strack.cpp:1367-1369`, and again at `:305-306`, `:1536`), as do `smidioutpump.cpp:231-232`/`:444-445` and `strackdetailpanel.cpp:229`. So a **folder inside an arrangement** resolves solo against whatever root is ambient — and `ssolorules.h`'s own comment states the assumption (*"`root` is the project's root container"*) and exists **because three copies of this rule had already disagreed**. This would be the fourth. |
| "The root" has one caller | The honest split is **34 direct `project->getRootComponent()` sites** in the object/UI slices, **20 more in `main/shell`**, and **~99 indirect ones through `splacements::rootContainer`** across 69 files. They mean two different things (§12 D4) and today nothing distinguishes them — which is why redefining `rootContainer` re-points 99 call sites *with no per-site review*. |
| Two single funnels exist and are worth their weight | `SMainWindow::ensureArranger_()` (`smainwindow.cpp:2149`, **private**, **29 call sites, all in that file**) is the sole arranger accessor; testkit verbs reach it indirectly, by finding the window through `qApp->topLevelWidgets()` and calling public `SMainWindow` methods whose bodies use it. `SActionHistory::submit()`/`drain_()` is the sole action funnel, `drain_` is synchronous with one caller, and `SActionUndoCommand` is constructed in exactly one place (`sactionhistory.cpp:89`). Much — **not** all, see §11.13 — of the tab-scoping ripple lands in those two. |

## 11. Challenging the use case

### 11.1 It is a second verb, not a changed one

`create-asset` must keep meaning what it means. Seven committed qxa cases assert
its non-destructive semantics (`asset_window_offset`, `asset_over_muted_container`,
`loop_asset_extend`, …), and "window a bus I already have, so I can drop that
section somewhere else too" is a legitimate operation in its own right. The new
gesture is **extract**: *move* these tracks out, *place* one clip where they were.
Two menu items, two verbs, two undo entries. Overloading one of them would break
the cases and the habit at once.

### 11.2 The tab root should be an `SStdMixer`, not "any container `SObject`"

§1 locked route (A): generalize `SStdMixerView` to take `SObject &root`. For *this*
use case that is the wrong trade. The extracted arrangement is a **summing root
holding lanes** — which is precisely what `SStdMixer` is. Make the new root an
`SStdMixer` and:

- the arranger needs **no change at all** — `sdetaileditors.cpp:9-25` keys on
  `metaObject()->className()`, so a second instance gets `SStdMixerView` for
  nothing, and the `SStdMixer` ctor takes no global ownership (it connects
  `channelsChanged` per instance, `sstdmixer.cpp:489-519`), so the channel width
  is safe.

**Of the four freebies an earlier draft of this section claimed, measured, one
and a half survive** — and the wording mattered, because an agent would have
used it to skip milestones:

| Claimed free | Actually |
|---|---|
| the arranger | **free**, per above |
| track selection is per-root | **half** — it does live on `SStdMixer`, but `SSelectionManager` resolves against the project root (`sselectionmanager.cpp:10`, `:26`, `:62`) and `svirtualkeyboarddock.cpp:67`/`:78` read `getSelectedTrack()` off it |
| solo is per-root | **not free** — `STrack` asks the ambient root at three sites (§10) |
| the track verbs work | **not free** — the view never calls that API; the actions re-derive the mixer from the project. That is §12 D4's editRoot work, not something D2 buys |
| invalidation works | **not free, and not even nearly** — §11.11 |

D2 is still the right decision; its *justification* is the arranger and the
class-keyed factory, nothing more.

Route (A) is still the right answer for the *other* tab kind — drilling into a
**folder track** (§1's `STrack::getDetailEditWidget` returning non-NULL). That is
where the 113 uses have to be faced, and it buys a different feature. It is
deferred, explicitly, and nothing here forecloses it.

### 11.3 The range and the whole-track move are in tension, and something has to give

The asset window is `[t0,t1)`. The tracks carry whatever they carry. Three ways
out, and none is free:

1. **Split at the boundaries** (05 §2.4): only the middle moves. Rejected — a
   track's fader, inserts, automation and take stacks are not divisible at a time
   boundary, so a split leaves *two* claimants to one track's identity. 05 §4b
   already rejected extract-and-replace for the same reason.
2. **Move whole tracks, window the range.** Material outside `[t0,t1)` leaves the
   master and is not placed anywhere. Simple, honest, and **lossy in audible
   output**.
3. **Move whole tracks, window the full extent.** Nothing is lost; the "selected
   time range" stops meaning anything.

(2) with the loss **announced** is the answer, plus (3) offered as a checkbox. A
bound that is announced is a design; a bound that is silent is a bug — the house
rule that already governs search truncation and cache eviction.

### 11.4 N lanes leave and 1 lane arrives — and that is a mixer change

An asset placement is **one clip on one lane**. So after extraction the master has
one lane whose fader governs the whole submix. That is a group bus, which raises
the obvious question: **is a folder track with a tab what is actually wanted?**
It gives the same routing with none of this machinery. The difference, and the
whole payoff, is **re-placement**: an extracted arrangement can be dropped at four
more positions and edited once. If the user never places it twice, the folder is
strictly simpler — and it is worth saying so in the dialog rather than discovering
it after the fact.

### 11.5 "A root of its own" is safer than what we do today, not just tidier

Today's in-place asset is heard **twice** (the container in place, plus each
placement), and `SPlaceAssetAction` carries a hand-written cycle guard that
refuses a whole-mixer asset outright and refuses placement into the container's
own subtree. A **detached** root is heard exactly once, cannot be placed inside
its own container because its container is not in the master tree, and makes the
whole-mixer refusal moot. The requester's instruction removes a class of bug.

### 11.6 …but it makes the arrangement inaudible while you edit it

This is the sharp edge of (iii) and the "for the time being" framing understates
it. (The *cost* of fixing it turned out lower than this section assumed — §11.15.) A detached root is not summed into the master, so with the master playing you
hear the sub-arrangement **only through its placement, at the placement's
position** — and if it is placed zero times, not at all. **You cannot iterate on
a drum loop you cannot hear.** Local audition (§4's "the playing tab") is
therefore a v1 requirement of this use case, not phase 5 polish.

What makes it tractable is §4's own locked decision: **one playhead, one playing
tab**. Everything downstream — `AudioEngine`, `twSpeaker`, `RenderSession`,
`SLivePlanBuilder`, the meters, `SMidiOutPump`, `SAudioRecorder` — assumes exactly
one root, and it can keep assuming that. `SApplication::rootComponent()` is one
function; the swap happens at a transport boundary and nowhere else.

### 11.7 Undo across roots is the one hazard that corrupts rather than fails

Every structural action addresses its target by an **index-path from "the root"**,
resolved at apply time. If "the root" is the *active* arrangement, then undoing an
action after switching tabs resolves `{2}` in the **wrong tree** — and index 2
usually exists there. That is not a rejected action, it is a silent edit to
somebody else's arrangement. It must be fixed in the same commit that makes the
root switchable, and it must be fixed **once**, in `SActionHistory`, not in sixty
action subclasses.

### 11.8 Recording, monitoring and MIDI-out are master-only, and must say so

`SAudioRecorder::collectArmed` walks `project->getRootComponent()`;
`SLiveMonitor` `dynamic_cast`s it to `SStdMixer`. An armed track inside a
sub-arrangement is invisible to both. The arm button must **refuse with a
message**, exactly as `ArmedForRecording` is already inert on load rather than
silently opening a microphone.

### 11.9 The cycle guard is too weak for detached roots

`strackpath::isSelfOrDescendant` walks `STrack` children within one root. With two
roots, "arrangement A places an asset of B, B places an asset of A" is a cycle it
cannot see, and the live pull would recurse. §6 declared acyclicity a model
invariant; with detached roots it needs a real reachability walk over
`childLinks()` **and** `SCut::getContent()`.

### 11.10 Two open arrangers share one preference file

`SStdMixerView::saveTrackControlWidth` writes `MixerView/TrackControlWidth`
whenever a view's control column changes. Two open arrangers fight over one key.
Accepted (last writer wins, it is a cosmetic width) — recorded so it is not
rediscovered as a bug.


### 11.11 The invalidation walk is master-rooted, so an edit in a detached arrangement is inaudible

**This is the finding that decides whether any of this holds**, and it is a
verified defect rather than a risk. `SObject::invalidateRenderPath()`
(`sobject.cpp:987-1000`) and its range-scoped twin (`:1055-1066`) walk **down from
`project->getRootComponent()`** and bump the epoch of every chain that *contains*
the edited object. For an object under a detached root the master's walk finds
nothing, so `contains` is false everywhere and **nothing is bumped — not even the
edited object's own epoch**, because the fallback branch is guarded on "there is
no root", not on "not found".

The epoch is the *sole* staleness signal in the page cache. So the first freeze of
a position is correct and **every edit after it is silently ignored** — no log
line, no rejected action, no refusal. And it compounds: `SCut::buildCapture_`
early-returns while a snapshot exists (`scut.cpp:325`), so the asset's first
snapshot is what every placement plays **permanently**.

Worse than a uniform failure, the split is diagonal. `notifyDependentsChanged()`
(`:1108-1127`) *does* cross the reference edge — `SCut` registers itself as a
dependent of its content (`scut.cpp:1269`) — but it has **exactly four callers,
all property setters**: `setSolo` (`:29`), `setMuted` (`:39`),
`setArmedForRecording` (`:49`), `setVolume` (`:88`).

| Edit inside a detached arrangement | Reaches the placement? |
|---|---|
| mute / solo / volume on a track | **Yes**, via `dependentLinks_` |
| add / move / split / remove a clip; plugin insert, bypass, param; automation; take switch — anything through `invalidateRenderPath[Range]()` | **No.** Silently, permanently inaudible |

So it looks like it works until you move a clip. The fix is small and local: give
both functions the master root **plus every root in the arrangement registry**
(a loop over what D3 already builds), and fall back to `bumpRenderChainEpoch()`
when none contains the object — strictly better than today's silent nothing.

This is the **fourth** time in this codebase that "walk from the project root"
has been the obvious design and the wrong one, after the level meters, MIDI-out
and the metronome.

### 11.12 A `window="range"` extraction silently shortens every later render

`SProject::getDurationFrames()` measures the **master** root's extent and
`SRenderAction` defaults its end to it — that header's own words: *"this narrows a
render, it never widens one"* (`srenderaction.cpp:94`). After an extraction the
master's contribution from those tracks is one clip whose length is the **asset
window**, so a project whose material ran to 60 s can start rendering 4 s with no
warning. The arranger ruler meanwhile measures its own root (`sstdmixerview.cpp:4761`),
so ruler extent and render length now disagree.

### 11.13 The ambient edit root leaks past the action funnel

§12 D5 makes the active arrangement part of an action's recorded context, and
because `apply()` bodies read the root through `rootContainer(project)`, setting
it around apply/undo/redo genuinely does reach all ~99 of them. But three classes
of caller are **outside** that funnel and are not covered:

- **The view mints a path against one root and submits it against another.**
  `sstdmixerview.cpp:629`/`650`, `689`/`706`, `548`, `4447` all compute with
  `pathOf(model_, …)` and resolve with `rootContainer(project)`. That is benign
  today only because the two are the same object. Index `{0}` exists in both
  trees, so the action *succeeds* and edits the wrong arrangement.
- **Model paths that are not actions at all.** `STrack::applyChildTrackAudibility`
  (`strack.cpp:1367`) reads the ambient root from a model refresh, not from an
  `apply()`.
- **`apply()` reached outside `submit`** — Save/Open (`smainwindow.cpp:316`,
  `:478`), `SCompositeAction`'s children (`scompositeaction.cpp:18`, `:22`), and
  delegating actions (`ssettrackvolumeaction.cpp:35`).

And one place where a bare index-path **cannot express the answer at all**:
`SRemoveAssetAction` computes `pathOf(project->getRootComponent(), &cut->getContent())`
(`sremoveassetaction.cpp:33-39`); for a detached root that returns `{}`, and `{}`
means *the root itself*. So undoing the removal of an arrangement asset registers
an `SCut` **windowing the entire master** — and `dissolve-arrangement` routes
through it.

The principled answer is a **root-qualified path** (`{rootName, indices}`) rather
than an ambient root, and the churn argument for the ambient one is weaker than
it looks: `resolveByPath`/`pathOf` **already** take an explicit root and name no
`SProject` (`sobjectpath.h:20-36`) — only `rootContainer` reaches into the
project. Against that: it is a ~99-site change touching every action's wire
format. §12 D4 is therefore revised rather than kept, and the residual tension is
named as the requester's call.

### 11.14 The outside-the-window material can be placed after all — and the capture is not cheap

Two things the "move whole tracks, window the range, announce the loss" answer
(D7) got wrong by omission:

**There is a fourth option and it is nearly free.** Place the outside material as
**two more clips of the same asset** — a head window `[0,t0)` and a tail
`[t1,end)` on the same new master lane. No track splitting, no second claimant to
a track's identity, **nothing lost in audio**, and it is two extra `place-asset`
calls with different windows on machinery the extraction already builds. It was
filed under "not in v1" without anybody noticing the cost of not doing it.

**Nobody priced the capture.** `SCut::buildCapture_` renders container content
into an in-memory `channels × frames` float buffer (`scut.cpp:425-460`, published
at `:553`), **synchronously, from the UI thread** via `rebuildReader`
(`:175-177`, annotated at `:1110` as *"the caller is the UI thread"*), and the
code's own estimate — *"can take tens of milliseconds"* (`:319-322`) — is off by
orders of magnitude for a whole arrangement. `window="extent"` is unbounded. That
makes §11.4's folder-track alternative not merely simpler but **dramatically
cheaper**, and it means D7's "extent" checkbox needs a size warning of its own.

### 11.15 Local audition is cheaper than §11.6 feared

The worry was that `AudioEngine` is *constructed* with its `synthOutput`
(`audio_engine.h:61`) and has no setter. True — and irrelevant, because
`twSpeaker::startOutput()` **re-reads the root and mints a fresh engine on every
start** (`twspeaker.cc:114-132`: `context_->rootComponent()`, then
`releaseEngine()`, then `make_shared<AudioEngine>`), and `releaseEngine()` joins
the readahead thread. Metering follows for free too — `masterProbe_.setTap(rootComponent())`
re-binds every call (`sapplication.cpp:452-461`). A transport-boundary swap is
close to free.

Three things that are *not* free, and that §12 D9 must name:

- **There are three roots, not two** — master, edit, and *playing* — and
  `SRenderAction` reads none of the first two: it takes
  `project->getRootComponent()->getRootComponent()` directly
  (`srenderaction.cpp:67-69`).
- **The metronome bans it.** `metronomeWanted` raises a live lane whenever the
  click is on and the transport rolls, and a metronome-only closure is explicitly
  permitted (`sliveplanbuilder.cpp:35-51`, `:212-214`). The metronome switch is a
  **project property that travels with the file**, so "refused while any live lane
  is up" bans audition for every project saved with the click on — and a gate
  written against test projects would not notice, because they default it off.
- **`~AudioEngine` never cancels its demand** (`audio_engine.cc:63-70`;
  `:757-761` says outright *"The stale handle is simply dropped — its nodes finish
  and publish pages"*), so after a swap the outgoing root keeps freezing at
  priority 9 ahead of the incoming one's demands. A first-audition latency spike,
  not a correctness bug; `CaptureRevalidator::retireComponents` exists for it and
  is not called on the Play/Stop path.

## 12. Decisions (this revision)

> **Read §13 and §14 alongside this list; where they disagree with §12, the
> LATER section wins.** Review added D13-D19 (§13); the requester then settled
> the two open questions with D20-D21 (§14). Net effect on §12: **D4 is RETIRED**
> (D16 revised it, D21 retires both), **D5 is RETIRED** (D21 makes it
> unnecessary), D7 is superseded by D20, D9 is qualified by D19.

**D1. Two verbs.** `create-asset` unchanged. `extract-arrangement` is new, and its
mutual inverse `dissolve-arrangement` is a first-class user verb ("bring these
tracks back into the song"), not a private undo helper.

**D2. An arrangement root is an `SStdMixer`.** Overrules §1 route (A) *for this
feature*. Folder-track tabs, and the generalization they force, are deferred and
unblocked by nothing here.

**D3. Arrangements are NAMED, registered on the project, and the name is the
stable id.** One reference pinned per entry (the `registerAsset` discipline).
Persisted as an **additive attribute on the existing `<SStdMixer>` element**, and
re-registered from a `deferResolve` — because an unreferenced root is deleted at
the end of a load, and because §5's pointer ids cannot be persisted state. This
retires §5 as written: open tabs persist **by arrangement name**.

**~~D4. `getRootComponent()` keeps meaning the MASTER root; `editRoot()` is the
arrangement being edited.~~ RETIRED by D21** — there is no ambient edit root;
paths are root-qualified. `getRootComponent()` does keep meaning the master. The engine half keeps asking for the master. The
action and UI half asks `splacements::rootContainer()`, which returns `editRoot()`.
Enforced by a grep gate, in the manner of `check_layering.py` / `check_logging.py`.

**~~D5. The active arrangement is part of an action's recorded context~~ RETIRED
by D21** — an action carries its qualified path in its own parameters, so undo is
exact by construction and §11.7's corruption mode does not exist.

**D6. Extraction moves WHOLE TRACKS** — clips, faders, inserts, automation, take
stacks — preserving order and nesting. No splitting, no copying, one source of
truth (05 §2.5).

**D7. The window is the selected range; ONE placement returns at `t0` on ONE new
master lane.** *(Superseded by D20: the outside material becomes registered,
unplaced SNIPPETS rather than a reported loss.)* Material outside the window is
**not** placed and the operation **reports** how much. A "use the whole extent instead" option is offered. Placing
the outside sections as further clips is explicitly not in v1.

**D8. A sub-arrangement is not summed into the master.** It is audible only
through a placement — the requester's instruction, and the safer model (§11.5).

**D9. Local audition is v1, and it is "one root at a time, swapped at a transport
boundary".** *(Cheaper than §11.6 feared — see §11.15 — and the metronome caveat
is D19.)* The transport binds to the playing arrangement (§4 unchanged);
`SApplication::rootComponent()` follows it; the swap is refused while recording,
while a live lane is up, and during a render. The offline half (`use-arrangement`
+ `render`) lands first because it is gateable with no transport at all.

**D10. Arming, recording, monitoring and MIDI-out are master-only in v1, and
refuse loudly** in a sub-arrangement (§11.8).

**D11. One whole-graph reachability check** (`sarrangements::reaches`) consulted by
`place-asset` and `extract-arrangement` alike — one cycle guard, not two (§8's
last bullet, now enforceable).

**D12. §3 and §4 stand.** One playhead owned by the playing tab; selection owned
by the active tab. D2 pays for most of the selection work in advance, and the two
single funnels (`ensureArranger_`, `SActionHistory::submit`) are where the rest
of the ripple lands.

## 13. Decisions added by review

**D13. An edit is invalidated from the master root AND from every registered
arrangement root** (§11.11), falling back to bumping the object's own epoch when
none contains it. A model invariant, and it belongs in `main/model/CONTRACT.md`
as one. **Blocking: nothing downstream is testable in audio until it lands.**

**D14. A container capture may never publish silence produced by a defused
page.** `buildCapture_` zero-fills, `break`s out of the page loop on a page with
no valid frames, then publishes **unconditionally** and sets `everHadCapture_`
(`scut.cpp:425`, `:446-448`, `:553-560`); combined with the early-return at
`:325` that silence sticks. An extracted arrangement's asset is a container
capture by construction, so a path that is hard to reach today becomes the normal
one. Refuse the publish; do not complete it.

**D15. Extraction reports the render-length consequence, not just the material
loss** (§11.12).

**D16. D4 is revised: the ambient edit root covers the action path only, and the
view passes its own root explicitly.** Every view-originated submit carries
`model_` rather than reading the ambient value (§11.13) — four call sites, and it
removes the largest ambient-root failure surface in the feature. A **root-qualified
path** (`{rootName, indices}`) is recorded as the target state; the ~99-site
migration is deferred, and the residual leaks (`applyChildTrackAudibility`, the
non-`submit` `apply()` callers) are named rather than papered over.

**D17. `SRemoveAssetAction` must address its content by arrangement NAME when
that content is a registered root, and refuse rather than fall back to `{}`**
(§11.13). Its current behaviour turns an undo into an asset over the whole
master.

**D18. `extract` and `dissolve` are only mutual inverses at one placement.**
Above one, `dissolve` either refuses (matching `remove-arrangement`, which does)
or its inverse is a composite that restores every placement. Left as-is they
contradict each other inside one milestone.

**D19. The metronome does not veto an audition swap** (§11.15) — a metronome-only
closure is excluded from D9's refusal, or the click is suppressed across the
swap; and the gate is written with the click **on**.

## 14. Decisions from the requester (2026-08-20, after review)

Two answers that settle the two open questions §13 left, and both simplify the
plan rather than extending it.

**D20. The out-of-window material becomes SNIPPETS: named, registered, and
unplaced.**

> *"I'd like to think of the out of (timeline) window material as snippets, as
> patches that the user may or may not use constructing the effective window on
> the timeline."*

So D7's "announce the loss" and §11.14's "place it as two more clips" are both
wrong — the first gives up material the user still owns, the second decides for
them where it goes. The extraction instead registers **one asset per contiguous
span of material outside the window**, each a window over the same arrangement,
each **unplaced** and visible in the resources dock. The user drags in whatever
they want.

Consequences:

- **Nothing is lost and nothing is force-placed.** The arrangement is the single
  source of truth; the primary asset and every snippet are windows over it, so
  editing the arrangement updates all of them (§0's live-pull payoff, for free).
- The spans are the **maximal contiguous extents of actual material** outside
  `[t0,t1)`, computed from the extracted tracks' clip extents — not a blind
  head/tail pair. A gap in the material produces two snippets, which is what
  makes them *patches* rather than leftovers.
- The count is **bounded and announced** (`kMaxSnippets`), like every other bound
  in this tree. A dense arrangement must not register ninety assets silently.
- Naming is derived and stable: `<Arrangement> snippet 1..N`, in time order.
- `window="extent"` therefore needs no size warning for *loss* reasons any more —
  but it keeps the one from §11.14, because the capture cost is real.

**D21. Every arrangement is addressable by every path descriptor. There is no
ambient edit root.**

> *"I believe multiple open tabs represent essential parts of one project. Hence
> they all should be addressable by actions, path descriptors of any kind and
> alike."*

This **overrules D4 and D16** and retires the ambient-root design entirely. A
path becomes **root-qualified**:

```
SObjectPath { QString root;  QList<int> idx; }      // root empty == the master
"Drums:0,1"          the 2nd child of Drums' 1st lane
"0,1"                the same address in the master — the existing spelling, unchanged
```

An action therefore *carries* the arrangement it addresses, in its own
parameters, and every consequence of that is a simplification:

| What the ambient design needed | Under D21 |
|---|---|
| `SProject::editRoot()` / `setEditRoot()` | **gone** |
| `rootContainer()` redefined under ~99 unreviewed call sites | **gone** — each site says which root it means, or keeps meaning the master because a bare path still parses to the master |
| `tools/check_editroot.py` | **gone** — there is no ambient value to police |
| D5's capture-and-restore of the root around apply / undo / redo | **gone** — undo is exact **by construction**, because the inverse carries the same qualified path |
| D5's cross-root merge guard (§11.13) | **free** — two faders at `{0}` in two roots now have *different* `mergeKey`s |
| The three ambient leaks (`applyChildTrackAudibility`, non-`submit` `apply()` callers, the view's path/root mismatch) | **gone** — none of them can read a value that does not exist |
| D17's `SRemoveAssetAction` `{}` hazard | **gone by construction** — `pathOf` over a registered root yields `Drums:` rather than `{}`, and `{}` keeps meaning only "the master itself" |
| `use-arrangement` as a *correctness* mechanism | **demoted** to what it should always have been: a scripting convenience that sets the DEFAULT root for bare paths in the cases that want one |

The cost is the migration the ambient design was chosen to avoid: the path
helpers, the ~99 resolution sites and the actions' attribute parsing. It is real,
but it is **mechanical and compiler-checked** where the ambient design's risk was
silent and runtime — which is the trade the requester has made, and it is the
right way round. `resolveByPath`/`pathOf` already take an explicit root and name
no `SProject` (`sobjectpath.h:20-36`), so the type slots in where the concept
already lives.

Two properties that must hold through the migration, and that the gates check:

- **A bare path still means the master, byte for byte.** Every existing `.qxa`
  case, every project file and both goldens are unchanged, because nothing writes
  a qualifier for a master-rooted path.
- **A qualified path that names an unknown arrangement is REFUSED**, never
  silently resolved against the master. That refusal is the whole safety property
  the ambient design could not offer.
