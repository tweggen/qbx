# Proposal 49 — Pan: one law, two stages, one automation target

> **STATUS: PROPOSED — DESIGN ONLY, awaiting requester review (QBX-105).**
> Implementation milestone tickets are filed AFTER that review, not before.
> Surveyed against `main` at **`1fe23b98`** (2026-09-13, after proposal 48 M1).
> Every file:line below is from that tree.
>
> **An adversarial review is REQUESTED** (the repo's practice since proposal
> 37 v2). §6 lists the claims this design expects to be challenged, so the
> reviewer can start there. None of them has been reviewed yet.

## The defect this closes

**The user can set a value that does nothing.** `SObject::pan_` is:

| Where | What it does today |
|---|---|
| `main/model/src/sobject.cpp:91` | stored, clamped to [-1, 1], emits `panChanged` — and **invalidates nothing** |
| `main/model/src/sobject.cpp:156` / `:244` | serialized on EVERY `SObject` as `pan='…'`, read back |
| `main/objects/cut/src/ssetclippanaction.cpp` | `set-clip-pan`: undoable, per-take, edit-group `broadcast` |
| `main/timeline/src/sclippropertiespanel.cpp:260` | a `QDoubleSpinBox` in the Clip Properties panel, with double-click reset |
| `main/testkit/src/sassertclipmixaction.cpp` | `assert-clip-mix expectPan=` |
| `main/objects/cut/src/stakehelpers.cpp:61` | **refuses** a take-column collapse when the wrapper's pan is non-zero |
| `main/objects/cut/src/scut.cpp:1490` | `cloneWindowOver()` copies it explicitly |
| `tw303a/` | **nothing reads it.** `tw303a/mix/CONTRACT.md` "Known debt" says so |

And `tests/cases/clip_volume_pan.qxa` section 3 **asserts** it is inaudible:
a render after `set-clip-pan pan="0.75"` must be `assert-file-identical` to
the unpanned one. That assertion was honest when written. This proposal
retires it, under a licence (T1).

