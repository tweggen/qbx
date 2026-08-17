# What the 2026-08-17 recording investigation closed, and what it did not

> Written as a handoff, in the same spirit as `RECORDING_CHANNEL_COUNT.md`:
> everything below is measured, not inferred, and the numbers are quoted so the
> next person can tell a regression from a machine.

## 1. What was reported

Five symptoms from one hand-test session:

| | Symptom | Outcome |
|---|---|---|
| a | first monitor playback of a session "2 semitones too low and slow" | root-caused (§2), not a code defect |
| b | monitor ~3 beats behind the cursor; take placed late by the same amount | **fixed** (PR #53) |
| c | recording ~2 semitones lower than sung | root-caused (§2), not a code defect |
| d | input device picker offered only "System default" | **fixed** (PR #54) |
| e | `[PREVIEW] recompute` repeating without end | **STILL OPEN** (§4) |

## 2. (a) and (c): the endpoint sample-rate trap — NOT ours

Capture endpoint at 44100, render endpoint at 48000, one hardware clock. Full
write-up is in `CLAUDE.md` § "THE ENDPOINT SAMPLE-RATE TRAP". The arithmetic:

```
48000 × (48000 / 44100) = 52 244.9 Hz     measured 52 144.1 Hz   (0.998 of it)
```

**Two hypotheses died on the way and are recorded so nobody re-runs them:** it
is NOT a wire-rate mismatch in `twSpeaker` (every `rate diag` reads
`project=48000 wire=48000 device=48000 resampler=passthrough`), and it is NOT a
double conversion in the recorder (`resampler 48000 -> 48000: passthrough`).
Both paths report themselves correct because each is logged **before the other
stream exists**. That is why the `output-rate check` had to be added.

PR #55 surfaces the condition (both rates in the Options combos, a once-per-
session warning at record start, the capture-rate check escalated to a warning).
It does not, and cannot, correct it.

## 3. (b): what was actually wrong, and what is still true

Two defects, both fixed in PR #53:

- `locatorHeldElsewhere()` was `isRecordingActive()`, so the record worker owned
  the playhead for the whole take — but `startOutput()` returns *before* the
  device starts. The cursor ran ahead of anything audible from frame one.
- The placement compensation was `+= (outputLatency − inputLatency)`: wrong
  sign, and a difference that is ~0 when the two latencies are similar. It is
  now `−= (priming + outputLatency + inputLatency)`.

**Still true, and ours:** the priming itself. Measured **2.26 s** on the
reporter's project (108 240 frames missing from a 19.4 s run), against a
capture-backend baseline of 0.06–0.13 s. Nothing mis-times because of it any
more, but two seconds of silence after pressing record is a poor experience, and
the readahead is where to look.

## 4. (e) The endless preview recompute — OPEN

`CaptureRevalidator::dispatchRecomputation` logs `[PREVIEW] recompute … ->
page validFrames=65536` several times per millisecond, forever, for one object.

**Never reproduced.** Two headless probes (an asset-bearing project idling under
`--test-case`, and the same under `--run-actions` with the window shown) produced
**2** recomputes in 4 s, not a spin. It needs continuous repaints, a drag, or
live playback.

What is established:

- The line only prints when `revalNeeded_nolock` is true, i.e. the cut's
  `currentPage_` is null or missing `Preview`. Since each round also *succeeds*,
  something destroys the page as fast as it is made.
- Exactly one thing resets `currentPage_` outside a Playback invalidation:
  `SCut::invalidateCapture()`. Its only idle-time driver is
  `SCut::onArrangementChanged()`, gated on `everHadCapture_` — so only
  container-backed (asset) or grained/stretched clips respond, which fits a
  single object repeating in the log.
- `notifyArrangementChanged()` is called from **seven places inside
  `SMVActualView::mouseMoveEvent`** — every clip-drag branch. A drag is
  therefore a per-mouse-move invalidate-everything storm by construction.
- **A latent defect found on the way, not yet fixed:** a revalidation job
  publishes a FRESH pool page (`CapturePagePool::releasePage` zeroes
  `validAspects`) carrying only the aspects *that job* computed, then swaps it
  in wholesale. Any aspect the previous page had and this job did not compute is
  silently lost. Two different masks on one cut therefore clobber each other
  indefinitely. No live caller of `getPlaybackCapture()` exists today, so it is
  latent rather than proven as the cause — but it produces exactly this
  signature.

**The diagnostics that would name the driver were written and then reverted**
(they are in the history: `diag: TEMPORARY debug lines for the endless preview
recompute`, and the revert names it). Re-apply with `git revert` of the revert
if it recurs. They add:

```
[ARRANGE] notifyArrangementChanged why=<action name | drag.<gesture> | …>
[INVCAP]  obj=<cut> reason=<call site> everHadCapture=<0|1>
[CAPMISS] obj=<cut> want=0x<mask> have=0x<mask>
```

Reading them: `[ARRANGE]` at the same cadence as `[PREVIEW]` names the driver
outright; `[CAPMISS]` with a non-zero `have` that is still missing the wanted bit
means the recompute is being *clobbered*, not failing.

## 5. What has no coverage

**Nothing in the qxa suite records anything**, and every device path needs real
endpoints. Every change in §2–§3 is hand-verified only. The wide-file-into-
narrower-track fixture case that `RECORDING_CHANNEL_COUNT.md` §6 asks for is
still missing, and would have caught the 16-channel defect the moment 36-B3
landed.

## 6. The structural answer nobody has taken

**ASIO.** One driver, one clock, matched input and output, no shared mixer and
no per-endpoint rate to disagree about — which is what a Tascam is built for.
`tw303a/devices/src/asio_driver_list.cc` already exists. It would retire §2
entirely and shorten §3. It is a proposal, not a patch: a new `AudioBackend` and
`AudioInput`, SDK licensing, a device/channel model, and no headless coverage
possible.
