# tw/graph — CONTRACT

Purpose: the component contract — the central seam of the whole system.
twComponent (five sub-contracts below), the latch plumbing between
components, twView (the position-translating clip wrapper), the format
negotiator, and tw303aEnvironment (sample rate / buffer size context).

Public headers: twcomponent.h, twlatch.h, twview.h, twnegotiator.h,
tw_freeze_context.h, tw_frozen_inputs.h, tw_page_plan.h, tw303aenv.h.

Depends on: tw/core, tw/pages. Forbidden: everything above (sources, dsp,
mix, ... — the graph defines the interface they implement).

The five sub-contracts of twComponent:
1. POSITION — isSeekable/seekTo/reset. Positions are in the component's OWN
   domain (POSITION_DOMAINS.md). seekTo is state-preserving; reset clears
   DSP state and position.
2. PULL — calcOutputTo(IOVector&, idx): advance the internal cursor,
   realtime path (target: no allocation, no Qt — see Known debt).
3. FREEZE — freezePage/freezePreviewPage + capture/restoreInternalState:
   random-access page rendering per FREEZE_PROTOCOL.md (normative).
4. TOPOLOGY — inputs/outputs/latches; UI thread only.
5. LIFECYCLE — teardown(): ZOMBIE state outputs silence, deregisters from
   parent, cascades to children; audio threads check state_ first.

Invariants:
1. twView::MapPosFn is the ONLY clip-relative→component translator; it runs
   BEFORE getComponent() (the mapping may lazily build the reader).
2. twLatchOutput is deleted through the base pointer — keep its virtual dtor.
3. freezePage never holds the component mutex while rendering.
4. tw303aEnvironment is the one QObject in the engine; nothing else in
   tw/ may inherit QObject (thread-adoption hardening — THREADING.md).
5. A page's `contentEpoch` may ONLY be compared against the counter of the
   component that STAMPED it. It is written by whichever component actually
   rendered the page, which is often NOT the one you got the page from: an
   insert-less twPluginChain forwards its upstream twTrackMix page verbatim,
   and twTrackMix self-bumps on every clip mutation, so its counter runs
   permanently ahead of the chain's. Comparing across the two is not merely
   imprecise — it is dead code that can never fire. A consumer that caches a
   page must therefore remember the epoch IT OBSERVED on the producer it asked
   (twLatchStreamingOutput::previousPageEpoch_) and re-validate on
   `observed != contentEpochNow()`. Getting this wrong meant a clip deleted
   from a track nested in a folder went on being played and metered forever.
6. A page served from twFrozenInputScope (a scheduler binding) is trusted for
   THAT render only and must never be recorded as observed-at-the-current-epoch:
   it carries no validation of its own (verify-at-publish is the scheduler's
   job), so blessing it would let a later, unbound call reuse it with nothing
   left to check. mix_test's "empty set falls back to the legacy pull" is the
   gate.

Threading: THREADING.md rules 2-3; one mutex per component, _nolock suffix
convention.

How to test: the full qxa suite exercises every sub-contract;
render_split_slip_offset.qxa is the MapPosFn regression.

Known debt: calcOutputTo default impl allocates per block; deprecated
raw-pointer calcOutputTo overload awaits removal; tw303a.cc (dead standalone
demo) parked in ../src/.
