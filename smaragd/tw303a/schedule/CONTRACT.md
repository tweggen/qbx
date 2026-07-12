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

How to test: exercised by every project load + edit (invalidation chains);
no dedicated unit test yet.

Known debt: revalidationComplete UI signal still TODO (UI re-reads on next
paint); shutdown discards queued jobs (acceptable for background work).
