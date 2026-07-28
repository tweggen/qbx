# Proposal 32: Project versioning, sharing, and portability

> **Status: DRAFT (2026-07-28).** A layered answer to "how does a user keep
> revisions of a song, back it up off-machine, work across machines, hand a
> track to a studio musician, and collaborate with others" — **without** qbx
> growing its own version-control system, and explicitly **without** real-time
> collaborative editing (operator streams, a server, OT/CRDT), which is
> out of scope for this proposal.
>
> **Thesis:** qbx should not *be* a VCS. It should make its project format
> *VCS-able* — diff-stable, portable, self-contained — and then wrap an
> existing VCS (git + LFS) in a thin, DAW-native History/Compare/Restore UX.
> The parts only qbx can do (stable identity, a portable bundle) are exactly
> the parts that are broken today; the parts git already does better than we
> ever could (history, remotes, branching, content dedup) are the parts we
> must not reinvent.
>
> Prerequisite / adjacent reading: the project file format
> (`docs/PROJECT_FILE_FORMAT.md`), the persistence module
> (`main/persistence/CONTRACT.md`), `SObject` serialization
> (`main/model/src/sobject.cpp`), the content-addressed sidecar store as a
> reuse pattern (`27_ANALYSIS_SIDECARS.md`, `tw303a/sidecar/`). **In-flight
> sibling work:** relative, cross-platform-safe media path serialization
> (`SExternFile` / `SPlainWave`, `SProject::linkToFile`) — see the coordination
> note in §D.

## Decisions taken with the requester (2026-07-28)

1. **Stable ids are UUIDs**, not a project-scoped counter. A counter risks
   collisions in the collaboration case — two branches off a common ancestor
   each allocate the next counter value and produce colliding ids on merge,
   which is exactly the M3 scenario. UUIDs are globally unique by construction,
   so independently-created objects never alias. Accepted cost: larger, less
   human-readable id strings in the diff.

## Motivating use cases (the challenge to meet)

1. Producing a song; fall back to / compare against an earlier version.
2. (1) + a non-local backup.
3. Work on the same project on multiple machines.
4. Hand a project to a studio musician to add a track.
5. Work with multiple people (non-real-time).

Real-time collaboration (shared live session, operator stream, server) is
deliberately **not** addressed here; it is the natural next step *after* (5) and
would be its own proposal.

## A. What is wrong today

Two properties of the current format block every use case above. Both are
prerequisites — no sharing story works until they are fixed, and neither is
git's job to fix.

### A.1 The project file is not diff-stable

Object identity is the raw in-memory address of the object, cast to an integer
and written fresh at serialize time. Four sites, identical approach:

| Attribute | Site |
|---|---|
| `id=` on every object | `main/model/src/sobject.cpp:114` |
| `objectId=` on every link's target | `main/model/src/slink.cpp:10` |
| `rootId=` | `main/model/src/sproject.cpp:51` |
| `pluginChainId=` | `main/objects/track/src/strack.cpp:44` |

```cpp
// sobject.cpp:114
o << " id='" << reinterpret_cast<std::uintptr_t>(this) << "'"
```

There is no persistent identity stored on the object — no UUID, no saved
counter. The value is computed from `this` at save time and used by the loader
(`main/persistence/src/sprojectloader.cpp`) only as an *opaque within-file
matching key* (`objectDict_`), then discarded. ASLR moves every object on every
process launch, so **open → edit one track → save rewrites nearly every line of
the file** — every `id`, `objectId`, `rootId`, `pluginChainId` changes even
where nothing was touched.

Three smaller sources of churn in the same path:

- `nRefs='<count>'` (`sobject.cpp:115`) is the live reference count, serialized
  into every object but recomputed on load — redundant in the file and able to
  drift.
- `properties='…'` on `SProject` (`sproject.cpp:80-83`) packs the property dict
  into one JSON-in-an-attribute blob; any change rewrites the whole line. (Its
  internal key order is stable — `QJsonObject` sorts by key — so only the *value*
  churn matters.)
