# The Linux gate

**What this file is for.** `ctest` is the safety net (CLAUDE.md), and on each
platform that net has holes. This is the list for Linux, so anyone gating a
change there can tell their own breakage from the standing breakage. Its
siblings are `docs/MACOS_GATE.md` and `docs/ASIO_WINDOWS_GATE.md`.

Unlike those two, this list was not built by hand: **it is what the first CI run
measured.** Before CI existed nobody had a Linux baseline at all, which is why
this file is new and short.

## Baseline

Measured **2026-10-03** on the first-ever run of `.github/workflows/ci.yml`
(`ubuntu-latest`, GitHub-hosted, Qt 6 from `qt6-base-dev`,
`QT_QPA_PLATFORM=offscreen`, `ctest -j4`), at the tip of `feat/ci`:

```
99% tests passed, 2 tests failed out of 400
Total Test time (real) = 367.51 s
```

With both exclusions below applied, the same tree is **green at 398/398** in
367 s.

Also **5 disabled** and not counted: `qxa.au_effect_audible`,
`qxa.au_missing_placeholder`, `qxa.au_slot_roundtrip` (AU is macOS-only) and
`qxa.media_options_page`, `qxa.media_secret_redaction`.

### Re-measured on the pinned image, 2026-11-23 (QBX-309)

The 2026-10-03 baseline above was taken on **`ubuntu-24.04`** with Qt
`6.4.2+dfsg-21.1build5`, which is what `ubuntu-latest` meant then. The label
moved to **`ubuntu-26.04`** between 2026-11-05 15:27Z (run `42055149990`, the
last on 24.04) and 2026-11-06 18:29Z (run `42271202494`), bringing Qt
`6.10.2+dfsg-7`. Nothing in any diff announced it, and every run since has
been green. `ci.yml` now pins `ubuntu-26.04` on both jobs, so the next image
change is a commit.

