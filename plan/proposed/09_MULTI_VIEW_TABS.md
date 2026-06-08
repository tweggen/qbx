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
