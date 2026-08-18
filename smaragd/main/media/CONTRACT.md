# app/media — CONTRACT

Purpose: the media SOURCE layer of the media browser (proposal 38). The
addressable identity of a media item (`SMediaRef` / `SMediaEntry`), the
category → suffix table, the abstract `SMediaSource` ABI every provider
answers through, the id → source registry, the local file-system provider,
and (gate 4) the Nextcloud/WebDAV connector. Later gates add the cache and
the drop helper here; the dock is a DIFFERENT module (`main/mediabrowser`,
app_ui).

Public headers: app/media/{smediaref,smediatypes,smediasource,smediaregistry,
slocalmediasource,swebdavclient,swebdavmediasource,smediacredentials}.h

**`smediacredentials.h` (GATE 5b) is the credential SEAM, header-only and
deliberately so.** It declares `smedia::CredentialProvider` ("give me the
Authorization header value for this source id") and a process-wide
install/get pair, implemented entirely as `inline` functions over a
function-local static — proposal 38 §C restricts this gate to "adding the
credentials header" under `main/media/src`, not a new translation unit, and a
`.cpp` here would need `SSecretStore` reachable from a module that must never
see it (`main/shell`'s `SMediaAccountManager` is the one and only
implementation, installed at `SApplication` construction). No production code
in THIS module calls `credentialProvider()` — `SWebDavMediaSource`'s
Authorization header is computed by its CALLER and handed to its constructor
(§B.8a) — the seam exists for code that only knows a source's id, which as of
gate 5b is nothing yet.

**Gate 4's `SWebDavClient`/`SWebDavMediaSource` are no longer UNIT-TEST
ONLY for the ACCOUNT half.** GATE 5b (`main/shell`) gives them a real caller:
`SMediaAccountManager` constructs a live `SWebDavMediaSource` per persisted
Nextcloud account and registers it with `SMediaRegistry`, so
`main/media/tests/webdav_source_test.cpp` is no longer the only thing that
has ever driven this code — `qxa.media_options_page` and
`qxa.media_secret_redaction` (`main/testkit`) now do too, end to end against
the in-repo stub. What is STILL missing is the DOCK actually browsing one:
gate 5c wires `SMediaBrowserPanel` up to a real account and adds the qxa
cases for it.

Depends on (engine): tw/core, tw/graph (the base every app module gets).
`tw/core/twlog.h` is the only one actually used. It must NEVER grow an edge to
another engine module — a provider that knows what a page is has started doing
the engine's job.

App edges: `model` only, and today not even that in code — the dependency is
declared because gate 3's cache reasons about `SFilePathRef`'s project-relative
spelling. It must never grow an edge to `objects/*`: a provider that knows what
an `SCut` is has started doing the dock's job.

Qt edges: `Qt::Network` (gate 4's `QNetworkAccessManager`, the first HTTP
client in this tree) and `Qt::Concurrent` (the PROPFIND XML parse, off the
GUI thread) arrive linked on the `app_core` LAYER rather than the module —
see `main/CMakeLists.txt`'s comment beside `target_link_libraries(app_core
...)`. `Qt::Xml` (`QDomDocument`, the PROPFIND parser) needs no new link line:
it is already PUBLIC on `app_model` and propagates up.

Threading: every request is answered OFF the main thread and delivered ON it.
`SLocalMediaSource` walks on `smedia::mediaThreadPool()` — a private, bounded
(2-thread) `QThreadPool`. `SWebDavMediaSource`/`SWebDavClient` need no such
pool: `QNetworkAccessManager` is async by construction, and the PROPFIND XML
parse runs via `QtConcurrent::run` + a `QFutureWatcher` owned by the client,
which is the standard Qt pattern for "deliver on the thread that owns the
watcher" and carries none of `SLocalMediaSource`'s hand-back hazard (T16) —
there is no raw pointer for a worker thread to dereference; the free parse
function takes everything it needs BY VALUE and never touches the client.
No `std::thread` anywhere in this module, ever: proposal 19's rule that a
non-Qt thread must never emit a Qt signal (Qt adopts the thread and the
teardown join deadlocks) applies here exactly as it does in the engine.

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

## How gate 4 discharges the same invariants for WebDAV, and what it had to add

`SWebDavMediaSource` is the second `SMediaSource` implementation, so §B.2's six
invariants apply to it verbatim; the mechanism differs because there is no
walker thread to hand results back from.

- **inv. 1 (async, id-tagged).** `QNetworkAccessManager` is async by
  construction, so there is no pool to manage. The one thing that DOES need to
  leave the main thread is the PROPFIND XML parse (a directory of 10 000
  entries is a megabyte of XML, §B.7): `SWebDavClient::finishPropfind` hands the
  bytes to `QtConcurrent::run` and a `QFutureWatcher` owned by the client
  delivers the parsed `QVector<Entry>` back on the client's own thread when
  `finished()` fires. This carries NONE of `SLocalMediaSource`'s hand-back
  hazard (T16): the free parse function takes the XML bytes and two decoded
  path strings BY VALUE and never touches the client, so there is no pointer
  for the worker thread to dereference, and the watcher is parented to the
  client and torn down (not leaked, not raced) if the client dies mid-parse.
- **inv. 2 (supersession by id).** `SWebDavMediaSource` keeps its own logical
  request-id space (`SMediaSource::nextRequestId()`) SEPARATE from
  `SWebDavClient`'s own PROPFIND/GET ids, translated through
  `propfindOwner_`/`getOwner_`. A result for a client id no longer in `live_`
  (cancelled, or the owning search already finished) is dropped before it ever
  reaches a signal emission.
- **inv. 3 (streaming).** `listDirectory()` and `search()` share ONE
  accumulator shape (`SearchState`/`appendToSearch`/`flushSearch`) with the
  same `kBatchEntries`/`kBatchMs` bounds as the local walker (declared once, in
  `slocalmediasource.h`, and reused here) -- `listDirectory` is modelled as a
  `search()` with `recursive=false` and a one-entry starting queue, which is
  its actual shape (one PROPFIND, no needle, dirs never filtered).
- **inv. 4 (announced truncation).** `kMaxSearchEntries`/`kMaxSearchDepth` are
  the SAME constants the local walker uses; `kMaxInFlightPropfinds = 4` is
  WebDAV's own bound (§B.7) and lives beside them for the same reason inv. 4
  names all three together. A bounded search logs one `TW_LOGW( "media", … )`
  naming every cap, exactly like the local walker's line.
- **inv. 5.** `SWebDavMediaSource`/`SWebDavClient` touch nothing under
  `app/objects/…` or `app/actions/…`.
- **inv. 6 (cancel is advisory).** `SWebDavClient::cancel()` `abort()`s the
  reply if one is in flight; a PROPFIND whose network round trip already
  finished and whose XML parse is still running on the concurrent pool is NOT
  interrupted -- the result still arrives, tagged with the same id, and
  `SWebDavMediaSource` drops it because the id is no longer in `live_`. Same
  two-layer shape as the local walker's atomic flag plus id check.

Decisions gate 4 had to make that §B.7 did not spell out:

- **`SWebDavClient::urlFor()` passes the DECODED path to `QUrl::setPath()`.**
  An earlier version percent-encoded each segment itself before calling
  `setPath()` -- and `setPath()` percent-encodes ITS OWN input, so a
  pre-encoded `%20` became `%2520` on the wire. A directory name with a space
  in it 404'd silently. `SMediaRef::path` is documented as "never
  percent-encoded" for exactly this reason: exactly one encoder should ever
  touch a path, and it should be the one that owns the wire format.
- **`getlastmodified` needs a fallback parse.** RFC 1123 (what HTTP/WebDAV
  actually send) always spells the zone as the literal `GMT`; `Qt::RFC2822Date`
  parses a numeric offset but rejects that literal outright. The parser tries
  the strict form first and, only if that fails and the string ends in `GMT`,
  substitutes `+0000` and retries -- it never guesses at a value Qt itself
  rejected.
- **The PROPFIND parser matches DOM elements by LOCAL NAME**, stripping any
  `prefix:` before comparing, rather than trusting a specific namespace prefix
  or requiring `QDomDocument` namespace processing to agree with a
  possibly-absent `xmlns` declaration. Real servers spell the DAV namespace
  prefix differently (`d:`, `D:`, none at all) and this is the commonest
  real-world WebDAV interop break; gate 4's own test re-runs the same listing
  under three prefix spellings against the in-repo stub.
- **A missing optional PROPFIND property is not a dropped entry.** `Entry`'s
  own defaults (`sizeBytes = -1`, an invalid `QDateTime`, an empty `etag`) ARE
  the "unknown" answer, so the parser only WRITES a field when the XML has it
  -- no entry is ever skipped for lacking one.
- **The request's own collection is excluded by comparing the response's own
  `href`** (decoded and slash-normalised) against the href the REPLY's own
  request URL resolves to -- recovered from `reply->url()` rather than
  threaded through as a second argument, so a future refactor cannot let the
  two drift out of step.
- **`SWebDavMediaSource::fetch()` and `listDirectory()`/`search()` never
  block each other**, and neither locks anything: everything here runs on the
  main thread, so the only serialisation that matters is "does a signal handler
  see state that some earlier handler already finished mutating", which the
  `searches_`/`live_` bookkeeping enforces the same way the local source's
  `live_` hash does.
- **`SWebDavMediaSource::caps()` includes `NeedsFetch`** (a remote file must
  be downloaded before it can be placed) but not `CanStream`, for the same
  reason the local provider has neither: the MVP places files by a fetched
  local path.

## Knobs

None yet. Gate 3 adds `SMARAGD_MEDIA_CACHE_DIR`.

## How to test

`ctest -R media_source_test` (gate 1, local provider) and
`ctest -R webdav_source_test` (gate 4, WebDAV connector). The local fixture
tree is committed at `smaragd/tests/media/` and its shape is what makes every
count a closed form:

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

`webdav_source_test` builds every fixture PROGRAMMATICALLY against
`SWebDavStub` (`main/testkit/src/swebdavstub.{h,cpp}`) rather than a committed
tree, because the shapes it needs -- a directory with a space and a `#` in a
name, a wide 8-subdirectory tree for the concurrency-cap assertion, a
5000+-entry directory for the truncation assertion -- are cheaper to build at
run time than to commit, and the stub binds `127.0.0.1:0` (an OS-assigned
port), which is what keeps it safe under `ctest -j4`.

