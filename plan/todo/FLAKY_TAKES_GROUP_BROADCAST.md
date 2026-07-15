# Flaky test: takes_group_broadcast (stale audio in silent tail)

## Status

Open. Pre-existing, ~40% failure rate. Discovered 2026-07-15 while reviewing
the `twComponent*` → `std::shared_ptr<twComponent>` refactor; confirmed
independent of that refactor (reproduced on the baseline with all review
changes stashed). Not a regression of any recent commit.

## Symptom

`smaragd/tests/cases/takes_group_broadcast.qxa` fails intermittently
(~4 of 10 runs) on **`assert-audio-energy (#21)`**:

```
SAssertAudioEnergyAction: RMS energy out of range expected [ 0 , 0.005 ] got 0.35274
# - Action assert-audio-energy (#21) failed to apply
```

Action #21 asserts that `group_comped.wav` frames **[96000, 192000)** are
silent (`maxRms 0.005`). That tail column was comped to take 1, whose source is
slipped 2 s and therefore runs past the source end in the tail region → it
**must be silence**. On failure the region instead contains **RMS ≈ 0.35274**,
which is almost exactly take-0 sec1's fixture value (**0.352**) — i.e. real
audio pulled from an *earlier source offset*, not random garbage.

The other 34 tests in the suite pass deterministically.

## Reproduce

```bash
cd smaragd/tests/cases
BIN=../../build/bin/smaragd.app/Contents/MacOS/smaragd
for i in $(seq 1 10); do
  "$BIN" --test-case takes_group_broadcast.qxa --test-output-dir /tmp/qxa_o 2>&1 \
    | grep -E "^PASS|out of range"
done
```

## Ruled out: concurrency race (render vs. preview workers)

First hypothesis: the offline render (`RenderSession` → `synthOutput_->freezePage()`,
`render_session.cc:171`) races with async `CaptureRevalidator` preview workers
that freeze the same components after the `select-take` invalidation.

Tested by making the render single-threaded: added a `CaptureRevalidator::waitUntilIdle()`
(active-job counter + condition variable) and wrapped the render in
`SProject::disableInvalidation()` / drain / `enableInvalidation()` so no preview
worker runs concurrently. **Flakiness was unchanged (~42%).** Hypothesis
falsified; the experiment was reverted. The non-determinism is therefore *not*
inter-thread — it varies between process runs of an effectively single-threaded
render.

## Narrowed location

- `twTrackMix::freezePage_nolock` (`twtrackmix.cc:293`) zero-fills its output
  page and (`:335`) skips any child whose `freezePage` returns `validFrames==0`.
  So a correctly-empty tail stays silent. The leak requires a **child clip's
  `freezePage` to return `validFrames > 0` with stale content** for a
  slipped-past-end range.
- The render path allocates `make_shared<twOutputPage>()` (zero-initialized
  vector), so the non-zeroing `CapturePagePool` is **not** the source.
- Therefore the bug is in the **leaf freeze / seek path**:
  `twView → SCut → source reader → twStreamingLatch`. Prime suspects: an
  offset/`bufPos` or `previousPage_` state-chain value that is intermittently
  wrong for the tail clip after the `split-clip` + `select-take` sequence,
  causing the source to read sec1's material instead of returning empty for the
  past-end region.

## Suggested next step

Instrument the child `freezePage` for the tail clip: log `startPos`/`childPos`,
the resolved source offset, `validFrames`, and the `previousPage`/`bufPos`
state on each page, across many runs, and diff a passing vs. failing render to
find where the tail's read position diverges. Focus on how sequential-freeze
position state is (re)initialized for a clip after a take switch, since the
divergence is between process runs rather than within one deterministic pass.

## Related

- `plan/todo/11_RENDER_SILENCE_BUG_INVESTIGATION.md`,
  `OFFSET_FLOW_THROUGH_PAGE_CACHE.md`, `OFFSET_FLOW_TRACE.md` — prior
  offset/page-cache investigations in the same freeze path.
