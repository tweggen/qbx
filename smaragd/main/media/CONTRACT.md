# app/media — CONTRACT

Purpose: the media SOURCE layer of the media browser (proposal 38). The
addressable identity of a media item (`SMediaRef` / `SMediaEntry`), the
category → suffix table, the abstract `SMediaSource` ABI every provider
answers through, the id → source registry, and the local file-system
provider. Later gates add the cache, the drop helper and the WebDAV client
here; the dock is a DIFFERENT module (`main/mediabrowser`, app_ui).

Public headers: app/media/{smediaref,smediatypes,smediasource,smediaregistry,
slocalmediasource}.h

Depends on (engine): tw/core, tw/graph (the base every app module gets).
`tw/core/twlog.h` is the only one actually used. It must NEVER grow an edge to
another engine module — a provider that knows what a page is has started doing
the engine's job.

App edges: `model` only, and today not even that in code — the dependency is
declared because gate 3's cache reasons about `SFilePathRef`'s project-relative
spelling. It must never grow an edge to `objects/*`: a provider that knows what
an `SCut` is has started doing the dock's job.

Threading: every request is answered OFF the main thread and delivered ON it.
`SLocalMediaSource` walks on `smedia::mediaThreadPool()` — a private, bounded
(2-thread) `QThreadPool`. No `std::thread` anywhere in this module, ever:
proposal 19's rule that a non-Qt thread must never emit a Qt signal (Qt adopts
the thread and the teardown join deadlocks) applies here exactly as it does in
the engine.

**NO WIDGET, EVER.** `#include <QWidget|QTreeWidget|QDialog|QMenu|QPainter|
QPixmap>` may not appear anywhere under `main/media/`. The compiler will NOT
catch a violation — `app_model` links `Qt::Widgets` PUBLIC, so every widget
header is reachable from every layer — so this is a contract plus a grep
(gate 1 AC 11).

## Invariants (proposal 38 §B.2 inv. 1-6, verbatim)

1. **Everything is async and id-tagged.** A recursive walk of a sample library
   is seconds of work; a `PROPFIND` is a network round trip. Neither may run on
   the GUI thread. `SLocalMediaSource` walks on a **private, bounded**
   `QThreadPool`; `SWebDavMediaSource` uses `QNetworkAccessManager`, which is
   async by construction. **No `std::thread` anywhere in this module**:
   proposal 19's rule that a non-Qt thread must never emit a Qt signal (Qt
   adopts the thread and the teardown join deadlocks) applies here exactly as
   it does in the engine.

   **THE CROSS-THREAD HAND-BACK IS A WORKER `QObject` WITH A SIGNAL, NEVER
   `QMetaObject::invokeMethod` ON A RAW POINTER.** An earlier draft specified
   the latter and it is a use-after-free: the pool thread dereferences the
   source to post the event, and a source can be deleted at any moment — gate 5
   AC 11 deletes one on account removal, and teardown deletes them all. A
   `QPointer` does not fix it either; `QPointer` is only safe to read on the
   thread that owns the object, so T3's guard covers handlers already ON the
   main thread and nothing else. The walk therefore lives in a `QObject` whose
   `batchReady` signal is connected to the source with `Qt::QueuedConnection`,
   because **Qt tears connections down under its own mutex when the receiver
   dies** — a queued emission to a destroyed receiver is dropped, not crashed.
   The worker is owned by the pool task and `deleteLater`s itself. Gate 1 AC 12
   destroys a source mid-walk and is the only thing that will catch a subagent
   improvising the raw-pointer version, because the safe and unsafe forms are
   indistinguishable on a machine that never loses the race.

   **A private pool, not `QThreadPool::globalInstance()`, and a named hang.**
   Two threads, dedicated to this module. `cancel()` is checked between
   entries, so a walker blocked inside a single `stat()` — a dead network mount,
   a spun-down drive — **never sees it and pins its thread until the OS
   returns**. That is an accepted limitation, stated because it cannot be
   designed away without an out-of-process walker; what the private pool buys
   is that it cannot starve `QtConcurrent` or anything else Qt runs on the
   global pool.

2. **Supersession is by ID, never by a flag.** The view keeps the id of the
   request it is displaying and **drops any batch whose id is not that one**.
   A late result from a cancelled walk therefore cannot repaint over a newer
   one — by construction, not by a race the cancel is supposed to win. Same
   shape as proposal 21's "superseded by replacing the handle".

3. **Results STREAM.** `entriesReady` is called with batches (every ~200
   entries or ~100 ms, whichever first) with `final=false`, then once with
   `final=true`. That is what makes the search *incremental* in the sense the
   user asked for: a recursive walk shows its first hits immediately.

4. **A truncation is ANNOUNCED, never silent.** `truncatedCount > 0` on the
   final batch means the walk hit its bound (`kMaxSearchEntries = 5000`,
   `kMaxSearchDepth = 12`, `kMaxInFlightPropfinds = 4`) and the view must say
   so in a footer line. The repo's own rule: "if a workflow bounds coverage,
   log what was dropped — silent truncation reads as *covered everything*".

5. **A provider never touches the model and never submits an action.** It
   returns entries and local paths. Placement is the dock's and the drop
   handler's business.