- The loader is an explicit leaves-first dependency resolver
  (`sprojectloader.cpp:44-216`): objects are re-created in dependency order and
  re-parented in creation order, so a load→save round-trip can **reorder the
  top-level `<…>` blocks** relative to the original file, independent of the IDs.

Consequence: git history is unreadable (every commit touches the whole file) and
line-level merges — the substrate of use cases 4 and 5 — are impossible.

### A.2 The project is not portable

Media is never copied into the project; it is referenced by path
(`main/objects/wave/src/splainwave.cpp:25-28`):

```cpp
o << " filename='" << getFileName() << "'";
```

Today those paths are **absolute** (imports come from a file dialog,
`main/shell/src/smainwindow.cpp:1168`; recorded takes are written to
`<projectDir>/YYYYMMDD_HHMMSS_mmm_<trackId>.wav`,
`tw303a/record/src/recording_session.cc:275-283`, but stored absolute). Clone
onto a second machine and every sample dangles. This alone kills use cases 3, 4,
5. **A sibling change is converting these to relative, cross-platform-safe paths
(§D)** — which closes most of this gap, with one residual sliver called out in
Milestone 1.

### A.3 What is already fine

- The container is a single plain-text `.qxp` XML file
  (`main/persistence/src/ssaveprojectaction.cpp:33-42`) — the right shape for
  git; the problem is its *contents*, not the container.
- The QAF sidecar cache lives outside the project tree, in the user cache dir
  (`main/shell/src/sapplication.cpp:383-389`) — derived data will not pollute a
  repo; no `.gitignore` rule needed for it.
- There is **no** existing versioning / snapshot-to-disk / backup / autosave /
  export-bundle feature to reconcile with. Undo is in-memory only
  (`SActionHistory` over a `QUndoStack`). This is greenfield.

## B. What is explicitly *not* qbx's job

- **A version-control engine.** No home-grown history, diff, or merge. git + LFS.
- **A merge algorithm for timelines.** DAW timeline data does not textually
  auto-merge safely. Collaboration is made *additive* (per-track files) and
  *coordinated* (soft ownership), not auto-merged. This is the Perforce/gamedev
  lesson, not an accident.
- **Real-time collaborative editing.** Out of scope, as stated.
- **A production-tracking tool** (à la Kitsu/Zou). That is task/review/status
  coordination layered *on top of* a VCS — complementary, not this proposal.

## C. Design: four milestones, each independently useful

### M0 — Make the format diff-stable *(the linchpin; pure persistence work, no VCS)*

Give every serialized object a **persistent identity** allocated once and stored
on the object, replacing the pointer-derived IDs:

- Allocate a stable **UUID** on object creation (see the §Decisions note — a
  counter is rejected for M3 collision risk) and store it on the object;
  serialize *that* instead of `reinterpret_cast<std::uintptr_t>(this)` at the
  four sites in §A.1. The loader
  already treats IDs as opaque within-file keys, so the resolution logic in
  `sprojectloader.cpp` is unaffected — only the *source* of the string changes.
- Stop serializing the derived `nRefs` (recomputed on load anyway).
- Impose a **canonical, stable element ordering on save** that does not depend on
  load-time dependency reconstruction (e.g. sort top-level children by stable id,
  or preserve and re-emit an explicit saved order), so a load→save round-trip is
  a no-op diff.

Acceptance: open → save → re-open → save produces a **byte-identical** (or at
minimum line-stable) `.qxp`; a one-track edit produces a diff scoped to that
track. This milestone alone delivers **use case 1** via plain `git` that a
developer can already drive today — readable diffs, real "fall back to an earlier
version," honest `git diff`/`git log`.

### M1 — Self-contained project (collect media into the bundle)

