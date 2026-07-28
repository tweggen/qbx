# Proposal 32: Project versioning, sharing, and portability

> **Status: DRAFT (2026-07-28; reconciled with the merged `SFilePathRef`
> relative-path work same day).** A layered answer to "how does a user keep
> revisions of a song, back it up off-machine, work across machines, hand a
> track to a studio musician, and collaborate with others" — **without** qbx
> growing its own version-control system, and explicitly **without** real-time
> collaborative editing (operator streams, a server, OT/CRDT), which is
> out of scope for this proposal.
>
> **Thesis:** qbx should not *be* a VCS. It should make its project format
> *VCS-able* — diff-stable and portable — and then wrap a **pluggable history
> backend** (git, *or* a plain shared-filesystem store on a studio NAS) in a
> thin, DAW-native History/Compare/Restore UX. The parts only qbx can do
> (stable identity, single-instance media references) are exactly the parts
> that are broken today; the parts an existing tool does better than we ever
> could (history, remotes, coordination) are the parts we must not reinvent.
>
> **Hard rule:** large media is never duplicated on disk — it is never placed
> inside a version-control object store, only referenced by path. **git-LFS is
> rejected on that ground** (see §C.1). This makes the "cheap laptop" and the
> "conservative NAS studio" cases the *same* design, not competing ones.
>
> Prerequisite / adjacent reading: the project file format
> (`docs/PROJECT_FILE_FORMAT.md`), the persistence module
> (`main/persistence/CONTRACT.md`), `SObject` serialization
> (`main/model/src/sobject.cpp`), the content-addressed sidecar store as a
> reuse pattern (`27_ANALYSIS_SIDECARS.md`, `tw303a/sidecar/`). **Landed
> (2026-07-28):** portable, cross-platform-safe media path serialization is now
> merged (`SFilePathRef`, `main/model/include/app/model/sfilepathref.h`, commit
> `f191e5d`) — this proposal has been reconciled with it; see §A.2 and §E.

## Decisions taken with the requester (2026-07-28)

1. **Stable ids are UUIDs**, not a project-scoped counter. A counter risks
   collisions in the collaboration case — two branches off a common ancestor
   each allocate the next counter value and produce colliding ids on merge,
   which is exactly the M3 scenario. UUIDs are globally unique by construction,
   so independently-created objects never alias. Accepted cost: larger, less
   human-readable id strings in the diff.

2. **Large media is never duplicated on disk.** Duplicate large files on a
   drive are unacceptable (cheap laptops, limited SSDs). Therefore media is
   never stored inside a version-control object store — it is single-instance,
   referenced by path; version control tracks only the small textual project
   files. **git-LFS is rejected** (it keeps a second copy by default; its
   `dedup` reflink is manual and unreliable on macOS/APFS). See §C.1.

3. **The shared-filesystem / NAS backend is a first-class deployment target**,
   co-equal with git — not a someday-maybe. Real studios are conservative and
   run SMB/NFS NAS boxes; most will not stand up or administer a git server.
   The history/coordination backend is therefore *pluggable* (§C), and the
   append-only + named-media-root constraints that the NAS path needs are
   design inputs to M0/M1, not retrofits.

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

Two properties of the format stood between the current app and every use case
above, and neither is git's job to fix. **One is now resolved:** the media-path
portability of §A.2 landed as `SFilePathRef` (2026-07-28). **One remains the
linchpin:** the diff-stability of §A.1 — no sharing story works until it is
fixed.

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

### A.2 The project's media references — portability (largely SOLVED, 2026-07-28)

Media is not copied into the project; it is referenced by path. This *used* to be
a raw **absolute** path written verbatim (the old
`main/objects/wave/src/splainwave.cpp:25-28`, `o << " filename='" <<
getFileName() << "'"`), so cloning onto a second machine dangled every sample —
which alone killed use cases 3, 4, 5.

**This is now fixed.** `SPlainWave::serializeSelfAttributes`
(`main/objects/wave/src/splainwave.cpp:33-36`) routes the attribute through
`SFilePathRef::toStored(getFileName(), project->projectFilePath())`
(`SFilePathRef` is a namespace of free functions,
`main/model/include/app/model/sfilepathref.h`), which encodes the reference in
three forms, most-portable first:

1. **project-relative** (`../../../tests/test_sawtooth.wav`) — the default;
2. **home-relative** (`~/audio/library/kick.wav`) — when the path would climb all
   the way up to the home directory;
3. **absolute** — only when the climb goes past home to a root, or the paths
   share no root (different Windows drives).