6. **`cancel()` is advisory and results after it must be safely droppable.**
   The walker checks an atomic flag between entries; a reply already in flight
   is `abort()`ed and its finish handler must tolerate being called anyway.

## How gate 1 discharges each of them, and the details the design left open

- **inv. 1.** `SMediaWalkTask` (a `QRunnable`, `src/smedialocalwalker.h`) holds
  **no pointer to the source** — it cannot touch a destroyed one even in
  principle. It owns an `SMediaWalkWorker` created on the MAIN thread with **no
  parent** (parenting it to the source would destroy it under the pool thread's
  feet) and `deleteLater()`s it from `~SMediaWalkTask`. Cancellation reaches
  the task through a `shared_ptr<std::atomic_bool>` that outlives the source.
  `smedia::mediaThreadPool()` is a function-local static `QThreadPool` with
  `maxThreadCount == 2`; it is an OBJECT and not a leaked pointer so that
  `~QThreadPool` joins a walk still running at process exit.
- **inv. 2/6.** `SLocalMediaSource::onWorkerBatch` drops any batch whose request
  id is no longer live. `cancel()` clears the id and sets the flag; `cancel()`
  on an unknown id is a no-op. A cancelled request therefore never sees
  `final=true` — completion and cancellation are different endings.
- **inv. 3.** `kBatchEntries = 200`, `kBatchMs = 100`, flushed by whichever
  comes first, then exactly one `final=true`.
- **inv. 4.** `truncatedCount` is a **LOWER BOUND** on what was not delivered,
  never an exact tally: the walk stops at the cap, so what lies beyond it is by
  definition uncounted. Non-zero means "incomplete", which is the whole claim.
  A bounded search also logs one `TW_LOGW( "media", … )` naming both caps.
- **inv. 5.** Nothing here includes `app/objects/…` or `app/actions/…`.

Decisions gate 1 had to make that §B.2 did not spell out:

- **The suffix filter is source STATE** (`SMediaSource::setSuffixFilter`), not a
  `listDirectory()` parameter, because the dock has ONE filter control
  governing both modes and a lazily-expanded tree issues a `listDirectory()`
  per expanded row with no caller in a position to re-supply it. `search()`
  keeps its explicit `suffixes` argument: a search request is a snapshot of the
  whole query, so a filter change mid-walk cannot make a result disagree with
  the request that asked for it.
- **The filter applies to FILES only.** Directories are always listed — a user
  must be able to navigate past a filter into a folder it hides.
- **A search reports FILES only.** A directory is a container to walk, not a
  result; §B.4's search mode is a flat result list.
- **A search matches the entry's WHOLE NAME**, extension included, case
  insensitively. That is what a user typing in a search box means, and it is
  why the needle `"a"` matches every `.wav` in the fixture.
- **A directory's `sizeBytes` is -1** ("unknown"), never `QFileInfo::size()`,
  which is platform junk and would make an exact-count gate disagree between
  Windows and Linux.
- **Symlinks are listed but never followed** (§B.4). It bounds a loop without
  relying on `kMaxSearchDepth`.
- **The registry does not OWN its sources.** It connects to
  `QObject::destroyed` and forgets one that dies — the only arrangement under
  which `source(id)` stays honest when gate 5 removes an account, and the only
  one gate 1 AC 12 can be written against.
- **`SMediaRef`'s URI is not a `QUrl`.** A source id may contain a colon
  (`nextcloud:<accountId>`), which `QUrl` reads as a port, and the path is
  stored DECODED. The format is `smedia://<sourceId>/<path>` split at the first
  `/` after the authority, which leaves `C:/samples/kick.wav` intact.
- **`SLocalMediaSource::caps()` omits `NeedsFetch`.** A local file is already
  local; `fetch()` completes on the next main-thread turn with the path it was
  given, so a caller never has to special-case which source it is talking to.

## Knobs

None yet. Gate 3 adds `SMARAGD_MEDIA_CACHE_DIR`.

## How to test

`ctest -R media_source_test`. The fixture tree is committed at
`smaragd/tests/media/` and its shape is what makes every count a closed form:

```
tests/media/lib/kick.wav          768044 bytes (a copy of test_sawtooth.wav)
tests/media/lib/snare.wav         768044
tests/media/lib/notes.txt         the thing the Audio filter must omit
tests/media/lib/sub/hat.wav       768044
tests/media/lib/sub/deep/tom.wav  768044
```

Four wavs in total, two of them directly in `lib/`. The batching, cancel and
mid-walk-destruction cases build a LARGER tree in a `QTemporaryDir` at run
time, because a five-entry fixture cannot produce more than one 200-entry
batch and the real batching rule is the thing worth gating.

## Known debt

- `cancel()` cannot interrupt a walker blocked inside one `stat()` (inv. 1).
- `truncatedCount` is a lower bound (inv. 4).
- No provider is registered anywhere yet — gate 2 mounts the dock and gate 5
  the accounts. `SMediaRegistry` is exercised only by `media_source_test`.
- The `kMaxSearchEntries` / `kMaxSearchDepth` truncation paths are NOT gated by
  gate 1: reaching either needs 5000 files or a 13-deep tree built at run time,
  and neither is worth the seconds it costs. The batching, cancel, supersession
  and mid-walk-destruction paths ARE gated, over a temporary tree.