The sibling's relative-path work makes references *portable in form*; a relative
path only *resolves* after a clone if the file travels with the project. Add a
**"Collect / consolidate media"** operation (on save-as-bundle, or explicit
menu) that copies externally-referenced media into a project `media/` folder and
rewrites the reference to the in-bundle relative path. Recorded takes already
land next to the `.qxp`, so they need nothing; external *imports* are the case
this closes. Content-addressing the copied files (reuse the `twSidecarStore`
hashing pattern) is an optional add-on — it dedupes identical samples and is
ideal for LFS — not a requirement. Delivers **use cases 2 and 3**: a portable
folder that pushes to a remote and clones elsewhere, media and all.

### M2 — History / Compare / Restore UX over git + LFS

A thin, opinionated binding so a musician never types `git`:

- A "History" panel: **Save Snapshot** (= commit), a timeline of versions,
  **Restore**, and **Compare** (at minimum: which tracks/clips/params differ —
  a semantic diff over the now-stable IDs, not a raw text diff).
- Auto-generate `.gitattributes` / LFS tracking for `media/` and a `.gitignore`
  (the sidecar cache is already out of the tree, so this is small).
- A remote is optional; when set, Save Snapshot can push. Covers **use cases 1+2
  for non-developers** and **use case 3** with a remote configured.

Nothing here reimplements VCS internals — it shells out to git and presents a
DAW-native surface (cf. Cubase project versions, Bitwig's bundling).

### M3 — Additive, non-real-time collaboration

- **Per-track file split**: serialize each track to its own file within the
  bundle so two people editing *different* tracks touch *different* files and git
  auto-merges cleanly. This is the real lever that turns "a musician adds a
  track" (**use case 4**) into a trivially additive commit.
- **Soft track ownership / lock** convention so concurrent edits to the *same*
  track are avoided rather than merged (the Perforce lesson).
- **Export/import a single track + its stems** as the interchange unit for the
  studio-musician handoff.

Honest boundary: stable IDs (M0) make diffs readable and additive merges clean;
concurrent edits to the *same* track still need coordination or explicit conflict
surfacing — **not** auto-merge. The moment that stops being acceptable is the
moment the scoped-out real-time/OT server becomes the right tool. This proposal
stops cleanly at that line.

## D. Coordination with in-flight sibling work

A sibling change is converting media paths to relative, cross-platform-safe form
in `SExternFile` / `SPlainWave` serialization and `SProject::linkToFile`
(currently the relative-resolution branch is gated on `sampleBaseDir_`, set only
by the `.qxa` test runner — this generalizes it). That work and **M0** both touch
the persistence/serialization path (`main/model/src/sobject.cpp`,
`main/persistence/src/sprojectloader.cpp`, the `serialize` methods). **Sequence
them so they don't collide on the same files** — recommended order: let the
relative-path change land first (it is narrower and already in progress), then do
M0's identity/ordering change on top, since M0's byte-stability acceptance test
should be written against the *final* path format. M1's collect-media step is the
natural completion of the sibling's work and should be co-designed with it.

## E. Options considered and rejected as the primary mechanism

- **Dropbox/OneDrive on the live folder** — acceptable as dumb backup of a
  *closed* project (partially serves use case 2), but last-write-wins silently
  eats collaborators' edits, offers no semantic history for use case 1, and risks
  partial writes while qbx holds the folder open. Not a foundation.
- **Raw git in the user's face** — the correct substrate (chosen, under the M2
  veneer) but hostile to a non-developer and worthless before M0.
- **Kitsu/Zou** — production tracking on top of a VCS, not file versioning;
  complementary, not the mechanism.
- **CRDT project document** — pulls toward the scoped-out real-time world;
  ordered-timeline CRDTs are hard. Rejected for this proposal.

## F. Open questions

1. `.qxp` single-file (M0–M2) vs. project-as-folder bundle (needed by M1's
   `media/` and M3's per-track split). Does M1 introduce the folder form, and is
   the single-file `.qxp` retained as an export/"flatten" format?
2. Semantic Compare (M2): how much does it show — track/clip add/remove/move —
   and does it reuse any of the action-model vocabulary (`docs/ACTIONS.md`)?
3. Migration: how does an old pointer-ID `.qxp` acquire stable UUIDs on first
   load under M0 (assign-on-load, one-time rewrite)?
