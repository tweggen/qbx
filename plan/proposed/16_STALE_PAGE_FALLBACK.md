# Proposal 16: Stale-but-consistent page fallback during live playback

> **Status: EXECUTED 2026-07-13** — see `plan/STATE.md`. Implemented as
> designed.

## Problem

Proposal 15 gave us scoped, correct invalidation: an edit bumps the content
epoch of every cache on the edited path to the root, and stale pages are never
served as *current*. But the audio thread's page adoption
(`AudioEngine::updateFrozenPage`) treats "stale" the same as "missing":

1. Edit lands mid-playback → root `synthOutput_`'s epoch bumps.
2. Audio thread notices `heldPage->contentEpoch < epochNow`, **drops** the held
   page, finds only the (equally stale) cached page at that position, rejects
   it → `currentFrozenPage_ = nullptr` → **silence**.
3. The readahead thread re-freezes the playhead page (milliseconds to hundreds
   of milliseconds for deep/grain-heavy chains) → sound resumes with the new
   content.

For an Ableton-grade editing feel, step 2 is wrong: the pre-edit page is a
perfectly consistent waveform. Hearing the *old* arrangement for a few more
milliseconds is strictly better than a dropout; the moment the re-frozen page
lands, the edit becomes audible anyway.

## Non-goals / safety

- **Offline renders stay exact.** `RenderSession` pulls via
  `synthOutput_->freezePage(...)` directly (synchronous, epoch-checked, always
  fresh) and never goes through `updateFrozenPage`. The fallback is confined
  to the realtime adoption path. Playback start (`seekTo`) also resets the
  readahead frontier, so a fresh playback never begins on fallback pages.
- **Staleness bookkeeping is unchanged.** Pages are still *marked* stale
  exactly as in proposal 15; the readahead still re-freezes them. Only the
  audio thread's "what do I play while that happens" answer changes.
- **Generation mismatches still drop hard.** A generation bump means the page
  object was invalidated/repurposed (`invalidateAllPages`) — its buffer cannot
  be trusted at all, fallback included.

## Design

### 1. Audio thread: prefer fresh, fall back to stale, silence only last

`AudioEngine::updateFrozenPage(desiredPos)` becomes a preference ladder:

1. **Fresh held page** covering the position → fast path (unchanged).
2. **Fresh cached page** at the position (lock-free `getPageIfExists`) →
   adopt. This is also the moment an edit becomes audible when we were riding
   a fallback: adoption is re-attempted on every batch because a stale held
   page no longer satisfies the fast path.
3. **Stale held page** still covering the position → keep playing it, poke the
   readahead CV so the re-freeze hurries.
4. **Stale cached page** at the position → adopt it as fallback. Two shapes:
   the pre-edit page still sitting in the map (readahead hasn't reached it),
   or — if the map entry is already a mid-render placeholder — the
   placeholder's `stalePredecessor` (below).
5. Nothing → silence, exactly today's underrun handling.

Mid-page content swap (step 2 while a fallback plays) is deliberate: the edit
already implies a waveform discontinuity; delaying to a page boundary would
just delay audibility by up to a page (1.37 s).

### 2. Keep the replaced page reachable while its successor renders

Proposal 15's `freezePage` *replaces* a stale-frozen map entry with a fresh
placeholder, so the pre-edit page vanishes from the map the moment the
re-render starts — precisely the window where playback needs it. Add to
`twOutputPage`:

```cpp
std::shared_ptr<twOutputPage> stalePredecessor;  // via std::atomic_load/store
```

- Set (under the component mutex) when `freezePage` replaces a stale-frozen
  entry with a placeholder.
- Read lock-free by the audio thread (`std::atomic_load`) when it finds an
  unfrozen placeholder at the playhead position.
- Cleared when the placeholder is stamped frozen — bounding memory: at most
  one predecessor per in-flight render, released at completion.

### What changes audibly

| Situation | Before | After |
|---|---|---|
| Edit while playing, playhead mid-page | silence until re-freeze | pre-edit audio, then new content within ~one readahead cycle |
| Page crossing into a position being re-frozen | silence until stamp | pre-edit audio via `stalePredecessor` |
| True underrun (readahead behind) | silence | silence (unchanged) |
| Offline render | exact | exact (unchanged) |

## Tests

New `tw_playback` module test (`playback/tests/playback_test.cc`):

- **Engine-level:** a tone source with a controllable render delay; start the
  engine + readahead, pull until audible; flip amplitude + `bumpContentEpoch`
  with a 300 ms render delay; the very next `pullBlock` must return full
  frames of the OLD amplitude (unfixed code returns 0 frames/silence); polling
  on, the NEW amplitude must become audible with no zero-frame pull in
  between.
- **Component-level:** freeze a page, bump, start a slow re-freeze on a second
  thread; the map's placeholder must expose the pre-edit page as
  `stalePredecessor` while rendering and release it once stamped.

## Effort / risk

Small and local: one member on `twOutputPage`, ~10 lines in
`twComponent::freezePage`, a rewritten preference ladder in
`updateFrozenPage`, one new module test. The risky surface (epoch semantics,
render path) is untouched.
