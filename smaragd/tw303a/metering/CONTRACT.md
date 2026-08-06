# tw/metering — CONTRACT

Purpose: turn a component's frozen pages into the two numbers a level meter
draws (a fast peak with a held tick, and a 300 ms RMS), plus a latching clip
flag. Proposal 34.

Public headers: tw_level_scan.h, tw_meter_ballistics.h, tw_level_probe.h.

Depends on: tw/core, tw/pages, tw/graph. Forbidden: everything else — in
particular NOT tw/playback or tw/mix. The probe takes any `twComponent`; which
component is the right tap is the app's decision, not this module's.

Invariants:

1. **Levels are read BY POSITION, never computed at freeze time.** Pages are
   frozen by readahead/revalidator workers far ahead of the playhead (a page is
   65536 frames ≈ 1.37 s at 48 kHz) and by renders with no playhead at all, so a
   freeze-time measurement would report the future. Reading the page that covers
   position P is inherently "the audio at P".

2. **The probe never blocks, never waits, and never creates a demand.** It reads
   through `twComponent::getPageIfExists()` — a try-lock lookup that returns
   nullptr rather than stalling — and reports a miss otherwise. `advanceTo()`
   returning false is the caller's cue to `idle()` the ballistics, so a dropout
   reads as a fast fall and never as a frozen bar. Nothing here may call
   `freezePage`/`requestPage`.

3. **Page acceptance ladder** (`resolvePage_`), a read-only echo of
   `AudioEngine::updateFrozenPage` so the meter agrees with the ear:
   (1) the cached page if frozen and current; (2) **frozen but stale is still
   accepted** — playback is serving exactly that page (proposal 16), so
   rejecting it would make the meter disagree with the ear precisely while an
   edit is absorbed; (3) a placeholder's `stalePredecessor` if frozen;
   (4) the page held from the previous tick, if it is for the same page start
   and still frozen — this is what makes a lost try-lock survivable;
   (5) otherwise a miss. The engine's copy is the source of truth; if it changes,
   this echoes it.

4. **Reading a page while it is re-frozen in place is an ACCEPTED race.**
   `getOrAllocatePage` re-renders into the same buffer, and `validFrames` is a
   plain `uint32_t` a freeze thread may be storing concurrently. `samples` is
   sized once in the constructor and never resized, so clamping the scan against
   `samples.size()` cannot go out of bounds; the worst case is one visually wrong
   meter frame. This is the same race the audio thread already runs — no new
   hazard class. Never assert on measured values.

5. **Ballistics are wall-clock driven, not tick driven.** Every decay is per
   second and integrated over the actual dt, and the RMS one-pole uses
   `alpha = 1 - exp(-dt/tau)`. Consequence, asserted by `metering_test`: one
   1 s step and 100 × 10 ms steps produce the same peak, hold and RMS. This is
   why ballistics live here (UI thread, no Qt) rather than in the engine — an
   engine-side accumulator could not offer it.

6. **`frames == 0` means "no measurement", NOT "silence".** `twScanSpan` returns
   it for an empty or null span, and `twMeterBallistics::push` routes it to
   `idle()`. A meter that treated it as a −∞ sample would snap to the floor on
   every page miss instead of decaying.

7. **Clip is latched, never time-based.** It clears only via `clearClip()` (the
   UI's click) or `reset()`. Threshold is 0.999, not 1.0: a float that
   round-tripped through 16-bit PCM lands on 32767/32768 and is a clip for every
   practical purpose.

8. **One probe and one ballistics instance per meter, single-threaded.** Neither
   is thread-safe, and neither belongs on the RT audio callback — the metering
   pump runs on the main thread (`SApplication::pumpMeters`).

9. A window straddling a page boundary is clamped to the page holding its start,
   losing at most `MAX_WINDOW` frames of tail once per page. Deliberate: the peak
   of the earlier part is still reported, and a second page lookup per tick is not
   worth it.

Deliberately NOT done: `twRenderAspect::twAspectMetadata` ("Duration, peak
levels", `tw/pages/tw_output_page.h`) is left untouched and still unclaimed.
`twComponent::freezePage` already stores `validAspects = twAspectAll`
unconditionally, so that bit is already set and already means nothing; giving it
meaning would pull metering into the demand/revalidation system — new demands,
new page-validity semantics, and exposure to the render byte-`cmp` gate — for no
benefit over reading pages that already exist.

How to test: `ctest -R metering_test`. The probe half builds the real
`twTrackMix → twRewire` shape (the actual per-track tap) over a scripted ramp
source and asserts window arithmetic, the page-boundary clamp, the miss paths,
stale-page acceptance, and that the reading is post-fader. End-to-end coverage
is `qxa.meter_levels`, which asserts against the ramped-sawtooth fixture's known
per-second RMS.

Known debt:
- Mono only. `twComponent::freezePage_nolock` renders `idx = 0`, and `SStdMixer`
  runs one bus, so there is no second channel to meter yet. When the mixer grows
  to two busses a stereo meter is two probes; nothing here needs to change.
- No K-weighted / LUFS loudness, only sample peak and RMS.
