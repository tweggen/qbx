# tw/graph — CONTRACT

Purpose: the component contract — the central seam of the whole system.
twComponent (five sub-contracts below), the latch plumbing between
components, twView (the position-translating clip wrapper), the format
negotiator, and tw303aEnvironment (sample rate / buffer size context).

Public headers: twcomponent.h, twlatch.h, twview.h, twnegotiator.h,
tw_freeze_context.h, tw303aenv.h.

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

Threading: THREADING.md rules 2-3; one mutex per component, _nolock suffix
convention.

How to test: the full qxa suite exercises every sub-contract;
render_split_slip_offset.qxa is the MapPosFn regression.

Known debt: calcOutputTo default impl allocates per block; deprecated
raw-pointer calcOutputTo overload awaits removal; tw303a.cc (dead standalone
demo) parked in ../src/.
