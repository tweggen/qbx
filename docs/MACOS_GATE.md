# The macOS gate

**What this file is for.** There is no CI; `ctest` is the whole safety net
(CLAUDE.md). On macOS that net has holes, and without a list of them anyone
gating a change here has to build a baseline by hand to tell their own breakage
from the standing breakage. This is that list. Its sibling is
`docs/ASIO_WINDOWS_GATE.md`, which does the same job for the other platform.

Filed as QBX-118. Baseline re-measured **2026-09-30** at `cf966eb8` (21
failures at `8f0dea85`, 7 now), on Darwin
25.5.0 / Apple silicon, Qt 6.11.1, a fresh out-of-tree `./ci/build.sh`. The four
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

## Standing failures (2)

Every line has a ticket. If your change breaks something that is **not** on this
list, that one is yours.

| test | cluster | ticket |
|---|---|---|
| `qxa.follow_scroll_hold` | shift+wheel does not scroll horizontally | QBX-129 |
| `plugins_scan_test` | E — the cold scan itself fails, not just the cache | QBX-126 |

Not in the table because it is not a standing failure: **`au_test`** is a flake
that SEGFAULTs under parallel load and passes alone (it did not fire in the last
serial run at all).

`qxa.plugin_native_editor` is no longer a standing failure (QBX-119), but it is
worth knowing why it used to be: it read **sticky state in your real
`~/.config/Smaragd`** and failed only once a plugin editor geometry had been
stored there. That is why QBX-118's original 21-failure inventory did not list
it, and why two people could disagree about the baseline.

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

## Cluster C: the pixel gates were reading the wrong rows

**A pixel gate must read a LOGICAL image** (QBX-125). The classifiers scan bands
computed from logical widget geometry — `canvas->laneTop( row )`,
`laneHeight( row )` — but `QWidget::grab()` honours the widget's device pixel
ratio. On a Retina Mac a 460 px wide canvas grabbed to a **920 px** image while
`laneTop()` went on answering in logical units, so every scan band landed at half
its intended y: **on a different lane than the one the verb named**, reported with
full confidence. Measured on `feel_flow_heatmap` before the fix — the classifier
scanned `scanTop=96 scanH=19` of a `w=920` image while the renderer had just been
handed `rect=460x98`, and found four colours with no palette member among them.

After rendering into a QImage constructed at dpr 1 (`sGrabLogical()`), the same
band reads `lutPixels=8721 lutIndexMin=0 lutIndexMax=23` — every one of the 24
palette steps present. The heat band had been painting correctly all along.