The anchor is `SProject::projectFilePath()` (set at `ssaveprojectaction.cpp:43` /
`sloadprojectaction.cpp:26`); `SPlainWave::fileName_` stays absolute in memory —
only the on-disk spelling changes. Load resolves via `fromStored`
(`splainwave.cpp:441-457`) and falls back to the raw stored spelling (then the
`.qxa` runner's `sampleBaseDir_`) when a relative reference does not resolve next
to the project file, so older projects still load. Gates: `filepathref_test`
(ctest) and `sample_path_portable.qxa`, the latter asserting the exact spelling
`filename='../../../tests/test_sawtooth.wav'`. (Plugin module paths are out of
scope — they resolve via the plugin search paths, `spluginslot.cpp`.)

**Two residual portability items remain this proposal's to own:** (a) media that
lives *outside* the project subtree on a shared volume — a NAS stock-media
library mounted at `/Volumes/StudioNAS` vs `Z:\` — which the three-rule scheme
can only spell as home-relative or absolute, i.e. *still* not cross-machine
portable; this needs a **named-root** indirection (§C.2). (b) a one-shot
**collect-media** step to make a self-contained bundle (Milestone 1).

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

- **A version-control engine.** No home-grown diff/merge algorithm; history is
  delegated to a pluggable backend (git, or the append-only NAS store of §C.4).
  The *only* homegrown piece is the append-only snapshot store — deliberately
  trivial (write a file, index it), precisely because git is fragile on network
  filesystems (§C.4).
- **A media store.** Large media never enters version control; it is referenced,
  single-instance (§C.1). No LFS, no annex, no content store for audio.
- **A merge algorithm for timelines.** DAW timeline data does not textually
  auto-merge safely. Collaboration is made *additive* (per-track files) and
  *coordinated* (soft ownership), not auto-merged. This is the Perforce/gamedev
  lesson, not an accident.
- **Real-time collaborative editing.** Out of scope, as stated.
- **A production-tracking tool** (à la Kitsu/Zou). That is task/review/status
  coordination layered *on top of* a VCS — complementary, not this proposal.

## C. Deployment topologies & pluggable backends

The design factors into **two orthogonal axes**. Conflating them was the error in
the first-draft thesis (which assumed git + LFS everywhere):

1. **Where media lives** — referenced in place (a NAS library, or a local
   folder) vs. collected into a self-contained bundle.
2. **What records history / coordinates edits** — a *pluggable backend*: git
   (local, an external remote, or a bare repo on a NAS share), or a plain
   **shared-filesystem snapshot store** on the studio NAS.

**M0 (stable UUIDs) is the shared bedrock under every combination** — both
`git diff` and the homegrown snapshot-compare need stable identity to produce a
readable diff.

### C.1 Hard rule: large media is never duplicated on disk

Media is never stored inside a version-control object store. It is
single-instance, referenced by path; version control tracks **only the small
textual project files**. Therefore:

- **git-LFS is rejected.** By default it keeps two local copies (working tree +
  `.git/lfs/objects`), and the cache retains extra fetched revisions besides.
  `git lfs dedup` reflinks the pair on copy-on-write filesystems, but it is a
  manual command and is reported unreliable on macOS/APFS — not a foundation for
  a hard no-duplication guarantee.
- **git-annex** *would* meet the letter of the rule (one copy in an annex store,
  a working-tree symlink to it) but is symlink-based and Windows-hostile;
  rejected for cross-platform simplicity (revisitable if the bundle topology ever
  needs single-copy dedup of a huge local library).
- This is also how professional DAWs behave — Logic/Ableton reference samples,
  they do not copy them into a versioned store.

### C.2 Media resolution — named roots on top of the merged `SFilePathRef`

The baseline is already in place: `SFilePathRef` (merged, §A.2) encodes a media
reference as project-relative → home-relative → absolute. That covers a
self-contained project and a library under `~`, but **not** media on a shared
volume whose mount path differs per machine (`/Volumes/StudioNAS` on macOS, `Z:\`
on Windows) — `SFilePathRef` can only spell that as home-relative or absolute,
neither of which is cross-machine portable.

The **addition** is a **named-root** indirection for exactly that case: a
reference stores `${STUDIO_LIB}/drums/kick.wav`; each machine maps the root
`STUDIO_LIB` to its local mount (persisted per-user in `SSettings`). This slots
in as a fourth, most-portable form ahead of absolute — three modes coexist:
relative-to-project (bundle) and home-relative (`SFilePathRef`, merged), plus
relative-to-named-root (this addition, for the NAS library). Reuse the existing
`twPluginSearchPaths` named-root pattern.

### C.3 The two topologies

| Topology | Media | History backend | Local media copies |
|---|---|---|---|
| **NAS studio** | on the NAS, by named-root reference | shared-FS snapshot store on the NAS | **0 on the laptop** (streamed over SMB) |
| **Solo / laptop** | one local `media/` folder | git (optional external remote) | **1** (the working folder; never a second) |

Neither ever creates a standing duplicate. The NAS studio never touches git or a
server; the solo user never needs a NAS. Same app, one config switch. (Off-machine
backup of the *solo* user's single media copy is an ordinary file-backup concern —
Time Machine, a synced folder, an external drive — outside VCS scope, because
media is write-once and needs no history.)

### C.4 Shared-filesystem backend (the conservative-studio path)

- **History = append-only snapshots.** Save Snapshot writes the `.qxp` to
  `<project>/.qbx/versions/<timestamp>-<label>.qxp` plus a small `history.json`
  index (author, message, time, parent). It *only ever creates new files* — the
  safest possible pattern on NFS/SMB, sidestepping the ref-lockfile corruption
  that plagues git repos on network filesystems. Compare = semantic diff of two
  snapshot XMLs over stable UUIDs; Restore = copy one back. Only the small XML is
  versioned, so hundreds of versions are megabytes — no LFS, no duplication. (The
  CVS-on-a-share idea, minus CVS's central-lock fragility.)
- **Coordination = advisory lock files with heartbeats.**
  `<project>/.qbx/locks/<project-or-track>.lock` holding
  `{user, host, pid, acquired, heartbeat}`; acquired by atomic create (or
  create-temp-then-rename — atomicity holds on NFSv3+/SMB); refreshed on a
  heartbeat; a heartbeat older than ~3× the interval is shown stale with a
  **Break lock** action naming who held it and since when. This fixes exactly
  what CVS got wrong (wedged stale `.lock` files). Advisory only — a NAS is not a
  distributed lock manager, but it is the coordination level these studios
  already run on.
- **Optional: a bare git repo on the NAS** (`git clone /Volumes/StudioNAS/proj.git`,
  no server process) is a valid backend for a git-comfortable team wanting real
  branching, *provided* edits are single-writer — which the lock already enforces.
  Offered, not the default, because git's ref updates are lock-fragile on network
  filesystems.

### C.5 The interfaces that make git and the NAS peers

- `VersionBackend { saveSnapshot, listVersions, restore, compare }` →
  `FilesystemSnapshotBackend`, `GitBackend`.
- `LockService { acquire, heartbeat, release, break }` → `FilesystemLockService`;
  a no-op for the solo git user.
- `MediaResolver` with named roots → covers bundle *and* NAS-library.

M2's History UI is written against `VersionBackend`, so one panel drives git *or*
the NAS folder, and M0/M1 are backend-agnostic.

## D. Design: four milestones, each independently useful

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

### M1 — Media resolution: named roots + optional consolidation

Default behaviour adds **zero** copies: media is referenced single-instance via
the merged `SFilePathRef` relative/home-relative paths (§A.2) *or* a **named
root** (§C.2, the piece M1 adds) for media that lives outside the project folder
on a shared mount (the NAS-library case). This alone makes a project resolvable
on another machine that maps the same root — serving the **NAS studio** topology
with no media movement at all.

For the **bundle** topology, add an explicit, one-shot **"Collect media"**
operation that brings externally-referenced imports under a project `media/`
folder and rewrites the reference to the in-bundle relative path. To honour the
no-duplication rule it defaults to **move**, not copy (with a copy option when
the user wants the external original left in place — a deliberate choice, never a
silent standing duplicate). Recorded takes already land next to the `.qxp`, so
they need nothing. Delivers **use cases 2 and 3** for the solo/bundle user; the
NAS user already has them via the NAS itself.

### M2 — History / Compare / Restore UX (backend-agnostic)

A thin, opinionated binding so a musician never types `git`, written against the
`VersionBackend` interface (§C.5) so the same panel drives git *or* the NAS
snapshot store:

- A "History" panel: **Save Snapshot**, a timeline of versions, **Restore**, and
  **Compare** — a *semantic* diff over the now-stable UUIDs (which tracks / clips
  / params differ), not a raw text diff, so it works identically for both
  backends.
- **Git backend:** generate a `.gitignore` that excludes `media/` and the sidecar
  cache — media is never git-tracked (§C.1), so there is no `.gitattributes`/LFS
  step at all. A remote is optional; when set, Save Snapshot can push.
- **Filesystem backend:** the append-only snapshot store + the break-lock UI of
  §C.4; no git, no server.

Covers **use cases 1+2 for non-developers** (both topologies) and **use case 3**
(git remote, or the shared NAS). Nothing here reimplements VCS internals — the
git path shells out to git; the FS path writes files (cf. Cubase project
versions).

### M3 — Additive, non-real-time collaboration

- **Per-track file split**: serialize each track to its own file within the
  bundle so two people editing *different* tracks touch *different* files and git
  auto-merges cleanly. This is the real lever that turns "a musician adds a
  track" (**use case 4**) into a trivially additive commit.
- **Track-level locks via `LockService`** (§C.4/C.5) so concurrent edits to the
  *same* track are avoided rather than merged (the Perforce lesson). On the NAS
  this is the filesystem lock; for the git-remote user it is a lighter
  convention. Locking a track lets a studio musician (**use case 4**) take
  *their* track while everyone else keeps working and appending snapshots.
- **Export/import a single track + its stems** as the interchange unit for the
  offline studio-musician handoff (no shared storage at all).

Honest boundary: stable IDs (M0) make diffs readable and additive merges clean;
concurrent edits to the *same* track still need coordination or explicit conflict
surfacing — **not** auto-merge. The moment that stops being acceptable is the
moment the scoped-out real-time/OT server becomes the right tool. This proposal
stops cleanly at that line.

## E. Relationship to the merged relative-path work

The portable media-path work is **merged** (commit `f191e5d`, `SFilePathRef`) —
media serialization now routes through `SFilePathRef::toStored`/`fromStored`
(§A.2). The earlier sequencing worry (do the relative-path change first, then M0)
is therefore moot; what remains is one carry-over for whoever builds M0:

- **M0 touches the same serialize/loader path** that `SFilePathRef` plugged into
  (`main/objects/wave/src/splainwave.cpp`, `main/model/src/sobject.cpp`,
  `main/persistence/src/sprojectloader.cpp`, the `serialize` methods). M0's
  byte-stability acceptance test must therefore be written against the *current,
  post-`SFilePathRef`* on-disk format — a media reference is now a portable
  spelling, not a raw absolute path, so a "byte-identical round-trip" baseline
  captured before `f191e5d` would be wrong.
- The named-root indirection (§C.2) and M1's collect-media step both **extend**
  `SFilePathRef` (adding a form / a consolidation op) rather than replacing it,
  and should be co-designed with its `toStored`/`fromStored` rules so the
  precedence order (named-root → project-relative → home-relative → absolute)
  stays in one place.

## F. Options considered and rejected as the primary mechanism

- **git-LFS for media** — rejected on the hard no-duplication rule: two local
  copies by default, manual/unreliable dedup (§C.1). The reason media never
  enters git at all.
- **git-annex** — meets no-duplication (one copy + working-tree symlink) but is
  Windows-hostile; rejected for cross-platform simplicity (§C.1).
- **Dropbox/OneDrive on the live folder** — acceptable as dumb backup of a
  *closed* project (partially serves use case 2), but last-write-wins silently
  eats collaborators' edits, offers no semantic history for use case 1, and risks
  partial writes while qbx holds the folder open. Not a foundation. (Studios that
  use it for *sharing* are served better by the NAS snapshot backend, §C.4.)
- **Raw git in the user's face** — a valid backend under the M2 veneer, but
  hostile to a non-developer and worthless before M0; and a self-hosted/external
  git server is exactly what conservative studios won't run (Decision 3).
- **A bare git repo on the NAS as the default** — offered as an option (§C.4) but
  not default: git's ref updates are lock-fragile on NFS/SMB, which the
  append-only snapshot store avoids by construction.
- **Kitsu/Zou** — production tracking on top of a VCS, not file versioning;
  complementary, not the mechanism.
- **CRDT project document** — pulls toward the scoped-out real-time world;
  ordered-timeline CRDTs are hard. Rejected for this proposal.

## G. Open questions

1. `.qxp` single-file (M0–M2) vs. project-as-folder bundle (needed by M1's
   `media/`, the `.qbx/` snapshot+lock store, and M3's per-track split). Does M1
   introduce the folder form, and is the single-file `.qxp` retained as an
   export/"flatten" format?
2. Collect-media default: **move** vs. copy, and the UX when the external
   original must remain (a shared library the user does not own).
3. Named roots (§C.2): how are they defined, named, and persisted per machine
   (`SSettings`), and what is the UX when a root is unmapped on open (prompt to
   locate, like a DAW "missing files" dialog)?
4. Default backend per topology, and whether the optional bare-git-on-NAS backend
   is worth building at all or the snapshot store fully subsumes it.
5. Semantic Compare (M2): how much does it show — track/clip add/remove/move —
   and does it reuse any of the action-model vocabulary (`docs/ACTIONS.md`)?
6. Lock atomicity fallback on legacy NFS (pre-v3 `O_EXCL`); is create-then-rename
   plus heartbeat sufficient, and what is the worst-case on a broken lock?
7. Migration: how does an old pointer-ID `.qxp` acquire stable UUIDs on first
   load under M0 (assign-on-load, one-time rewrite)?