Run `45236914217` (PR #382, the pin itself), same exclusions:

```
100% tests passed out of 637
Total Test time (real) = 517.31 s
```

The same 5 disabled. The rise from 398 is the suite's growth, not the image:
`main`'s last unpinned run at `c9b7a256` (`45005733166`, already 26.04) also
passed 637/637. Neither exclusion was revisited on the new image.

## The two failures

### 1. `secret_store_test` — no keyring on a headless runner

**Not a code defect. Excluded, because it cannot be configured around.**

The test reports `platform default backend on this build: libsecret`, and a
GitHub-hosted runner has no D-Bus session or keyring daemon for libsecret to
talk to.

The first attempt here was to set `SMARAGD_SECRET_BACKEND=memory`, on the
strength of the test's own opening section proving that `resolveBackend()`
honours it. **That did not work, and the reason is worth recording:** the test
does not only ask `resolveBackend()` what it would pick — it goes on to
exercise *the platform default backend* deliberately, resolved independently of
the override ("platform backend under test: libsecret"). Overriding the
environment cannot rescue a test whose subject is what the environment would
otherwise have chosen.

So it is excluded, in the same category as the three macOS-only AU cases: an
environment this runner cannot provide, not a defect.

Two consequences to be honest about. The libsecret backend is **not covered on
Linux by CI** — exercising it needs a session bus and a keyring, i.e. a
desktop, so it stays a manual check. But `libsecret-1-dev` is still installed
deliberately, so the backend is still **compiled**; dropping the dependency
would have turned the test green by removing the code it tests, which is worse
than an exclusion that says what it is.

### 2. `qxa.asset_clip_preview` — SEGFAULT, needs triage

**Excluded from CI, and this is a real open question, not an environment
quirk.** It segfaults in ~3.6 s. Three possibilities, in the order I would
check them:

- the offscreen platform plugin — a preview path that assumes a real window or
  screen would crash under `QT_QPA_PLATFORM=offscreen` and nowhere else;
- a genuine Linux-only bug in the asset preview path;
- a `-j4` interaction, though the other 399 cases tolerate it.

Nothing here distinguishes them: the diagnosis needs a Linux box with a
debugger, which the run that found it does not have. The exclusion lives in
`.github/workflows/ci.yml` as `CTEST_EXCLUDE`, in one place, with a pointer to
this section — **this list is meant to shrink.**

## Flakes seen once in CI and NOT excluded (QBX-200)

**Three tests failed together in run `38911317466` attempt 1** on
`feat/QBX-186-isolate-take`, and **attempt 2 of the same run, on the same
commit with no changes, was green** in 12m46s. So all three are
non-deterministic, none is excluded, and none of them is a reason to read your
own run as broken — but equally, a failure in one of them is not automatically
"the known flake" either. Check the numbers in the log before concluding.

```
 49 - qxa.collect_external_media_missing (Failed)
354 - qxa.sample_missing_survives (Failed)
440 - twlog_test (Failed)
```

### `twlog_test` — FIXED, not excluded (QBX-193)

The assertion was `worst single logf < 2000us` for an RT thread under five-way
contention, i.e. a wall-clock worst case, which is dominated by OS scheduling
rather than by the lock it meant to measure. On a saturated runner it broke with
the design working perfectly; the same run reported 211,295 records dropped to
contention, which is exactly the condition such a ceiling is least able to
survive. It is now gated deterministically by HOLDING the sink's mutex, so
`try_lock` cannot succeed and the drop is a fact rather than a probability. See
`tw/core/CONTRACT.md` invariant 5. **Nothing about it is load-sensitive any
more, so a failure there is a real defect.**

### The two `.qxa` cases — MECHANISM NOT ESTABLISHED

Both load `smaragd/tests/sample_missing_survives.qxp`, and both turn on one
reference being unreachable **on this machine**. That is a claim about the
environment in two ways, and either one would produce the exact failure
signature seen (wrong counts, nothing else):

1. the stored spelling is `~/__smaragd_no_such_dir_missing__/__missing_sample__.wav`
   and the `~` is expanded against **`$HOME`**;
2. if resolution fails, the loader's last-resort recovery adopts any file with
   the **same basename sitting next to the project file**
   (`splainwave.cpp`, `instantiateFromDomElement`) — by design, for projects
   carried as a unit.

A THIRD case, `qxa.load_missing_sample_placed_survives`, loads a different
fixture carrying the SAME `~` path and did not flake. That neither confirms nor
refutes the above: before QBX-200 its only assertion over the miss was
`assert-log contains="__missing_sample__.wav"`, and a line about a file that
RESOLVED carries the basename too ("Name of extern file is …"), so it would have
passed either way. It now carries the same precondition assertion as the other
two, which closed that hole.

**The HOME half is demonstrable on demand**, which is as close to a mechanism as
this got. Running the two cases with `HOME` pointed at a directory that DOES
contain `__smaragd_no_such_dir_missing__/__missing_sample__.wav` reproduces
CI's failure signature closely: `assert-extern-files FAILED: missing is 0
expected 1`, three times in `collect_external_media_missing`, plus
`collect-external-media FAILED: copied 2 expected 1`. CI reported four rejected
actions against this one's four-to-five, so the shapes agree without matching
exactly. What is NOT explained is what could have supplied such a path on a
GitHub runner for one attempt and not the next — attempt 2 was green on the same
commit, so whatever it was, it was transient. **This is a hypothesis with a
matching signature, not a diagnosis.**

**Not reproduced locally in any configuration tried**: 5/5 serially, 490/490 at
`-j4` and 490/490 at `-j24` on a 12-core box (oversubscribed well past CI's
`-j4` on 4 cores), and, after QBX-200, 30 runs of each of the three cases at
`SMARAGD_REVAL_WORKERS` 1, 4, 8 and 16 under a parallel `ctest -j8` of the rest
of the suite — **360/360**. A GitHub runner differs enough in per-core speed and
memory bandwidth that a load-shaped failure may simply be unreachable here,
which is itself worth knowing.

What QBX-200 changed, since the mechanism could not be found: **both cases now
state that precondition as their first assertion** rather than assuming it, so a
machine that supplies either path fails on a named line instead of on an opaque
count, and the loader **announces a placeholder** (`sample NOT FOUND, keeping a
silent placeholder: <stored> -> looked in <resolved>`) where it used to create
one silently. The resolved path is in the message, so a surprising `$HOME`
appears in the run itself.

**And the run now carries the diagnosis.** `ci/gates.sh` exports
`SMARAGD_LOG_CONSOLE=1`, so `ctest --output-on-failure` prints the failing
case's own log. Before that, a failed `.qxa` in CI left only the runner's
summary — "4 actions rejected" and the verb names — while every warning the
actions emitted, including `assert-extern-files FAILED: missing is N expected 1`
and the paths it looked for, was discarded. That is why these two could not be
diagnosed from the run and had to be guessed at. A green run is as quiet as
before, because `--output-on-failure` prints nothing for a test that passed.

## `qxa.master_closure_linear_ring` — MECHANISM ESTABLISHED, and it was never CI-only (QBX-228)

**Not excluded, and no longer expected to flake.** This case went red in CI on
two unrelated PRs — #296, a docs-only change, and #302 — and the per-platform
record previously had it filed with `devices_input_test` as a load-sensitive
timing flake. That attribution was wrong in both halves.

**What it was.** The measurement window was ABSOLUTE
(`startFrame="168000"`), and captured frame 0 is the **device start**, not the
locator position. The leading silence in front of it is a wall-clock quantity of
the machine, which `sdumpplaybackcaptureaction.h` states at length and ends by
requiring that "anything measured over this file … bound the start with
`assert-audio-onset`, and begin the measurement window one device block in".
This case did not. Over the RAMPING material it measures, a displaced window
reads as an amplitude difference.

**The numbers, all measured:**

| | |
|---|---|
| this box, in the usual mode | `0.0820714` |
| CI's failing value, **identically on both PRs** | `0.0845199` (+2.98 %) |
| the same window displaced by **2048 frames** locally | `0.0845219` |
| frames of slip the ±3 % band admits | **−2028 .. +2008** |

So the band was a **positional tolerance in disguise**, CI missed it by sixteen
frames' worth, and widening it would have been the one wrong remedy: at 3 % it
already tolerated two capture blocks.

**It is NOT CI-only.** While this was being fixed, the sibling case
`master_closure_heard_ring` switched modes between two `ctest` rounds on the dev
box: `0.164143` → `0.169044`, exactly the −2048 value, then bit-stable in the new
mode for four consecutive runs. Its ceiling is `0.1691`, so it passed by
`0.00006`. "Bit-stable locally" was true of each MODE and said nothing about
which mode a run would get. CI is not special; it lands in the mode that misses
the band.

**TWO of the four are anchored, and that split is a measurement rather than an
oversight.** `master_closure_linear_ring` and `master_closure_heard_ring` vary
by a pure capture-head displacement — `heard_ring`'s displaced reading of
`0.169044` is exactly what sweeping its clean capture to −2048 frames predicts —
so an onset anchor removes it, measured 2.98 % → 0.32 %. Both are now anchored
with `assert-audio-energy anchorOnset="1"`, and `linear_ring` gates the
mechanism with injected displacements (`leadSilenceFrames` at 4096 for a usable
RMS margin and at 48000 for a mode-independent proof that the injection is
real), so the failure reproduces locally and deterministically instead of
waiting for a runner.

**The two ROOT cases were deliberately NOT anchored**, because anchoring one of
them made it worse. `master_closure_heard_root` had two modes over six measured
runs — onset 2056 reading `0.204219`, and onset ~4075 reading `0.201039` — and
shifting the window by the ~2019 frames the onset moved read `0.197326`,
further away. Filed as **QBX-239**, which measured what it is.

**QBX-239: the root cases' variation is the RELATIVE ALIGNMENT of their two
sources, not a displacement.** Each capture decomposes exactly (residual
`0.000018`, gains `0.50000` / `0.50238`) into the ring, `test_sawtooth.wav`,
which the file input LOOPS every 192000 frames, and the root,
`test_autosaw.wav`:

| mode | root audible from | ring file frame 0 at | old window | one-loop window |
|---|---|---|---|---|
| A (1 of 14 runs) | ~2148 | 6144 | `0.204219` | `0.175791` |
| B (13 of 14 runs) | ~4148 | 4096 | `0.201039` | `0.176901` |

Between the modes the two sources move about two blocks in OPPOSITE directions.
A whole-capture sweep of B against A over ±8192 frames finds no offset that
matches the captures (best residual `0.065` against a signal of `0.17`). The
onset follows the root, which is loud from its first frame, while the 1 s
window's reading was decided by the ring: a monitored input has no defined
phase against the arrangement, and one second of a ramping, looping input
measures that phase. Modelled over every phase of the loop, that window reads
anywhere from `0.1205` to `0.2342`. Not a product defect.

**The fix is a window of exactly one input loop**, `[64000, 256000)`. It is a
multiple of the root's 100-frame period too, so neither source's start moves the
reading. What is left is a small cross term: 440 Hz's 12th harmonic and 480 Hz's
11th are both 5280 Hz. Computed from the fixtures over every relative offset, it
spans `0.175744`..`0.177672` around the closed form `0.176741` (raised:
`0.265836`..`0.268381` around `0.267151`, a factor of 1.5115). The bands add at
least 1.2 % to each side of those ranges. The cases now wait 6600 ms instead of
5200 ms, which captures about 322560 frames.

**Sabotaged, all on one commit:** with the root absent (the pump's frozen-input
sum skipped), both cases read `0.133322`, the ring alone, and fail. The raised
case at −12 dB reads `0.176901` and fails, and the root case at −6 dB reads
`0.267362` and fails. The rejected design was checked as well: the file input's
cursor was started a quarter loop in (48000, then 48050 so that the cross term
moves too). The new cases stay green (`0.176138` / `0.267932`). The OLD cases,
run from saved copies, fail at `0.128151` / `0.237583`.

**If it returns**, the assertion now names where it measured and what it
anchored on — `over [ 169826 , 217826 ) "anchored on onset 19304 + 150522"` —
so one failing log line is enough to tell a displacement from a level change.

## `qxa.midi_rec_union_r2` — SEGFAULT, MECHANISM ESTABLISHED and FIXED by QBX-276, not excluded (QBX-308)

**Not excluded, and not expected to crash again.** A failure here now is a new
defect, not this flake.

**The sightings.** Each one SEGFAULTed after the case's last assertion, and each
went green when the identical tree was re-run:

| when | PR / branch | commit | run / job | time to crash |
|---|---|---|---|---|
| 2026-10-31 | #326 | `eb2840e6` | `41230648835` / `124623977120` | 1.99 s |
| 2026-11-01 | #327 | `aded0a91` | `41294517642` / `124812441412` | 1.62 s |
| 2026-11-02 | `main` (QBX-276's report) | `aec8f613` | `41392830211` / `125153732045` | — |

A passing run takes 0.07–0.10 s. Both crash logs end on the same line, the first
`twTrackMix: removed 0 clip(s)` of `verify-undo`'s first undo, after the case's
own `<redo/>`.

**The mechanism is QBX-276's, established there under ASan**, where it was a
heap-use-after-free on every run. `place-midi-recording` holds its plan and
rebuilds it on every apply, so a redo deleted the first plan. But
`SActionUndoCommand` kept the FIRST inverse, so the next undo's
`unmerge-columns` read its operands out of a freed `SMergeColumnsAction`. A
normal build usually hands the freed block straight back to the re-plan's own
merge, which is why the case passed nearly always. Fixed by `0dc8d084` (PR #358,
merged as `5175f19e` on 2026-11-08): `SActionHistory::reapply()` returns the
redo's inverse and `SActionUndoCommand::redo()` adopts it (actions `CONTRACT.md`
inv. 7). It is gated by `qxa.undo_redo_adopts_inverse`. QBX-308 was filed later
from the two older sightings, without that link.

**Measured at `main` `3670ccab`, 2026-11-27, on a 12-core box.** The loop ran
the binary with ctest's ENVIRONMENT and a private `XDG_CONFIG_HOME`, and
recorded the exit code. `repeat_test.sh` was not used, because it cannot tell a
SEGFAULT from a failed assertion.

| regime | `SMARAGD_REVAL_WORKERS` | runs | crashes |
|---|---|---|---|
| idle | default, 1, 4, 16 | 200 + 100 + 100 + 100 | 0 |
| idle, poisoned heap (`glibc.malloc.perturb=65:tcache_count=0`) | default | 200 | 0 |
| 36 spinners on 12 cores, load 36–39 | default, 1, 16 | 200 + 100 + 100 | 0 |
| the same load, poisoned heap | default | 100 | 0 |

**And the fix is what the zero rests on, measured by restoring the pre-fix
behaviour in `SActionUndoCommand::redo()`** (keep the first inverse, delete the
redo's; restored from saved bytes afterwards):

| sabotaged build | runs | result |
|---|---|---|
| `midi_rec_union_r2`, ordinary heap | 100 | 0 crashes, as in the CI history |
| `midi_rec_union_r2`, poisoned heap | 100 | **52 SEGFAULT** |
| `undo_redo_adopts_inverse` | 5 | **5 FAIL**, `want stale= 0 … got stale= 2` |

So the poisoned heap turns this use-after-free from rare into about one run in
two, and the dedicated case catches it on every run. **No CI job has failed
this case since the fix.** All 79 completed ctest jobs from 2026-11-08 to
2026-11-27 were checked, re-run attempts included.

## `qxa.rec_union_cycle_inside` — reported as a local suite flake, NOT reproduced (QBX-220)

**It has never failed in CI.** It is recorded here anyway so the next person
does not repeat the measurement, which is the expensive part.

Two failures were seen on a dev box, on two different feature branches, each
followed by a green re-run on the identical tree:

| when | failing assertion | numbers |
|---|---|---|
| QBX-209's gate run | `assert-recorded-clip` | `minPasses="2"` not met |
| QBX-213's gate run | `assert-audio-energy` | rms 0.111858 against [0.15, 0.21] |

**It did not reproduce on `main` at `77f648d6` in any regime tried**, and the
measured quantity did not move at all:

| regime | runs | failures | window rms, every run |
|---|---|---|---|
| isolated, serial, idle (QBX-220's own report) | 12 | 0 | not recorded |
| isolated under 36 spinners on 12 cores, load 29 (same) | 12 | 0 | not recorded |
| isolated, instrumented | 4 | 0 | `0.176362` |
| full suite `-j4`, quiet box (load < 1.5) | 4 | 0 | `0.176362` |
| full suite `-j4` under 12 spinners, load ~17 | 4 | 0 | `0.176362` |

`0.176362` is the value to six digits on all 12 instrumented runs, with the
render 144000 frames and `passes=3` every time. The loaded suite runs were not
inert — they broke `qxa.metronome_click_while_recording` 3 times in 4 and
`devices_input_test` once — so the box was genuinely hostile and this case
still did not move. **Whatever the two original failures were, they are not
reachable on `main` today, and they are not CPU load.**

### How to triage it if it returns

`smaragd/tests/test_sawtooth.wav` is a per-second LEVEL STAIRCASE — rms
`0.066650 / 0.176362 / 0.290568 / 0.405489` for seconds 0..3 — and the band
`[0.15, 0.21]` brackets second 1 exactly. So that assertion does not ask "is
there energy", it asks **which source second am I reading**, and a failure
value decodes to a position. Decode it before reading the failure as level
drift.

The reported `0.111858` decodes two ways and **one number cannot choose**:
69.7 % second 0 + 30.3 % second 1 (about 33500 frames reading a second early),
or 28690 frames of silence inside the window. That is why the failing artifact
matters: `ru_ci.wav`'s per-second profile settles it immediately. A healthy one
reads `[0.3257, 0.0, 0.1764]` — the silent middle second is correct and
deliberate, a documented plan 53 deviation in `srecordplanner.cpp` (a cycle
pass that stopped mid-cycle answers for its whole extent, "silence included —
which is what a tape machine does").

Note that `assert-audio-energy` **cannot** detect a short render:
`audio_analysis.cc` divides by the frames actually read, not the requested
`frameCount`, so a truncated file reports its content at full level. A
truncation hypothesis for this failure is therefore ruled out, not merely
unlikely.

### What it is NOT

It is not the QBX-222 capture-head slip, which was the first thing checked. The
window is 48000 frames, where one 1024-frame device block costs 1.1 %; the
failure is a 37 % shortfall. **But that family is real and has a live member**:
`qxa.metronome_click_while_recording` fails about 2 in 3 in isolation at
loadavg >= 12, because it anchors its grid on a first click the capture start
truncates. That one reproduces on demand and is filed as QBX-227.

## `qxa.master_insert_while_monitoring` — MECHANISM ESTABLISHED; B FIXED by QBX-320, C still open (QBX-321); NOT excluded (QBX-318)

**Not excluded.** It went red on three unrelated PRs on 2026-11-24, and on two
`main` pushes on 2026-11-10 that were found later by scanning every CI job
since 2026-11-08. Each time the value was the **bit-identical** `0.17129`
against `[0.1592, 0.1691]`. Every PR failure passed when the job was re-run on
the same commit:

| PR | run | job | result |
|---|---|---|---|
| #387 | `45373093378` | `138372083387` | `0.17129` |
| #388 | `45375039041` | `138378872970` | `0.17129` |
| #389 | `45376223988` | `138383120939` | `0.17129` |
| `main` push, `c44f135b` | `42863616969` | `129995989812` | `0.17129` |
| `main` push, `231514a9` | `42863723087` | `129996365577` | `0.17129` |

All five dumped 256000 frames, all five had `[SCHED] graph stats:
nodesExecuted=757 nodeRetries=0 missPages=0 selfStale=0` and no render-policy
refusals. Those lines say nothing about this failure; the answer is in the
audio.

### Decoding the value

The window `[168000, 216000)` is ABSOLUTE, and it lies on a ramp of
`test_sawtooth.wav` (the per-second level staircase described under QBX-220),
so every reading decodes to **how far the ring's material sits from where this
box puts it**. Measured on local captures, with `audio_analysis.cc`'s
arithmetic:

| ring displaced by | reads |
|---|---|
| 0 | `0.1641429` (the expected `0.164142`) |
| +1024 | `0.1665854` |
| +2048 | `0.169044` (passes by `0.00006`) |
| +3072 | `0.1712965` |

### Two mechanisms, independent, both measured

**C — the capture head (the QBX-228 family).** The whole capture's ring starts
about **2049 frames late** (onset 8872 instead of 6823). It reads `0.1690398`,
which the log prints as `0.16904`. Seen in **2 of 37** idle runs, with no frozen
miss. This is the displacement the onset anchor was built for, and this case
does not use one.

**B — a clock stall at the flip, a product glitch.** The master insert re-roots
the frozen lane below the master chain (`[READAHEAD] Frozen root changed:
restarting chain at page 65536`). The callback that follows can find **no page
of the new root**, so `AudioEngine::pullBlock` comes up short by a whole block
at engine position **129024**, and the **playhead stalls for that block**. Under
Closure the frozen lane's audio is discarded, but it still owns the clock. Then:

- the RT plays a **1024-frame silent gap in the monitored signal**:
  `mixStream`'s `notYet`, because the next ring entry is now in the future;
- every frame after the gap is **one block later, for the rest of the arm**:
  the input FIFO kept filling while the pump, pacing on the stalled clock,
  idled.

Confirmed by a temporary counter in `twSpeaker::renderCallbackBody` (since
reverted). Over 22 instrumented runs it read `frozenShort=1 frames=1024
first=129024` in exactly the 5 runs that had the gap, and 0 in the other 17.

| regime | runs | none | B ×1 | B ×2 | C | other |
|---|---|---|---|---|---|---|
| idle, load < 3.6 (`ctest -R` loops, and the binary run with ctest's ENVIRONMENT) | 37 | 25 | 10 | 0 | 2 | 0 |
| load decaying 13 → 6 after the spinners | 10 | 8 | 2 | 0 | 0 | 0 |
| 36 spinners on 12 cores, load 13–33 | 16 | 4 | 9 | 1 | 0 | 2 (onset +1025, no gap, `0.166582`) |

**CI's `0.17129` is C and B×1 together.** A C capture with its post-flip window
moved one block reads `0.1712904`, which `qDebug` prints (6 significant digits,
trailing zero dropped) as exactly CI's `0.17129`. A three-block stall alone would
read `0.1712965` and print as `0.171297`, so the printed digits favour the
composition. CI's `[READAHEAD] … playhead=` samples, 3072 behind this box after
the flip, are **not** evidence either way: they are readahead-tick samples, and
locally they move by ±2048 from run to run.

### Confirmed from CI's own captures (QBX-320)

Since QBX-322, CI uploads the capture of a failing playback-capture case. Two
red runs on 2026-11-28 (`46118704137` and `46120153235`, both `0.17129`) decode
identically: **onset 8872** (C), a **1024-frame silent block at capture frame
132096** (B's gap), and the monitored lag **6144 at frame 48000 against 7168 at
168000** (B's permanent block of latency). So CI's value is C + B×1, read off the
audio rather than from the printed digits.

### B is fixed (QBX-320)

The author picked all three remedies on 2026-11-28, and all three are in.
Each one is in `smaragd/tw303a/playback/CONTRACT.md`: inv. 16b, inv. 21's
amendment, and re-root property 7.

1. **The live input is resynced after any clock stall.** The RT notes each
   short frozen pull on the live clock. The pump then drops exactly that many
   of the input's oldest frames, so a stall costs its block once and never
   becomes latency.
2. **A re-root below the master is published warm.** The probe showed that the
   cause was the readahead's frontier, not a missing page lookup. At the
   re-root, the new root's page under the playhead was stale in 14 of 15
   runs. The restart then set the frontier back to the page start and
   underran. Now the speaker demands that page and the next one, and waits
   for them (2.5 ms measured, bounded at 250 ms) before it publishes. The
   restart also probes before it stores the frontier.
3. **Under a Closure, a short frozen pull advances the clock** rather than
   stalling it, because its audio is discarded anyway.

Measured with the binary run under ctest's ENVIRONMENT, and every capture
decoded. B shows as a 1024-frame silent block near 132096 plus a lag drift of
+1024 between windows at 48000 and 168000:

| build | case | regime | runs | B (gap + drift) |
|---|---|---|---|---|
| `main` (`10b338c0`) | this case | load decaying 20 → 4 | 20 | **17** (plus one C) |
| `main` | this case | 36 spinners on 12 cores, load 15–36 | 20 | **9** |
| fix | this case | idle, load ≤ 4 | 20 | 0 |
| fix | this case | 36 spinners, load 39–40 | 20 | 0 |
| fix | `master_insert_monitor_clock` (new) | idle / 36 spinners | 20 / 20 | 0 / 0 |

This case still passes on `main` whenever C is absent, because its band
tolerates up to two blocks of B. That is why B is gated by its own case,
`master_insert_monitor_clock`, which asserts the drift and the gap directly.
On a build with all three remedies reverted that case failed 9 of 10 idle and
7 of 10 under 36 spinners. In the 3 passing runs B did not fire at all.

C is untouched: its onset of 8872 still appears, 1 in 20 on `main` idle. With B
gone, anchoring the window on the onset no longer hides anything, and that is
QBX-321.

One run in 40 of this case on the fixed build failed `assert-log` rather than
the audio. The Closure line was logged 4 ms before the `wait-ms` that opens
the assertion's window. That happened in none of the 40 `main` runs, and it is
not explained.

### Why it was not fixed in QBX-318

C is the case's fault, and anchoring on the onset would remove it. B is not the
case's fault. A master edit while monitoring costs the monitored input a dropout
of a block or more and adds that much monitor latency until disarm. The case only
notices this because the window reads position. Anchoring alone would make CI's
failing state pass at `0.166585` and leave B tolerated up to two blocks. That is
relaxing the case around a live glitch, so it was not done.

Every remedy for B needed a decision (taken 2026-11-28; see above):

- keep the clock running when a Closure-suppressed frozen pull misses;
- warm the new root's current page before publishing the re-root, as the
  arm/disarm epoch gate does for content;
- resync the input FIFO after any clock stall, since any frozen underrun while
  monitoring adds latency this way.

**If it returns**, decode the value against the table above. Then look for a
silent block near frame 132096 in
`test-output/master_insert_while_monitoring/master_insert_while_monitoring.wav`
(that is B), and for an onset later than ~6823 (that is C).

## Running the comparison yourself

```bash
./ci/gates.sh                  # build + the four checkers + ctest, excluding nothing
CTEST_EXCLUDE='^(qxa\.asset_clip_preview|secret_store_test)$' ./ci/gates.sh   # what CI runs
```

A local run excludes nothing by default, on purpose: the exclusion is a
statement about CI's environment, not about the test.
