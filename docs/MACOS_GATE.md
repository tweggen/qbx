# The macOS gate

**What this file is for.** There is no CI; `ctest` is the whole safety net
(CLAUDE.md). On macOS that net has holes, and without a list of them anyone
gating a change here has to build a baseline by hand to tell their own breakage
from the standing breakage. This is that list. Its sibling is
`docs/ASIO_WINDOWS_GATE.md`, which does the same job for the other platform.

Filed as QBX-118. Baseline re-measured **2026-09-30** at `8f0dea85`, on Darwin
25.5.0 / Apple silicon, Qt 6.11.1, a fresh out-of-tree `./build.sh`. The four
static checkers (`check_layering`, `check_logging`, `check_includes`,
`check_tempo_authority`) are clean; everything below is `ctest` only.

## Run it serially, not with -j4

```bash
ctest --test-dir smaragd/build            # serial: ~475 s for ~400 tests
```

**Do not build a baseline under `-j4`.** Two runs of *identical* code under
`-j4` on this box differed by **18 tests** — the whole `grain_*` and
`render_sawtooth_*` families plus `split_plain_screenshot`, `takes_screenshot`
and `test_track_*`. A `-j4` set difference is therefore worthless as evidence
that a change broke something. Serial costs about eight minutes and is stable
to within the one flake named below.

Two tests are **(Disabled)** in CMake — `qxa.media_options_page` and
`qxa.media_secret_redaction`. `ctest` lists them under "The following tests did
not run", in the same `N - name` format it uses for failures, so a script that
greps for that shape will report them as failures. `LastTestsFailed.log`
contains only real failures and is the safer thing to diff.

## Standing failures (16)

Every line has a ticket. If your change breaks something that is **not** on this
list, that one is yours.

| test | cluster | ticket |
|---|---|---|
| `qxa.asset_clip_preview` | C — lane overlay paints nothing in the classifier's window | QBX-125 |
| `qxa.doubleclick_blue_clip_resolve` | C | QBX-125 |
| `qxa.feel_flow_heatmap` | C | QBX-125 |
| `qxa.feel_flow_metric_lab` | C | QBX-125 |
| `qxa.folder_sum_preview` | C | QBX-125 |
| `qxa.follow_scroll_hold` | C | QBX-125 |
| `qxa.take_lane_domain` | C | QBX-125 |
| `qxa.takestack_legacy_wrap_lanes` | C | QBX-125 |
| `devices_midi_test` | D — no high-resolution wait on macOS | QBX-124 |
| `devices_input_test` | D | QBX-124 |
| `plugins_scan_test` | E — the cold scan itself fails, not just the cache | QBX-126 |
| `qxa.automation_plugin_param` | outside the five clusters | QBX-127 |
| `qxa.instrument_sine_render` | outside the five clusters | QBX-127 |
| `qxa.plugin_editor_persistence` | outside the five clusters | QBX-127 |
| `qxa.plugin_native_editor` | fixed by QBX-119; fails on `main` until that merges | QBX-119 |
| `au_test` | flake — SEGFAULTs under load, passes alone | QBX-118 |

`au_test` is the only one that is a flake rather than a solid failure: it passed
3 of 3 on its own and SEGFAULTs under parallel load. `devices_midi_test` is
solid but marginal — see below.

`qxa.plugin_native_editor` is worth knowing about even after QBX-119 merges,
because on `main` it reads **sticky state in your real `~/.config/Smaragd`**: it
passes on a machine that has never stored a plugin editor geometry and fails
once one exists. That is why QBX-118's original 21-failure inventory does not
list it and why two people can disagree about the baseline.

## Fixed, and what it taught

**Clusters A and B — seven cases, one root cause** (`qxa.mixer_pane_layout`,
`mixer_pane_polish`, `mixer_sections`, `mixer_narrow_strip`,
`mixer_close_teardown`, `mixer_inserts`, `send_strip_ui`).

Qt lets a style report a **layout item rect** smaller than the widget rect, so a
control with decorative margins can be packed by its visible body rather than
its frame. `QWidgetItem` reserves the small rect; `setGeometry()` then expands
the widget back to the large one. On Fusion and Windows the two rects are equal
and nothing shows. `QMacStyle` reports real margins:

```
mixer strip, macOS, before | item 0 min=20x8 max=20x8   widgetGeom 20x20
```

A 20 px M/S/R button given an **8 px cell**. The three buttons landed 12 px
apart, overlapping by 8 px each — which is what `assert-mixer-layout` was
reporting as 24-28 overlapping pairs with `crushed=0`. Nothing was being
squeezed; the cells were simply too small for the widgets in them.

It was never only cosmetic. The same deficit silently broke every arrangement
promise QBX-100/115/116 gated: `msrColumn`, `msrBesideFader` and
`narrowUnderMsr` all read **0 on macOS and 1 on Windows**, so three shipped
guarantees were false on this platform and no one could see it.

Fixed in `SApplication`'s constructor by setting `Qt::WA_LayoutUsesWidgetRect` on
every widget — Qt's own documented switch for this, whose documentation names
macOS as the reason it exists. It is applied **unconditionally** rather than
under `Q_OS_MACOS`: where the two rects are already equal it changes nothing, and
an `#ifdef` would be two code paths of which only one can ever be gated on a
given machine, which is how this survived in the first place.

**If you add a layout gate, this is why it now means the same thing on all three
platforms.** Before the fix, `sAuditLayout()`'s comparison of widget geometries
was only meaningful where the style's layout rect equalled the widget rect.

## Cluster D is a defect, not a tolerance

QBX-118 guessed clusters D's 5 ms and 2 ms bounds were Windows numbers to be
widened. They are not, and this is the trap worth not falling into twice.

`MidiOutScheduler::waitUntil()` has a high-resolution wait **for Windows only**
(`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`), and its comment records why: with the
portable `std::condition_variable::wait_until` the Windows box measured a worst
error of 15.36 ms, and the high-resolution timer brought the same run to well
under 1 ms. macOS takes the portable path and pays for it. Measured, 10
consecutive runs of `devices_midi_test`:

```
2.869  3.028  3.120  5.025  5.027  5.034  5.037  5.037  5.039  5.044   (ms)
```

The 5 ms bound sits *inside* that distribution, which is why the test fails most
runs but not all. `devices_input_test` lands on the same quantum (5.02-9.85 ms
against a 2 ms bound). Two independent tests showing one quantum is what makes
it the platform's wait granularity rather than either test's arithmetic.

So the bounds are honest and the code is slow. Widening them would hide 5 ms of
MIDI jitter — about a 32nd note at 150 bpm. QBX-124 has the measurements and the
candidate fixes.