## Known debt

- `cancel()` cannot interrupt a walker blocked inside one `stat()` (inv. 1,
  local provider only -- `SWebDavClient::cancel()` has no equivalent gap, it
  `abort()`s a real `QNetworkReply`).
- `truncatedCount` is a lower bound (inv. 4).
- No provider is registered anywhere yet — gate 2 mounts the dock and gate 5
  the accounts. `SMediaRegistry` is exercised only by `media_source_test`.
- The `kMaxSearchEntries` / `kMaxSearchDepth` truncation paths are NOT gated by
  gate 1's local provider: reaching either needs 5000 files or a 13-deep tree
  built at run time, and neither is worth the seconds it costs there. Gate 4
  DOES gate `kMaxSearchEntries` for WebDAV (a single big PROPFIND response is
  cheap to build), but not `kMaxSearchDepth` -- a 13-deep stub tree needs 13
  real PROPFIND round trips per branch and was judged not worth the seconds
  either. The batching, cancel, supersession and mid-walk-destruction paths ARE
  gated for both providers.
- **`SWebDavClient`/`SWebDavMediaSource` are UNIT-TEST ONLY (gate 4).** No
  account, no credential, no UI, no qxa case has ever driven them -- see the
  module-level note near the top of this file and gate 4's PR body. Gate 5c
  is the only thing that closes this.
- Not gated by gate 4, named plainly rather than implied: a real Nextcloud
  server (TLS, real app-password auth, redirects, rate limiting -- manual
  runbook only, §C.6 of the proposal), network physics (throughput, latency, a
  flaky link), a resumed/partial transfer (not implemented at all), and a very
  large download's memory behaviour (the stub serves fixtures up to ~200 KB;
  nothing here streams to disk in a way that would behave differently at
  100 MB, but that claim itself is untested).