There is no TRACK pan at all. `self:Pan` is named as missing in proposal 37 P5
(§4.5 table, line 572), proposal 36 §8 ("Panning. No pan law, no panner
stage"), and proposal 48 D8, where the mixer strip has **no pan knob on
purpose**: "a knob wired to `SObject::setPan()` — the value would be stored,
serialized, undoable and inaudible, which is the worst of the four available
outcomes".

## Reading list, before any milestone

1. `tw303a/mix/include/tw/mix/twgainstage.h`: the fader. Class infinity,
   pure, "at 0 dB unmuted a PURE COPY", and inv. 24's public arithmetic that
   the live pump shares. Track pan goes here (D2).
2. `tw303a/mix/src/twtrackmix.cc:600-670`: the clip loop, where
   `gainScalar × gainCurve × fade` is applied per OUTPUT channel with a
   CLAMPED source channel. Clip pan goes here (D2).
3. `main/objects/track/src/strack.cpp:2105` `refreshClipGainCurves()`: the one
   main-thread funnel that pushes a clip window's parameters into the mix.
4. `tw303a/playback/src/twliveplan.cc:77` `checkMasterShape` and
   `twMasterChainState`: proposal 45 D4a. A master control this check cannot
   see is a silently doubled image under live monitoring (T6).
5. `tw303a/playback/src/twlivepump.cc:375`: the pump's in-place
   `applyGain`, **with no channel argument** (T5).
6. `tw303a/playback/include/tw/playback/twspeaker.h:49`: `pullChannels` /
   `interleave`, the stereo monitoring reduction (D6).
7. `main/objects/track/src/strack.cpp:816` `sendTapComponent()`: the two send
   tap points (D2, proposal 47 D3).
8. `plan/proposed/36_MULTICHANNEL_SIGNAL_FLOW.md` §4.4 ("mono plays on every
   channel") and §8 (channel roles and a fold law are NON-goals).

---

## 1. Decisions

### D1. The pan law: a CENTRE-UNITY equal-power BALANCE, defined for width 2 only

**The law.** One pure function, `tw/mix/twpanlaw.h`, used by every stage
that pans (clip, track, live pump):

```
p ∈ [-1, 1]      (the stored SObject::pan_ unit, unchanged)

p == 0   :  gL = 1,              gR = 1                (exactly; no arithmetic)
p  > 0   :  gL = cos(p · π/2),    gR = 1                (the far side falls)
p  < 0   :  gL = 1,              gR = cos(|p| · π/2)
|p| == 1 :  the far side is EXACTLY 0.0f               (not cos(π/2) = 6.1e-17)
```

Applied per OUTPUT channel: channel 0 × gL, channel 1 × gR.

**Why centre-unity is FORCED, not chosen.** Every project in existence was
mixed with pan at centre = unity, because pan did nothing. A law that puts
centre at -3 dB (the classic mono-panner constant-power law) would make every
project 3 dB quieter the day M1 lands, and would break D5's byte identity for
every golden. Among centre-unity laws there are two families:

| Family | Hard pan | Rejected because |
|---|---|---|
| **Balance** (near side held at unity, far side falls) | total power -3 dB | — chosen |
| Compensated constant-power (near side rises to +3 dB) | total power constant | A panned signal goes ABOVE its unpanned level, which can clip a track that was at 0 dBFS a moment ago. A pan gesture that can clip is a surprise a user has to learn |

**Why the far side falls along `cos`, not linearly.** At p = 0.5 the far side
is -3.01 dB (`cos(π/4)`), not -6.02 dB (`1 - 0.5`). A cos sweep sounds even
across the travel. Linear sounds like nothing happens until the last quarter.
Both are one line, so the choice costs nothing either way.

**One law for mono and stereo SOURCES.** A mono clip on a stereo track reaches
the mix as two identical channels (§4.4's clamp, `twtrackmix.cc:647`). A
balance over two identical channels IS a mono pan with the centre-unity law.
A stereo clip gets a true balance. The law does not ask which it has. There is
no "mono pan vs stereo balance" mode switch, and T8 is the one place that
distinction could leak back in.

**Width ≠ 2 is TRANSPARENT, and the verbs REFUSE a non-zero pan there.** The
ticket offered three answers for 1/4/6/8-channel projects:

| Option | Verdict |
|---|---|
| **Stereo-only: pan is defined at width 2; at any other width it is a pure pass-through, and setting a non-zero value is REFUSED** | **Chosen** |
| Pan channels 0/1 of a wider project (the monitor pair) and leave 2..N alone | Rejected. `twSpeaker`'s "first two channels" is a LISTENING compromise for the device. Proposal 36 says it must never be applied silently to a delivered file. On a quad project (FL FR RL RR) it would shift the front image and leave the rear centred. That is a defect, not a feature |
| A balance over N channels via channel roles and a fold law | Rejected **for this proposal**. It is exactly the non-goal 36 §8 names. It needs a `<SProject>` role map, a UI to author it, and a law per layout. It is a proposal of its own, and nobody has asked for surround panning |

Width 1 is transparent by necessity: there is one channel and nothing to
balance. The monitor fans it to both device channels and a pan could not be
heard anyway.

**A stored pan that becomes inert is ANNOUNCED, never silent.** A width change
(`set-project-channels`) must not be blocked by a pan value somewhere in the
project, so the verb still succeeds. It logs ONCE, naming how many lanes and
clips carry a non-zero pan that is now inert. Every pan control is disabled
with a tooltip that names the width. The values are kept, and they are heard
again if the width returns to 2. This is the `twSpeaker` "log the reduction
once per width" discipline, applied to the model.

### D2. Where pan lives: TWO stages, and the order is the existing graph's

```
clip page ─► [clip gain × fade × CLIP PAN] ─► twTrackMix sum ─► twPluginChain ─► twGainStage [fader × mute × TRACK PAN] ─► twRewire
                 (twTrackMix clip loop)                               │                         │
                                                              pre-fader send tap        post-fader send tap
                                                              (pre-pan)                 (post-pan)
```

**Clip pan (the existing `pan_` on a clip WINDOW) lives in `twTrackMix`'s clip
loop,** as a fourth factor in the product that already carries `gainScalar`,
`gainCurve` and the fade. That is the only place where a clip is still a clip
and not yet part of a track sum. It is PRE-FX, pre-fader, and ahead of BOTH
send taps. A panned clip's position therefore survives into the insert chain
and into every send, which is what a clip-level position means.

It reaches the entry through `refreshClipGainCurves()`, the funnel that
already pushes volume, curve and fade from the SAME window
(`SClipWindow::parametersOf`). So a wrapped take column's pan is the
wrapper's, a direct column's is the active take's, and `set-clip-pan` already
edits exactly those (`panTargetCut`). The resolution rule is already
consistent and nothing new is invented.

**Track pan lives in `twGainStage`, beside the fader.** It is post-FX,
post-fader and pre-rewire. Three reasons:

1. **The gain stage is class infinity and pure**, and pan is a per-channel
   scale. Adding it keeps the component pure, keeps range invalidation EXACT,
   and keeps D3's automation in the same invalidation class as `self:Volume`.
2. **mix/CONTRACT inv. 24: the fader's arithmetic is PUBLIC and is the ONLY
   COPY.** The live pump applies it to blocks it rendered itself. Putting pan
   inside that same `Envelope` + `applyGain` is what makes a monitored track's
   pan identical to the frozen one it hands back to (D6). A separate
   `twPanStage` component would need a second public arithmetic, a second plan
   field and a second place to forget.
3. **Post-FX is what an instrument's output needs** (37 D5's argument for the
   fader, unchanged), and a stereo insert must see the unpanned image. A
   stereo reverb fed a hard-panned signal is a different effect.

**The send taps (proposal 47 D3) fall out of the tap points, not a new
decision.** `sendTapComponent(preFader=true)` is the chain output, which is
pre-fader AND pre-TRACK-pan. `preFader=false` is the gain stage output, which
is post both. Clip pan is upstream of both. A "pre-fader send that follows the
main pan" (some DAWs offer it) would need a third tap point or a pan factor on
the SEND. It is out of scope and named in §4.

**System lanes (proposal 45).** The master lane and send lanes ARE tracks with
a gain stage, so they get track pan for free, and `set-track-pan` accepts them
exactly as `set-track-volume` does (`docs/ACTIONS.md:59`). The CONDUCTOR lane
contributes no audio by construction (45: no clips, and the master lane's own
`twTrackMix` drives nothing), so a pan on it is precisely the inert knob this
proposal exists to remove: **`set-track-pan` REFUSES the conductor lane.**
§6 asks the reviewer whether `set-track-volume`'s "accepted" row for the
conductor is the same defect.

**Event clips.** `SMidiCut` is not an `SCut`, so `set-clip-pan` already REFUSES
an event clip (`panTargetCut`'s `dynamic_cast<SCut*>`). That stays. An
instrument's audio is under TRACK pan. Per-note pan (CLAP note expressions) is
proposal 37 territory and out of scope. M1 gates the refusal so it is on
record rather than accidental.

### D3. `self:Pan` automation

| Property | Answer | Why |
|---|---|---|
| Owner | `STrack` (every user lane, master, send; conductor refused as in D2) | 37 §4.5's table already reserves `self:Pan` on `STrack` |
| Value domain | **[-1, 1], the stored `pan_` unit** | The same "a lane's domain is its target's own" rule as `self:Volume` in dB. No second unit exists anywhere in the tree to disagree with |
| Interpolation | Step / Linear / Exp exactly as `twAutomationCurve` evaluates them, **in pan-value space**; the law is applied to the interpolated VALUE per frame | So a Linear sweep follows the cos law, which is correct. Interpolating the two GAINS separately would be a second implementation of the law |
| Before the first point | holds the first point's value | The universal convention. Pan has no "structural" meaning to preserve, unlike `self:Muted` |
| Trim vs Read | **Trim: `clamp(static + curve, -1, 1)`. Read: the curve alone** | Mirrors inv. 21's "TRIM SUMS" in the target's own domain. Clamping happens ONCE, after the sum |
| Invalidation class | **INFINITY, EXACT range** | It lives in `twGainStage`, which is pure in position. The range is exactly the pages the curve moved, as `pushTrackAutomation()` already does for volume |
| Snapshot | `shared_ptr<const twAutomationCurve>` in the gain stage's `Envelope`, read ONCE per page | inv. 19. The pump plan snapshots the same `Envelope`, so a pan lane is heard live for free (D6) |
| Read-family redirect | `set-track-pan` on a Read-family lane writes a POINT at the locator | The P5 rule for `set-track-volume` / `set-track-mute`: otherwise the knob moves, the render does not, and undo carries a step nobody can hear |
| Recording | Touch/Latch/Write through `SAutomationRecorder`, one `set-automation-points` per gesture | 37 P6. The pan knob is a third control feeding the same pass (M4) |
| `cut:Pan` (a clip envelope) | **NOT in this proposal** | Named in §4. The `cut:Gain` plumbing would carry it, but nobody has asked, and every extra target is another thing to gate |

### D4. Mounts: ONE control spelling, and every mount is a mount of it

`app/timeline/spanscale.h` is the `sfadercurve.h` precedent. It is the one
place that maps a pan value to a control. `app/mixerui` already includes
`app/timeline/sfadercurve.h`, and the clip properties panel is in
`app/timeline`, so that layer is visible to every mount.

| Function | Answer |
|---|---|
| value ↔ integer tick | `tick = round(p × 100)`, `p = tick / 100`, over -100..100. It round-trips EXACTLY (unlike the fader, whose `sDbToFader(0.0)` is tick -191 = +0.0625 dB) |
| text | `C` at 0; `L37` / `R100` otherwise. The same text in every mount, in the tooltip, and in `describe()` |
| reset | `sdefaultreset::onDoubleClick` to exactly 0.0, committed DIRECTLY rather than through the tick (the `applyVolumeDb` precedent) |
| disabled state | at width ≠ 2 or on the conductor lane, with a tooltip naming why (D1/D2) |

| Mount | Control | Commits |
|---|---|---|
| Arranger track head (`SSMVMixerControl`) | a small knob at Full density, hidden at lower densities (the M/S/R button density rules) | `set-track-pan` |
| Track Detail dock (`STrackDetailPanel`) | a horizontal slider under the volume row, outside the scroll area (fix/detail-pane-layout's "what you watch while playing") | `set-track-pan` |
| Mixer strip (`SMixerStrip`, 48 D8) | a knob above the fader; **the disabled "Pan (not implemented)" context entry is DELETED** | `set-track-pan`, stamping the strip's own arrangement (48's rule) |
| Clip Properties (`SClipPropertiesPanel`) | the existing spin box becomes a slider + value (QBX-84 per the ticket text) | `set-clip-pan` |

The generic plugin editor is untouched: plugin parameters are not pan.

### D5. Byte identity: pan 0 does NO arithmetic, and the file is already pan-bearing

**The goldens.** Both goldens (`mc_mono.qxp`, `mc_stereo.qxp`) carry `pan='0'`
on all 23 objects, and no committed `.qxp` fixture anywhere under `tests/`
carries a non-zero pan (grep over `tests/**/*.qxp` at `1fe23b98`). The render
path must therefore be byte-unchanged at pan 0. That happens **by construction,
not by luck**, in two places:

- **`twTrackMix`:** `hasGain = gainCurve || gainScalar != 1.0 || !fade.none()`
  gains `|| clip.pan != 0.0`. At pan 0 the clip takes the same `mixFrom`
  straight copy it takes today, with no multiply by 1.0. The law is never
  called with p == 0 on that path.
- **`twGainStage`:** inv. 20's "at 0 dB unmuted a PURE COPY" becomes "at 0 dB,
  unmuted, pan 0 and no pan curve, a pure copy". `isFlat()` covers the pan
  curve too, so a flat pan stretch degenerates to two scalars, and at pan 0
  to none.

**The file format needs no change.** `pan='…'` has been written on every
`SObject` element for as long as the serializer has existed, so a track's pan
reuses it with no new attribute, and every existing file still reads. The only
new XML is a `<lane target="self:Pan">`, written only when one exists (37 P5's
"writes NOTHING when there are no lanes"). The writer is a `QTextStream`
(C locale) and the reader is `QString::toDouble` (C locale), and
`clip_volume_pan.qxa` section 4 already round-trips `0.75`. See T9 for the
parse sites a NEW verb must not add.

**What DOES move, under licence:** `clip_volume_pan.qxa` sections 3 and 4
(T1). Nothing else is expected to. `detail_pane_reset_defaults.qxa` renders with
a pan of 0.75 in effect, but only compares panned renders with each other, so
it should survive. M1 must verify that, not assume it.

### D6. Monitoring, rendering, the live lane and sends

**Pan is applied IN THE GRAPH, upstream of `twSpeaker`'s reduction.** At
width 2 the reduction is the identity (`L = ch0, R = ch1`), so the monitored
image and the rendered image are the same pages. At width 1 or width > 2 pan
is transparent (D1), so the reduction has nothing to lose. Render and monitor
still share no code, and neither needs to.

**The live lane (proposal 21) hears pan through the shared arithmetic.** The
pump applies `twGainStage::applyGain` over the plan's `Envelope` snapshot
(`twlivepump.cc:378`). Track pan and `self:Pan` join that `Envelope`, and
`applyGain` gains a REQUIRED channel parameter (T5). A monitored track is then
panned identically before and after the hand-back. Clip pan does not arise on
the live lane: an armed audio input is not a clip.

**The master lane under monitoring (45 D4a).** A non-zero master pan or a
`self:Pan` lane on the master is a scale the linear split cannot see.
`twMasterChainState` gains `pan` and `panAutomated`, and `checkMasterShape`
answers `laneClosure("the master lane is panned")` /
`"…pan is automated"`. A CONSTANT pan is linear, so the split algebra would
technically hold. The check stays strict for the reason it gives for a
constant fader: "a scale is a fader, faders grow curves". One rule, applied to
one more control.

**Sends (proposal 47).** Covered by D2's tap points. A send lane's OWN track
pan positions its return in the master. Nothing about the cycle refusal,
`rewireSendBuses` or the unity-level requirement changes.

**Meters.** The metering tap is the track's root (`twRewire`, post gain
stage), so the head's two lanes show track pan and clip pan immediately, with
no metering edit. That is proposal 34's "read by position" design paying off
again.

---

## 2. Traps

| # | Trap | What it costs if missed |
|---|---|---|
| T1 | **`clip_volume_pan.qxa` §3 asserts pan is INAUDIBLE** (`assert-file-identical cvp_panned.wav cvp_unity.wav`), and §4 asserts channel 0 at the unpanned level after a reload with `pan="0.75"` | M1 fails its own suite. The fix is to REWRITE both sections under a licence recorded in the case header (the goldens-refreeze discipline), never to delete them: §3 becomes "pan 0.75 attenuates channel 0 by exactly `cos(0.375π)` and leaves channel 1 byte-level"; §4's band moves accordingly |
| T2 | **`SObject::setPan()` invalidates NOTHING** (`sobject.cpp:91`), unlike `SCut::setVolume()` | The edit is inaudible until some unrelated edit bumps the chain, and a gate that renders only AFTER the edit finds a cold cache and passes. `SCut::setPan` needs the `setVolume` override's `invalidateRenderPathRange`. The gate must render BEFORE and AFTER in one process (the wrapped-take-window lesson) |
| T3 | **`cos(π/2) ≠ 0` and `cos(π/4)·√2 ≠ 1` in floating point** | "Hard left" leaks -324 dB into the right channel. That is harmless to the ear and FATAL to an `assert-audio-energy maxRms="0"` gate. The law defines 0 and ±1 exactly and short-circuits p == 0 |
| T4 | **A pan gate over `test_sawtooth.wav` or `test_autosaw.wav` proves the LAW but not the IMAGE** (proposal 36 trap 22: two identical channels) | Fine for "channel 0 fell by cos(pπ/2)". Useless for "a stereo source's image is balanced, not collapsed". That claim needs `test_stereo.wav`'s 6 dB ladder |
| T5 | **The live pump calls `applyGain(ch, ch, frames, pos, t.gain)` with NO channel index** (`twlivepump.cc:378`) | If `applyGain` gains an optional channel defaulting to 0, the pump compiles unchanged and pans BOTH channels with channel 0's gain. A monitored track then snaps to a different image at disarm. The parameter must be REQUIRED, so the build breaks at the call site |
| T6 | **`checkMasterShape` cannot see a master pan** | The linear split sums an unpanned live lane onto a panned root page, so the arrangement is balanced and the monitored input is not. It is 45 D4a's defect in a new field. M2 adds it to `twMasterChainState` |
| T7 | **`twTrackMix`'s `hasGain` gate** | A panned clip at 0 dB with no curve and no fade skips the scaling branch entirely, and the pan is inaudible on exactly the commonest clip there is |
| T8 | **The clip loop's per-channel factor must index the OUTPUT channel `c`, not `srcCh`** | `srcCh` is clamped (`twPageClampChannel`), so a mono clip has `srcCh == 0` for every `c`. Indexing the law by `srcCh` gives both output channels the LEFT gain, and a hard-right mono clip goes silent |
| T9 | **Locale** (`LC_NUMERIC=de_DE` on the Linux box) | `QTextStream` and `QString::toDouble` are C-locale and safe. A new `std::stod` / `strtod` / `atof` anywhere on the pan path (a verb's `readXml`, an env knob) collapses `0.75` to `0`, and the "a pan of 0 does no arithmetic" rule then hides the bug completely. Parse through `parseDoubleInvariant` or `QString` |
| T10 | **inv. 20's pure-copy test must include pan** | A track at 0 dB with a non-zero pan takes the copy path and the track pan is inaudible. That is T7 at the other stage |
| T11 | **The flat-path optimisation and a pan curve** | `isFlat()` must say "flat" only when volume, mute AND pan are all flat over the span. Otherwise a pan sweep renders at its first frame's value for a whole flat-volume page |
| T12 | **A `set-project-channels` shrink or grow with pans stored** | Must succeed (D1). If it silently makes pans inert, a user hears their stereo mix collapse to centre on a width change with no explanation. It logs once and disables the controls |
| T13 | **The waveform preview folds channels into one envelope** (proposal 36 B8), so a hard-left clip still DRAWS full height | Deliberate and consistent with proposal 39 M2 ("a drawn waveform describes the audio its object PRODUCES", folded). A tag or an L/R marker is a UI idea for M4's review, not a fix. It must not be "fixed" by scaling the preview by pan, which is M2's defect again |
| T14 | **An ASSET clip over a panned track** | The track's root is post gain stage, so its captured material carries the track's pan, exactly as it carries the fader today. Consistent, and called out so nobody "fixes" it |
| T15 | **`stakehelpers.cpp:61`'s collapse refusal** | It now guards something audible, so it stays as it is. It must not be read as dead code during M1 |
| T16 | **`assert-master-sums`** sums per channel | Still holds with pan, because pan is applied BEFORE the rewire. It needs no edit, and M2 should run it over a panned project to prove that |

---

## 3. Milestones

Each milestone is watched FAILING under at least one sabotage per fix before
it lands (the repo's rule). The committed goldens are byte-identical at every
milestone (D5). Every audio gate is a CLOSED FORM over `test_autosaw.wav`
(RMS **A = 0.230956** per channel over any 100-aligned window, both channels
identical) unless it says `test_stereo.wav`.

### M0 — the law, pure, and the control spelling

- `tw/mix/twpanlaw.h`: `struct twPanGains { double l, r; }`,
  `twPanGains twPanLaw(double p)` with the exact cases of D1.
- `app/timeline/spanscale.h`: tick / text / reset spelling (D4).
- **AC0.1** `twPanLaw(0) == {1, 1}` bit-exact; `twPanLaw(±1)` far side `== 0.0`
  exactly; `twPanLaw(0.5).l == cos(π/4)` to 1e-15; monotone over 201 ticks;
  `twPanLaw(-p)` mirrors `twPanLaw(p)`.
- **AC0.2** tick ↔ value round-trips exactly over -100..100; text is `C`,
  `L1`…`L100`, `R1`…`R100`.
- **Gates:** `panlaw_test` (ctest). Nothing audible moves. The whole suite is
  unchanged.

#### M0 as executed (QBX-108)

- **The gate is TWO binaries, not one.** `panlaw_test` is an engine module test
  (`tw_module_test`, links `tw_mix` only) and carries AC0.1. AC0.2 is
  `panscale_test` in `main/`, because `tw303a/` may not include an app header
  and the law must not wait on an app link to be tested.
- **Two answers the design left open, now written down.** A NaN pan is centre
  and a value outside [-1, 1] is hard pan, in both the law and the spelling, so
  no input can produce a gain above 1 or a NaN sample. `sPanText` spells the
  TICK, so a stored value between ticks (0.004) reads `C`, the value a control
  would show.
- **Nothing calls either header yet.** The `twpanlaw.h` doc and
  `tw303a/mix/CONTRACT.md` state the caller's D5 obligation (no multiply at
  pan 0) so that M1 cannot satisfy the law and still break byte identity.

### M1 — clip pan is AUDIBLE (the stored `pan_` stops lying)

- `ClipEntry` gains `pan`; `refreshClipGainCurves()` pushes it; the clip loop
  applies `twPanLaw` per OUTPUT channel inside the existing product (T7, T8).
- `SCut::setPan` override invalidates like `setVolume` (T2).
- Transparent at width ≠ 2. `set-clip-pan` REFUSES a non-zero value at
  width ≠ 2 with a `TW_LOGW` (0 is always accepted, so a reset never fails).
- `set-project-channels` logs the inert-pan count once (T12).
- `clip_volume_pan.qxa` §3/§4 rewritten under a recorded licence (T1).
- **AC1.1** pan 0.75 on a clip: ch0 RMS = `A·cos(0.375π)` = **0.088384**,
  ch1 = **A**, and a render taken BEFORE the edit in the same process is
  unchanged (T2).
- **AC1.2** pan -1: ch1 is **exactly 0.000000**, ch0 = A (T3).
- **AC1.3** a mono-source clip and a stereo-source clip (`test_stereo.wav`)
  under the same pan each follow the law per channel, and the stereo image's
  ladder ratio survives on the near side (T4).
- **AC1.4** pan back to 0 → `assert-file-identical` to the never-panned render
  (D5).
- **AC1.5** a wrapped take column pans by the WRAPPER's value, a direct column
  by the active take's (D2 resolution rule).
- **AC1.6** `set-clip-pan` on a 1- and a 6-channel project with pan 0.5 is
  REJECTED and announced (`assert-log`, `minCount="1"`); pan 0 is accepted.
  `set-clip-pan` on an event clip is REJECTED (D2).
- **AC1.7** the goldens re-render byte-identically.
- **Gates:** `qxa.clip_pan_audible`, `qxa.clip_pan_width_refused`, rewritten
  `qxa.clip_volume_pan`, `action_roundtrip_test` (accept/refuse rows).
  **Sabotages:** drop the `hasGain` term; index by `srcCh`; drop the
  `setPan` invalidation; let p == 0 through the law.

#### M1 as executed (QBX-109)

- **Measured against the closed forms** (A = 0.230956): pan 0.75 gives ch0
  **0.088383** (0.088384 due) and ch1 **0.230956**; pan -1 gives ch1
  **0.000000**; the stereo ladder under +0.5 gives **0.353546 / 0.249990**
  and under -0.5 **0.499987 / 0.176770**; back at pan 0 the render is
  byte-identical to the never-panned one.
- **AC1.5 needed TWO fixtures, and D2's text did not predict why.** On load
  `stakes::normalizeColumns()` FOLDS a wrapped column into a direct one when
  the fold is exact, and a wrapper with pan 0 folds. So a fixture whose pan
  sits on a TAKE loads as a DIRECT column and pans by its active take
  (`clip_pan_folded_take`), while a PANNED wrapper is refused the fold (T15's
  "the wrapper is PANNED") and is the only way to reach the wrapped rule
  (`clip_pan_wrapped_take`). The first draft of that case assumed the fixture
  stayed wrapped and failed on its own first assertion.
- **The inert-pan announcement counts OBJECTS**, reachable from the master
  root, every arrangement root and every asset body, each shared object once.
  It fires only on a change that LEAVES width 2.
- **The refusal log spells the value with `QString::number`**, not `%f`: the
  box's `LC_NUMERIC` is de_DE (T9's hazard, on the logging side).
- **`detail_pane_reset_defaults.qxa` survived unchanged**, as D5 expected but
  did not promise.

### M2 — track pan, post-FX, on every audio path

- `twGainStage::setPan(double)`; `Envelope` gains `pan`; `factorAt` /
  `applyGain` take a REQUIRED channel (T5); inv. 20/`isFlat` include pan
  (T10, T11).
- `STrack` reuses `pan_` → `onTrackPanChanged` → `setPan` +
  `invalidateRenderPath()` (proposal 45's measured rule: an epoch bump alone
  left a muted render byte-identical).
- `set-track-pan` (path-addressed like `set-track-volume`; ABSOLUTE; coalesces;
  accepted on master and send lanes; REFUSED on the conductor and at width ≠ 2).
- `twMasterChainState.pan` → `laneClosure` (T6). The pump is updated with the
  channel (T5).
- **AC2.1** track pan 0.75 → the AC1.1 closed forms on the rendered file.
- **AC2.2** **post-FX**: `tw.test.clap.stereoskew` on the track, pan -1.
  Pre-FX pan would give ch0 = **0.5A**, ch1 = 0; post-FX gives ch0 = **1.5A**
  (the skew's cross term) and ch1 = **0**. The two orders differ by 3×, not by
  a tolerance.
- **AC2.3** clip pan and track pan COMPOSE as a per-channel product.
- **AC2.4** a post-fader send receives the panned image; a pre-fader send
  receives it unpanned (`assert-audio-energy` on a project whose send lane is
  the only path to one channel's content, using `send_bus_audible`'s shape).
- **AC2.5** master pan 0.5 + an armed monitored track → the live plan reports
  `the master lane is panned` (`assert-log`, placed at `set-monitor-mode`,
  per 45's lesson) and is rendered by the pump; `liveOwnedRefusals` 0.
- **AC2.6** a monitored track with pan -1: the monitored capture's ch1 is
  silent before AND after disarm (RUN_SERIAL, `SMARAGD_CAPTURE_SPEED=1`,
  paced `file:` input).
- **AC2.7** `assert-master-sums` holds over a panned project (T16).
- **AC2.8** the conductor lane refuses `set-track-pan`; master and send lanes
  accept it and are HEARD.
- **Gates:** `qxa.track_pan_audible`, `qxa.track_pan_post_fx`,
  `qxa.send_pan_taps`, `qxa.monitor_pan_live` (RUN_SERIAL),
  `qxa.master_pan_closure`, `mix_test`/`playback_test` additions,
  `action_roundtrip_test`. **Sabotages:** optional channel defaulting to 0 in
  the pump; omit pan from the pure-copy test; omit it from `isFlat`; omit it
  from `twMasterChainState`; pan in `twTrackMix` instead of the gain stage
  (AC2.2 bites).

### M3 — `self:Pan` automation

- The lane target on `STrack`; `twGainStage::setPanCurve(curve, absolute)`;
  `pushTrackAutomation()` pushes it; exact range invalidation.
- `set-track-pan` on a Read-family lane writes a point at the locator.
- `sAutoScaleFor("self:Pan")`: [-1, 1] with a centre line; the lane samples
  through `SAutomationLane::valueAt` (37 P6's "one interpolation").
- **AC3.1** a Linear ramp -1 → +1 over 4 s: per-second RMS on both channels
  equals the law integrated over each second (closed form, computed in the
  case header).
- **AC3.2** a Step at 2 s from -1 to +1: ch1 exactly 0 over [0, 1.9 s), ch0
  exactly 0 over [2.1 s, 4 s).
- **AC3.3** Trim: static 0.5 + a flat curve 0.75 → clamped 1.0 (ch0 exactly 0).
  Read: the same curve alone → 0.75.
- **AC3.4** editing one breakpoint invalidates exactly the pages it moved
  (the `automation_edit_invalidates` shape).
- **AC3.5** the lane round-trips inline, and a project without one writes no
  `<automation>` element (the goldens).
- **Gates:** `qxa.automation_pan_ramp`, `qxa.automation_pan_trim_read`,
  `qxa.automation_pan_stereo` (`test_stereo.wav`), `action_roundtrip_test`.

### M4 — the mounts

- The four mounts of D4, all through `spanscale.h`; double-click reset
  everywhere; the mixer strip's "Pan (not implemented)" entry deleted;
  controls disabled with a tooltip at width ≠ 2 and on the conductor.
- `SAutomationRecorder` takes the pan knob as a third control (Touch/Latch/
  Write), with the fader's "offer to the recorder first" rule.
- The knob/slider DISPLAYS the read value while a Read-family pan lane exists
  (pumped from `meterTick`, 37 P6's rule).
- **AC4.1** `describeHead()` / the mixer strip's `describe()` / the detail
  dock's gain `pan=` in `spanscale`'s text, appended after the existing fields
  so every current `contains=` string still matches.
- **AC4.2** a drag on each mount commits ONE `set-track-pan` / `set-clip-pan`
  undo step; double-click commits exactly 0.
- **AC4.3** `assert-track-detail-layout` and `mixer_pane_layout` stay at
  crushed 0 / overlap 0 at every measured size (proposal 48's floors) with the
  new control in place.
- **AC4.4** a Touch pass on the pan knob over a real transport → one
  `set-automation-points`, heard through the capture backend.
- **Gates:** `qxa.pan_mounts_describe`, `qxa.pan_reset_defaults`,
  `qxa.automation_pan_write_pass` (RUN_SERIAL), the existing layout cases.

---

## 4. What this proposal does NOT build (named, so nobody infers it)

- **Surround / N-channel panning, channel roles, a fold law** (D1, 36 §8).
- **A project-level PAN-LAW CHOICE** (-3 dB centre, compensated
  constant-power, linear). There is one law. A choice is additive later, and
  it would have to keep centre-unity as the default for D5.
- **Stereo WIDTH / dual-pan** (separate L and R position controls).
- **Send pan and "pre-fader send follows main pan"** (D2).
- **`cut:Pan`** clip envelopes (D3).
- **Per-note pan** (CLAP note expressions / VST3 `kPanTypeID`).
- **Plugin delay compensation**, unchanged (37 P9).
- **An L/R indicator on the clip body** (T13).

## 5. NOT gated (by design, stated before execution)

- **What pan SOUNDS like.** The law is gated against closed forms; whether
  the cos sweep is perceptually even is a listening judgement.
- **Pixels of every mount.** `describe()` and layout geometry are gated,
  paint is not (`screenshot` is blank offscreen, as everywhere).
- **Real device output**: the capture backend is the measurement.
- **The knob's mouse ergonomics**: synthesised presses go straight to the
  handler.
- **Pan on a width change mid-playback** (the width change itself is not a
  playback-time operation today).
- **A hand-edited file with pan outside [-1, 1]**: clamped on load by
  `setPan` as today; no case.
- **Zipper noise on a static pan change during playback**: a static change
  re-freezes pages exactly as a fader change does, and no ramp is added for
  either. If a fader edit is not audibly zippered today, a pan edit is not
  either. That claim is carried over from the fader, not measured.

## 6. Adversarial review — REQUESTED, not yet done

Claims this design expects to be challenged, in the order I would challenge
them:

1. **"Balance, not constant-power."** A mono source panned hard loses 3 dB of
   total power. Is centre-unity really forced, or would a one-time,
   announced loudness change be acceptable in exchange for the more
   conventional law? D5's byte identity only needs pan 0 to be unity, and
   BOTH families give that.
2. **"Width ≠ 2 refuses."** Is refusing at width 1 friendlier than silently
   storing? And is REFUSING at width 4 wrong when a user's quad project really
   is FL FR RL RR? D1 says roles are a separate proposal. The reviewer should
   say whether refusal makes a 4-channel user's life worse than today, where
   the knob is merely inert.
3. **"Track pan in the gain stage."** Does widening inv. 24's public
   arithmetic (a required channel argument) break any other caller of
   `applyGain` / `factorAt`? `grep` at `1fe23b98` finds only the pump; the
   reviewer should re-grep.
4. **"Pre-fader send is pre-pan."** Is that what users of this app expect, or
   should a pre-fader send follow the track pan?
5. **"Conductor refuses pan."** If `set-track-volume` is ACCEPTED on the
   conductor (`docs/ACTIONS.md:59` does not single it out), that row may be
   the same inert-knob defect. That is a separate ticket, but the reviewer
   should confirm or refute it.
6. **"A constant master pan forces Closure."** Strictness copied from the
   fader. Is the cost (pump renders the master) acceptable for a control users
   set once and leave?
7. **The DAW claims.** This design deliberately cites no specific DAW's
   default pan law, because none was verified. If the reviewer knows the
   REAPER / Cubase / Logic defaults, that evidence belongs in D1.
