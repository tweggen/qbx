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
7. A component's play cursor has exactly ONE writer at a time, and a freeze is
   that writer for the duration of its render. freezePage_nolock does
   reset()/restore → seekTo(page->startPosition) → renderFrames(), serialized on
   cursorMutex_ — but seekTo() itself takes only mutex(), a DIFFERENT lock. So a
   seek arriving from outside that freeze rewrites the cursor the render is
   about to read, and the whole 65536-frame page comes out as the audio of the
   SEEK TARGET while being cached under, and served for, its original startPos —
   valid, current-epoch and indistinguishable from correct. Therefore: code
   OUTSIDE the freeze machinery calls twComponent::seek(), which asserts the
   window is clear before dispatching; code INSIDE a freeze (freezePage_nolock,
   twTrackMix's clip walk, twView::seekTo, a plugin chain seeking its own taps)
   calls the virtual seekTo() directly. Every component that renders must mark
   its window with twComponent::FreezeInFlight — the base freezePage_nolock does
   it, and so must any subclass that overrides the whole freeze (twTrackMix).
   There are deliberately NO external seek cascades left: play start, record
   start, the SCut capture rebuild and the plugin chain's producer seek were all
   deleted rather than locked, because position is carried BY THE PAGE.

8. Page retention is stated in FRAMES. releaseOldPages(keepAfterPos) drops a
   page whose [startPos, startPos + FRAME_CAPACITY) ends before keepAfterPos.
   It compared against PAGE_SIZE — the page's size in BYTES — until proposal 36
   B1a, which made the window four times wider than the comment claimed. Note
   the second half of the finding: NOTHING IN THE TREE CALLS releaseOldPages.
   Component page caches are pruned only by invalidation and by teardown, so a
   long session's outputPages_ maps grow without bound; the fix was made
   because B1b multiplies a page's byte size by its channel count, at which
   point the same expression would have become width-dependent as well as
   wrong. Pinned frame-exactly by graph_test.
9. Every live twComponent is in a process-wide registry (proposal 36 B1a), for
   the per-component half of the page accounting. The registry holds RAW
   pointers and owns nothing; it is a leaked singleton so that a component
   destroyed after static destruction still has somewhere to deregister.
   componentPageStats()/describePageMemory() hold the registry lock for the
   WHOLE walk, and that is what makes the raw pointers safe: ~twComponent takes
   the same lock as its first act, so no component can get past the top of its
   base destructor while a walk is running, and the deleter frees the storage
   only after that destructor returns. Snapshotting the pointers and releasing
   the lock — the obvious alternative — would leave exactly the window in which
   a worker dropping the last reference to a reader turns a diagnostic into a
   use-after-free. The deadlock that arrangement would otherwise invite is
   closed by pageStatsTry()'s TRY-lock: the walk never waits for a component
   mutex while holding the registry lock, and reports how many it skipped.

10. PAGE WIDTH IS DECLARED BY getOutputChannels(), and by nothing else
    (proposal 36 §4.2, milestone B2). Default 1. Every page a component
    allocates for itself — freezePage's placeholder, getOrAllocatePage, the
    defused RT-guard page — is built at that width, so a component's declared
    width and its pages' width cannot disagree. It is NOT getNOutputs(), which
    is the patch-bay port count and already means three different things in
    three classes (twRewire's plugs are buses, twSampleReader's outputs are
    channels, twWavInput reports a hardcoded 4 with one latch built); the two
    must never be merged. twView forwards it to whatever it resolves to.
    twFormatCaps::channelCounts is DERIVED from it rather than restated, so it
    cannot drift — nothing reads that field and B9 deletes it.
11. A page WIDER THAN ONE CHANNEL is filled by renderPageWide(), never by a
    per-channel loop over renderFrames() (§4.3). freezePage_nolock forks on the
    width of the PAGE IN HAND — a declared width is a promise about future
    pages; the page you hold is a fact — and at width 1 takes byte-for-byte the
    pre-B2 call, which is what keeps the byte-exactness gate meaningful. The
    loop is forbidden because a cursor-bearing component (twSampleReader:
    `pos_ += dest.length()`) would advance a whole page per channel and fill
    channel 1 with the NEXT page's audio, and because internalState is captured
    once, so a state chain would be meaningful for channel 0 only. A component
    declaring width > 1 that does not override renderPageWide() gets the base
    implementation, which REFUSES — reports once, fills silence, returns 0
    frames, and counts the refusal (twComponent::wideRenderRefusals()). It is
    deliberately not a Q_ASSERT: those are compiled out of the build everyone
    runs. A wide component should still implement renderFrames() as its narrow
    degradation, for the legacy mono-scratch paths no fork can widen.
12. A PLUG PULL YIELDS CHANNEL min(latchIndex, page->channels - 1) of the page
    the producer actually froze (§4.4 rule 1), in twStreamingLatch::copyData —
    for the bound-page branch and the legacy-pull branch alike. The latch has
    carried that index since it was written and never consulted it; giving it
    its channel meaning is what makes twSampleReader's per-channel latches real
    instead of dead. The width acted on is always page->channels(), never the
    producer's declared width (pages are laundered between components — an
    insert-less twPluginChain forwards its twTrackMix page verbatim and its
    silence pages are width 1). A wide consumer may instead read its bound
    input PAGES directly and pick channels itself (rule 2), which is why no
    channel argument is threaded through readStreamingData/copyData.

Threading: THREADING.md rules 2-3; one mutex per component, _nolock suffix
convention.

How to test: the full qxa suite exercises every sub-contract;
render_split_slip_offset.qxa is the MapPosFn regression; `ctest -R graph_test`
pins invariants 8-9 (links only tw_graph). Invariant 7 has a
dedicated stress case in playback_test ("seek storm"): four threads freezing
position-coded pages while a fifth hammers AudioEngine::seekTo, then every
produced page is decoded and must carry the audio of its own startPosition.

Known debt: calcOutputTo default impl allocates per block; deprecated
raw-pointer calcOutputTo overload awaits removal; tw303a.cc (dead standalone
demo) parked in ../src/.
