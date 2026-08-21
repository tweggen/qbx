# tw/schedule — CONTRACT

Purpose: async capture revalidation — a worker pool + priority queue that
recomputes stale capture aspects (Preview/Playback/Metadata/Export) for
document objects, through the engine-owned IRevalidatable interface.

Public headers: capture_revalidator.h, revalidatable.h, capture_aspects.h.

Depends on: tw/core, tw/pages, tw/graph. Forbidden: app headers — the app's
SObject IMPLEMENTS IRevalidatable (thin delegations); the revalidator never
sees SObject.

Invariants:
1. Job processing: check revalNeeded under revalMutex → allocate/reuse
   nextPage → RECOMPUTE OUTSIDE all locks (10-100 ms) → swap pages + mark
   aspects under the lock (two-page model, THREADING.md rule 2).
2. Pool exhaustion re-queues at lowest priority — never blocks.
3. Priorities: Playback 10 > Metadata 5 > Export 2 > Preview 1.
4. twCaptureAspect bits (here) are NOT twRenderAspect bits (tw/pages) —
   different layouts, do not mix.
5. Workers are raw std::threads: no Qt anywhere downstream
   (invalidateAspects → scheduleRevalidation is fire-and-forget).
6. **Epoch comparisons are SAME-COUNTER only.** Content epochs are PER
   COMPONENT, so a value is meaningful only against the counter it was read
   from. A page's `contentEpoch` stamp is written by whichever component
   RENDERED it, which is not always the component that returned it — an
   insert-less `twPluginChain` forwards its `twTrackMix`'s page verbatim — so
   `page->contentEpoch < someOtherComponent->contentEpochNow()` is noise:
   whichever counter ran ahead decides it, permanently, in whichever direction.
   Verify-at-publish therefore compares `PageNode::observedEpoch` (that
   component's own epoch, read by `planPage`) against that same component's
   `contentEpochNow()`.
7. Verify-at-publish has TWO halves and they are not symmetric. Stale DEPS or
   an incomplete bound set ⇒ ONE bounded retry with re-frozen deps. An
   outdated own PLAN (`observedEpoch` moved: the dep set / clip resolutions
   are the pre-edit ones) ⇒ NO retry — the same plan would rebuild the same
   wrong structure. Publish the page anyway (proposal 16's RT stale-page
   fallback needs something to serve) and re-stale the position so the
   epoch-scoped readahead supersession re-demands and re-PLANS it.
8. Anything the scheduler invalidates itself bumps that component's epoch, and
   that bump must be re-observed into `observedEpoch` — a scheduler bump read
   back as an edit re-stales the page it just published, which the readahead
   re-demands, which retries and bumps again: a livelock, not a fix. For the
   same reason the self-stale invalidate is skipped unless a CURRENT cached
   page actually exists at that position (most per-track components cache
   nothing, and the epoch is shared by every page of the component).

9. **The RUN BARRIER IS NOT A SCHEDULER FEATURE** (proposal 37 P3c, design D4
   / 4.4). A render start and a play start invalidate the whole path from every
   INSTRUMENT track up to the root, from the run's start position onward, and
   clear the processor's `lastEnd_`. All of that happens in
   `SApplication::beginRun()`, on the MAIN thread, before the consumer issues
   its first demand — never here, and never from the readahead thread. Two
   reasons it cannot live in this module: the invalidation that consumers
   actually observe is the app-side `SObject` path walk (design F13 — a
   component-local `invalidatePagesInRange` cascades nowhere), and invariant 8
   above is exactly why a barrier issued from inside a running node would
   livelock. If a worker is mid-render at the barrier position when it lands,
   verify-at-publish self-staleness re-stales that result, so the wrong page is
   never published as current; the barrier is idempotent under any ordering and
   costs at most one re-render.

10. **retireComponentNodes(set) is NOT pause()** (proposal 21 L0, design 21
   §5). The live lane takes a track's processors out of the frozen graph, so
   the nodes already planned for those components must stop — and pause() is
   the wrong instrument for that: it drains ALL in-flight work, including
   import-time analysis jobs that run for tens of seconds, and it stops the
   graph everywhere, so a change concerning two components would hang the UI.
   The semantics are exactly: queued/waiting nodes of those components are
   DROPPED and never execute (their demands complete as NOT PRODUCED — a count
   on the handle, which a consumer treats like a miss: stale page or silence,
   never a wait); a RUNNING one is WAITED FOR, bounded by one page render,
   because its page already exists; the dedup entries are removed so a later
   demand PLANS FRESH; every other component's nodes are untouched. A dependent
   of a dropped node loses that edge, becomes runnable, and renders with the
   input unbound — i.e. it sees a MISS, which verify-at-publish already counts
   and the legacy fallback already covers for content.
   Two mechanical points that make the promise true rather than likely: a
   worker CLAIMS a node (state = Running) under the same queueLock_ that
   dequeued it — claiming it later, inside processGraphNode, leaves a window in
   which the node is in no queue and in no state a retirement can see, and it
   would then execute after the call returned — and the retirement holds
   expansionMutex_, so no expansion can add a node for a retiring component
   while it walks (design §5 says the exclusion wiring precedes the drain; this
   makes "should not arrive" into "cannot").

11. **shutdown() IS A TEARDOWN, NOT A DRAIN — and the OWNER quiesces before it
   destroys anything the pool can reach.** shutdown() drops all three queues,
   aborts every outstanding demand, and every worker leaves on the next turn of
   its loop WHATEVER STATE THE POOL IS IN; scheduleRevalidation() and
   scheduleAnalysisJob() then refuse, and scheduleRevalidation() refuses
   BEFORE taking its pin (a pin nothing will ever release keeps the app-side
   object alive forever — see revalRemoveRef's deleteLater re-arm). Two ways
   this was wrong at once, both observed as a hung app in ~SProject on
   File -> Open: the worker exit test required all three queues to be empty
   while shutdown() cleared only two of them, so a PAUSED pool with one
   leftover reval job span at 100 % CPU against a wait predicate that was
   already true and join() never returned; and the owner ran the whole
   destruction of its object graph BEFORE the pool's own destructor, handing
   live workers a reval lane full of dangling borrowed pointers. The reval
   lane's entries are borrowed and nothing else's are, which is why this lane
   is the one that must be dropped rather than drained. A dropped job is NOT
   unpinned, for retireObject()'s reason. Note the workers are adopted by Qt
   (the app posts a queued invokeMethod from revalCompleted), so a pointer
   freed under a worker tends to surface as a fault in Qt's per-thread
   teardown, nowhere near the object.

How to test: `ctest -R schedule_test` (retireObject and retireComponentNodes lifetime/retirement — including 100
randomized interleavings of a retirement against a running demand — the
dependency-
counting scheduler, and both directions of verify-at-publish self-staleness);
`SMARAGD_LOG_LEVEL=debug` prints the run's `nodesExecuted / nodeRetries /
missPages / selfStale` at shutdown, which is how a scheduler change is shown
not to multiply renders. Also exercised by every project load + edit.

Known debt: revalidationComplete UI signal still TODO (UI re-reads on next
paint); shutdown discards queued jobs (acceptable for background work — and
required, see invariant 11).