Rendering at 1x rather than scaling a 2x grab down matters more here than
anywhere else in the app: these gates assert **exact colour identity**
(`feelFlowPalette()` membership, the clip body's own rgb), and interpolating a
downscale would produce values that are in no palette at all.

Two cases went green on that alone; `feel_flow_heatmap` needed one more thing.
`assert-lane-overlay` required the lane fill to be present in the scan region —
sound for a whole-lane scan, where fill surrounds the clips, but false in
`bandOnly` mode where the scan region **is** the opaque feel-flow band. That
precondition only ever fired because the band was being read off the wrong rows.

The remaining six fail for a different reason, now visible because the grab is
honest: the canvas never reaches the size the case asked for. **QBX-128**, and it
is the same Qt hazard as QBX-119 — under `--test-case` nothing is mapped, so any
verb that resizes a widget and then measures it is measuring the old size.

## The VST3 fixture was never copied into the app bundle

Two of the three failures outside QBX-118's clusters were one packaging bug
(QBX-127). A qxa case names a plugin module by a RELATIVE path, which
`SPluginSlot::resolveModulePath` resolves against
`QCoreApplication::applicationDirPath()` — on macOS that is `Contents/MacOS`
INSIDE the bundle. `twtestclap.clap` was copied in there; `twtestvst3.vst3` was
not, so it sat in `build/bin` where nothing looked for it:

```
[vst3] no loadable binary in 'twtestvst3.vst3'
[slot] could not instantiate the plugin ... the slot ... is MISSING
```

The slot ran the transparent placeholder, so the audio came out at unity:
`automation_plugin_param` read RMS 0.230956 where it wanted 0.115478 — the
ungained level, exactly — and `instrument_sine_render` failed the same way. The
file itself was a perfectly good `Mach-O 64-bit bundle arm64` the whole time.

**The third case is the warning.** `plugin_restart_no_livelock` names the same
fixture and passed throughout, because a transparent placeholder is a perfectly
good way to not live-lock. A missing fixture does not always announce itself as a
failure; it can also quietly turn a case vacuous.

## A gate that reads the real smaragd.ini is not hermetic

`plugin_editor_persistence` asserted a settings key was ABSENT, and
`assert-settings-file` reads `SSettings::instance().configDir()` — the REAL
user-scope INI, the same file the developer's own sessions write. So the
assertion was never a statement about the run. It failed on this box because an
earlier session had left `editorGeometry\clap%3Atw.test.clap.gui` behind, and no
amount of correct product behaviour could have made it pass again. Verified by
deleting the key by hand: the case passed and the run did **not** write it back.

The case now establishes that precondition with `settings-remove` at the TOP of
its actions — before anything can legitimately write a geometry, so the assertion
still means "nothing wrote it". Removing the key just before asserting would have
made the gate vacuous; sabotage-checked by making `saveGeometryToSettings()`
write unconditionally, which the case still catches.

Redirecting the whole INI for `--test-case` runs would be the bigger fix and was
deliberately not taken: `configDir()` also holds `plugincache.v2.json`, so every
case would then cold-probe every plugin installed on the machine.

## Resizing an unmapped window does not resize its children

**Under `--test-case` nothing is mapped, so any verb that resizes a widget and
then measures it is measuring the OLD size** (QBX-128; the same Qt behaviour as
QBX-119, where a never-shown `QDialog` lost its `resize()` to the next layout
pass).

`resize()` updates the window's own geometry at once, but the QResizeEvent that
drives the layout is deferred to the eventual show (`WA_PendingResizeEvent`), and
a layout that was never told it is dirty has nothing to do when asked. The pixel
gates asked for a 900x600 canvas and grabbed a 150x89 one. Measured on
`take_lane_domain`, each step applied in turn to the same run:

```
after resize()                       canvas 150x89
after activate()                     canvas 150x89
after invalidate() + activate()      canvas 150x89
after draining QEvent::LayoutRequest canvas 150x89
after WA_DontShowOnScreen + show()   canvas 460x604
after hide() + clearing the attr     canvas 460x604
```

Only the last one does anything, and the settled geometry **survives going back
to hidden** — which is what makes it restorable rather than a permanent change to
the window's state. `invalidate()` is in that list because it is the obvious fix
and it does not work.

`WA_DontShowOnScreen` is what keeps the `show()` honest: AppKit never maps the
window, so nothing appears on the developer's desktop mid-suite and QBX-117's
full-screen-Space hazard cannot arise. A window that is already visible (an
ordinary run) is left alone entirely.

This had been invisible for as long as `grab()` returned a 2x pixmap: the doubled
image passed the `top + lh > img.height()` bounds check that a logical one
correctly fails, so the gates were failing that check by luck and mis-measuring
whenever they passed it. Fixing the grab (QBX-125) is what made it visible.

## Cluster D was a defect, not a tolerance — and it is fixed

QBX-118 guessed cluster D's 5 ms and 2 ms bounds were Windows numbers to widen.
They were honest; the code was slow. **The bounds are unchanged and the tests
now pass with two orders of magnitude of margin** (QBX-124).

macOS coalesces timers by a PROPORTION of the requested sleep, so a thread that
sleeps to a deadline wakes progressively later the longer it sleeps. The sender
thread now holds a `THREAD_TIME_CONSTRAINT_POLICY` — what CoreAudio gives its own
render thread — and does not change how it waits, so the existing no-lost-wakeup
machinery is untouched.

| test | before | after | bound |
|---|---|---|---|
| `devices_midi_test` | 5.02-5.07 ms | **0.032-0.075 ms** | 5 ms |
| `devices_input_test` | 5.02-9.85 ms | **0.031-0.042 ms** | 2 ms |

`late()` for the MIDI scheduler went from 8-14 of 16 messages to **0**.

Two things that do NOT work, measured so nobody spends a build on them again:
raising the QoS class (`QOS_CLASS_USER_INTERACTIVE` is no better than the default
and was worse at 10 ms), and swapping the primitive for `mach_wait_until()`
(indistinguishable from the condition variable — the coalescing is applied to the
THREAD, not to the call). Details and the full table: `tw303a/devices/CONTRACT.md`
inv. 13.
