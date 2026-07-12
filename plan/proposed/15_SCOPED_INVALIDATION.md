# Proposal 15: Scoped page invalidation (per-component content epochs)

> **Status: EXECUTED 2026-07-12** — see `plan/STATE.md`. One deviation from the
> design below: `SStdMixer::bumpRenderChainEpoch()` must bump the per-bus
> `twMixer`s as well as the rewire — the summed mix is cached at both levels
> (found via a stale-render failure of the sibling-tracks regression).

## Status quo (after the 2026-07 content-epoch fix, commit c3268a5)

Frozen-page staleness is governed by ONE global counter,
`tw303aEnvironment::contentEpoch()`. Every page is stamped with the epoch it
was rendered at; every consumer (base `freezePage` cache check, streaming-latch
held page and state-chain predecessor, `AudioEngine::updateFrozenPage`, the
readahead loop) treats `page->contentEpoch < env.contentEpoch()` as stale.
Any edit — clip insert/remove/update, track mute/gain, `setInput` rewiring —
bumps the global counter.

Consequence: **moving a clip on track A also stales every cached page of
track B** — B's content readers, plugin inserts, and rewire all re-render on
the next freeze, even though their audio is bit-identical. The root mix pages
must of course re-render (they contain the sum), but sibling-track caches are
collateral damage.

This was a deliberate trade-off, not a requirement:

- **Correctness first.** The bug being fixed was "playback after edit plays the
  old arrangement". A global epoch is impossible to get wrong: there is no
  graph to traverse, so there is no path that can be missed.
- **No dependency-graph lifetime problem.** Scoped invalidation needs
  producer→consumer edges. The engine's `dependents_` vector exists but was
  never wired, and wiring it means raw consumer pointers stored on producers —
  every teardown path (there are per-class copies of `teardown()`) would have
  to deregister, or the invalidation cascade dereferences freed components.
  The clip hop (child-track rewire → parent `twTrackMix`) crosses a `twView`
  whose underlying component is resolved lazily and can change identity,
  making registration-time wiring fragile.
- **The cost is bounded.** Re-freezing is demand-driven; only pages in the
  readahead window / render cursor actually re-render, at ~1-5 ms per 65536-
  frame page per component. For small projects the waste is unmeasurable.

The cost DOES matter for wide/heavy projects: with N tracks, one clip move
forces N-1 tracks' full pull chains (including expensive grain-stretch
readers) to re-render inside the readahead window, where scoped invalidation
would re-render only the edited track's chain plus a cheap re-mix (memcpy +
sum) at each ancestor.

## Correct scope of an edit

An edit on track T must invalidate exactly the **path from T to the root**:

    T.trackmix → T.pluginInserts/chain → T.rewire
      → parent.trackmix → parent.chain → parent.rewire
      → … → root.rewire  (= AudioEngine's synthOutput_)

Not invalidated: sibling tracks' readers/chains/rewires, and T's own content
readers (content pages are keyed by source position and survive move/split by
design; stretch rebuilds the reader, which retires its cache wholesale).

## Design: per-component epoch, app-driven propagation

Replace the single `tw303aEnvironment` counter with an atomic epoch **per
`twComponent`**; keep every staleness check exactly where it is today, but
compare against the *producing component's* epoch instead of the global one.
Every check site already knows its producer, so no check needs new plumbing:

| Check site                                   | Producer it already knows      |
|----------------------------------------------|--------------------------------|
| `twComponent::freezePage` cache check        | `this`                         |
| `freezePage_nolock` contiguity (state chain) | `this` (prev page is its own)  |
| `twStreamingLatch::copyData` held/chainFrom  | `getComponent()`               |
| `AudioEngine::updateFrozenPage` / readahead  | `synthOutput_` (root)          |

Stamping is unchanged in shape: base `freezePage`, `twTrackMix::freezePage`,
and `twPluginInsert::freezePage` stamp with their own epoch read before
rendering.

**Propagation is the app's job.** Proposal-14 layering already gives the app
full knowledge of the path to root, with owned (not borrowed) pointers:

- `STrack` owns `cpTrackMixers_[i]`, `cpPluginChains_[i]`, `cpRewire_`.
- The parent chain is the `SObject` tree, ending at the root mixer.

Add `twComponent::bumpContentEpoch()` (self only; `twPluginChain` overrides to
forward to its `plugins_`, since insert pages bake in upstream audio), and in
the app one helper:

```cpp
void STrack::invalidateRenderChain()   // called from trackChildWasMoved,
{                                      // trackChildDurationChanged,
    for (int i = 0; i < nBusses_; ++i) {          // WasAdded/WasRemoved,
        if (cpTrackMixers_[i])  cpTrackMixers_[i]->bumpContentEpoch();
        if (cpPluginChains_[i]) cpPluginChains_[i]->bumpContentEpoch();
    }
    if (cpRewire_) cpRewire_->bumpContentEpoch();
    if (auto *p = parentTrack()) p->invalidateRenderChain();   // up to root
}
```

Engine-level bump sites keep only their local scope: `twTrackMix` clip/mute/
gain methods bump `this` (self-knowledge: "my output changed"); `setInput`
bumps the consumer. Engine-only users (tests) propagate themselves; the app
owns the timeline, so the app owns the path.

This avoids the `dependents_` swamp entirely: no raw cross-component pointers,
no deregistration protocol, no view-hop discovery — the `SObject` tree IS the
path to root, and `STrack` outlives the components it bumps.

### Edge cases

- **Plugin add/remove:** `STrack::onPluginSlotInserted/…Removed` call
  `invalidateRenderChain()` after `rebuildWiring()` (setInput's consumer-local
  bump covers the inserts; the chain/rewire/ancestors need the app walk).
- **Mixer-level edits** (track volume on `SStdMixer`, track add/remove/move):
  bump from the affected lane's `STrack` (or the root directly).
- **Sample-rate change / project reload:** rebuilds the graph; fresh
  components start at epoch 1 with empty caches — nothing to do.
- **`releaseOldPages` / memory:** unchanged; stale pages are replaced on
  demand exactly as today.

### What guards against a missed path?

The global epoch's one real virtue is unmissability. Two mitigations:

1. Keep the qxa regression (`render_after_edit_stale_cache.qxa`) and add
   variants that edit a NESTED track and a sibling-heavy project.
2. Add a mix-module unit test that freezes two sibling rewires, edits one
   track, and asserts (a) the edited path's pages are re-rendered (pointer or
   epoch changed) and (b) the sibling's cached page object is served untouched
   — the scoping property itself, not just audibility.

Optionally keep the env-global counter as a debug assert: in dev builds, a
component whose page passes the per-component check but predates the last
global bump could log once, catching forgotten propagation paths.

## Effort / risk

- Engine: mechanical — move the counter, retarget ~6 comparison sites,
  `twPluginChain::bumpContentEpoch` forwarder. Low risk; all sites were
  centralized by the c3268a5 fix.
- App: `STrack::invalidateRenderChain()` + calls from the four child-event
  slots and the plugin slots; root-mixer edits. Needs a `parentTrack()`
  accessor (SLink walk).
- Tests: 1 unit test (mix module), 1-2 qxa variants.

Estimated: half a day including tests. Payoff scales with track count and
reader cost (granular clips); zero behavior change for single-track projects.
