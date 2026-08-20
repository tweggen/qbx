# Proposal 40 — Groove resonance: an entrainment model, a compliance heatmap, and the edits it feeds

> **Status: CONCEPT DRAFT v2 (2026-08-20).** Research-grounded design sketch,
> pre-milestone. Nothing here is committed engineering; the literature numbers
> ARE checked (two survey passes, 2026-08-20, sources at the end), and the
> integration seams ARE the real ones (surveyed against the tree at
> `main` 21fa7d1). v2 incorporates the requester's decisions: the user-facing
> name is **"Feel Flow"**, the analyzed object is the post-FX internal
> bounce, the UI is a hot-spot overlay, activation is opt-in, the perceptual
> constants are configurable, and §3.5 adds the counter-tension readout.
> **v3 (same day)** incorporates an independent adversarial review (§10):
> two-pass scoring against a common frozen reference, structure-only
> training, group-delay calibration, track-level aspects in the timeline
> domain, background bounce acquisition, and a hardened M0 fixture set.
> Remaining open points are marked **DECIDE**.

Prerequisite reading: `plan/proposed/27_ANALYSIS_SIDECARS.md` (the aspect
substrate), `plan/proposed/28_WARP_MARKERS.md` (the edit path this feeds),
`plan/proposed/39_FOLDER_SUM_PREVIEW.md` (the lane-overlay + pixel-gate
precedent), `smaragd/tw303a/sidecar/include/tw/sidecar/twaspects.h`,
`smaragd/main/timeline/CONTRACT.md` inv. 1.

## 1. The idea, and the three refinements it went through

The origin is a concrete editing experience: adjusting multi-minute live-drum
material for a record by *listening repeatedly* and marking parts that sat in
the groove against parts that fell out of it — a slow, serial, human pass. The
feature is that pass made visible: activate "groove analysis" on a drum
subgroup and get a **heatmap of better- and worse-complying regions**, a
tuning-device-style readout while listening, and adjustable assumptions about
the target feel. The two natural downstream edits are **time-stretch**
(we already have warp markers driving the vocoder) and **dynamics**.

The idea was refined three times during design discussion, and all three
refinements are load-bearing:

1. **The reference is NOT a quantization grid.** The hypothesis is that
   deviation from the *listener's expectation* creates the physical reaction,
   and that different instruments being deliberately non-simultaneous on the
   same nominal beat — in a stable, characteristic way — is a carrier of feel,
   not an error.
2. **The model is a resonator, not a feature-timeline comparison.** Acoustic
   input drives a resonator whose characteristics correspond to a desired
   groove at one-bar or multi-bar periods; the resonator parameters are either
   trained from reference material or continuously adapted to the incoming
   signal (a listener without prior expectation). Working intuition: physical
   movement is how a body drains energy out of that reinforced resonance.
3. **No source/voice separation, ever.** The analysis works on the FULL
   summed signal, in the time domain, through a dense bank of
   frequency-separating IIRs. Requester's reasoning (a producer's, and a
   falsifiable claim): closely similar impulses modify the perceived groove —
   a bass drum's sweep frequency, whether it resonates, a parallel
   low-frequency sound — and any separation stage filters exactly that away.
   The "instruments" of §2.2 are therefore realized as band REGIONS of one
   signal, never as segregated streams.
4. **The pendulum tree is a MUSCULOSKELETAL claim, not a metaphor.** A body
   of joined pendulums held upright needs muscle tension to keep from
   falling in either direction; sustaining a "laid back" phase displacement
   is holding a lean, and holding a lean costs a *constant counter-tension*.
   The requester's claim: that counter-tension — its energy and its
   variance — is a direct physical metric correlating with terms like "laid
   back". §3.5 formalizes it from state the model already carries.

**The purpose, stated bluntly to refuse a misreading: this feature does NOT
rank material or predict "success" with a listener.** It is about exactly
two things: **(a) consistency** — of the material with its own established
feel — and **(b), as far as honestly possible, predicted pure PHYSICAL
effect.** Everything in §2.3 about the listener-prediction ceiling is
therefore not a limitation of the feature but a boundary it never crosses
by design.

All four refinements turn out to be where the science actually is (§2, §3). The
naive version of this feature — colour regions by distance-to-grid — is
*contradicted* by the experimental literature and would have reproduced, as a
tool, exactly the mistake the literature spent a decade correcting.

## 2. What the literature supports, and what it refutes

Two structured survey passes (microtiming/groove-perception; embodiment/
neuro/prediction), ~30 primary sources. The numbers that constrain this
design:

### 2.1 REFUTED: "human deviation from the grid creates groove"

Five independent research programs, five convergent results:

| Study | Manipulation | Result |
|---|---|---|
| Frühauf/Kopiez/Platz 2013 (N=93) | snare/kick shifted ±15/±25 ms vs quantized | quantized rated BEST; ratings fall monotonically with shift; early hurts more than late; snare shifts hurt more than kick |
| Davies/Madison/Silva/Gouyon 2013 | idiomatic microtiming scaled 5/20/40 ms | deviations DECREASED groove/naturalness for funk & samba (sig. at 40 ms); only the jazz shuffle was tolerant |
| Senn/Kilchenmann et al. 2016 (N=159) | real duo recordings, deviation scaled −100 %…+100 % | ratings peak near quantized-to-tight (quadratic peak ≈ −60 %); expansion past the played original reliably hurts; experts more irritated (η²=.205 vs .076) |
| Madison & Sioros 2014 (+ Madison 2011) | quantized vs original performances | NO correlation of groove with microtiming magnitude; groove tracked density, beat salience, syncopation |
| Datseris et al. 2019 (N=160) | jazz piano: quantized / original / doubled / inverted | quantized rated HIGHER than original (OR 1.65); doubled far lower (OR 0.23) |

**Consequence: the score must never reward deviation magnitude as such.**

### 2.2 SUPPORTED: stable inter-instrument asynchrony as the carrier of feel

- **Hofmann/Wesolowski/Goebl 2017** (jazz trios, production + listening):
  per-drummer hi-hat-vs-ride offsets of **+27.9 / +13 / +8.6 ms — stable per
  individual**, a personal signature, not noise; ride-vs-bass near-perfect
  (**+2.1 ms**); hi-hat-vs-bass "laid-back" offsets **7–26 ms, stable per
  performer PAIRING**. Listeners preferred asynchronies reduced to **under
  ~19 ms** but **rejected fully quantized outright**.
- **Danielsen et al. 2015**: 10 expert drummers, *instructed* laid-back snare
  = **~17.4 ms late at 96 bpm**, repeatable — and played **louder** (timing
  and dynamics are coupled in real performance; a tool that edits one should
  at least display the other).
- **Nelias et al. 2022** (>450 jazz solos + listening test): a **systematic
  ~30 ms downbeat delay** (≈9 % of the quarter at ~150 bpm — tempo-scaling,
  not a constant) *enhances* swing, while **random timing deviations do not
  contribute and can impair it**. The clearest systematic-vs-jitter contrast
  in print.
- **Danielsen's beat-bin analyses**: D'Angelo "Left and Right" carries a
  **55–80 ms** stable guitar-vs-drums discrepancy — structural, genre-
  defining, perceived as a wide beat, not as error.
- **Kilchenmann & Senn (motion capture)**: bodily entrainment of expert
  listeners peaked at **tight-but-nonzero** microtiming — not at zero, not at
  exaggerated. The one dataset where the optimum is measurably off zero, and
  it is a *movement* measure, not a rating.

**Consequence: the primitive is the pairwise offset distribution — its MEAN
is the feel, its VARIANCE is the error** (realized per band region, never by
separation — §3.1/§3.3). That is exactly the requester's refinement 1.

### 2.3 The measurement floors and ceilings a scorer must respect

- **JND floor**: ~6 ms absolute (IOI < 240 ms), ~2.5–6 % of IOI above that
  (Friberg & Sundberg 1995). Differences below this band are not
  discriminable; never penalize them.
- **Feel band**: stable offsets ~10–30 ms (the cluster across 2.2).
- **Fusion ceiling**: two *fast-attack* sounds (drum pairs) segregate into two
  perceptual events at **≈40 ms** asynchrony (Danielsen/London et al.); past
  it, "compliance" is the wrong category — that is articulation/syncopation.
- **P-centers, not waveform onsets**: perceived onset lags physical onset by
  an attack-shape-dependent amount (few ms for a kick, up to >100 ms for slow
  attacks; Gordon 1987, Villing et al.). A fast+slow pair fuses with the fast
  attack anchoring the compound. Comparing raw transients between a kick and
  a bass guitar misstates simultaneity by construction. The band-envelope
  front end of §3.1 lets a compound sound's effective timing EMERGE from the
  band weighting instead of being asserted by an onset picker; §6 M0
  measures how close that gets to the published P-center behaviour.
- **Low voices deserve more weight**: timing information is encoded better
  for lower pitch (larger MMN to timing deviants in the low stream; tapping
  follows the low stream — Hove et al. 2014, PNAS). Weight kick/bass residuals
  above hat residuals.
- **The honesty ceiling**: in the largest ecological dataset (Senn et al.
  2018 — 665 raters × 248 real drum patterns), *composed-pattern* features
  predict almost nothing of groove ratings (syncopation R²=.010, density
  R²=.011) while **listener attitude/familiarity dominates (R²=.152)**. The
  strongest audio-feature correlates of groove are *performance/production*
  measures — attack sharpness r=.51, RMS-envelope variance r=.39, low-band
  spectral flux r=.35 (Stupacher/Hove/Janata 2016). **Consequence: the UI
  must present a "timing & energy signature", never a predicted groove
  rating.** The tool tells you where the material departs from its own
  established feel; whether the feel is good is the producer's ear's job.

### 2.4 The resonator framing has a literature of its own

The requester's refinement 2 is not a metaphor invented here — it is the
model class the field uses:

- **Neural resonance theory** (Large): rhythm perception as entrainment of
  nonlinear oscillators; meter as resonance pattern. EEG frequency-tagging
  (Nozaradan et al. 2011/2012) shows the brain *adds* energy at beat/meter
  frequencies that are weak in the stimulus — expectation is a resonance the
  input reinforces.
- **Engineering precedent**: Scheirer 1998 tracked beats with a bank of
  resonators driven by subband envelopes; adaptive-frequency oscillators
  (Righetti et al. 2006) entrain their own frequency to the input; PLP
  (Grosche & Müller) is a time-resolved pulse-salience curve — a linear
  cousin of the heatmap backbone below.
- **The body as resonator**: preferred-tempo data fits a resonance curve
  peaking near 2 Hz / ~120 bpm (van Noorden & Moelants 2001); optimal groove
  tempo measured at 107–126 bpm (Etani et al. 2018). Motion-capture:
  movement decomposes into eigenmodes locked to metrical LEVELS — bar-level
  sway, 2-beat limb swing, beat-level bounce (Toiviainen et al. 2010, 11 of
  12 eigenmovements frequency-locked); audio features predict body parts
  (pulse clarity ↔ core movement r=.67, low-band flux ↔ head speed r=.73,
  percussiveness ↔ hands, Burger et al. 2013).
- **Prediction is motor**: temporal predictions flow from sensorimotor to
  auditory cortex (beta band, Morillon & Baillet 2017; ASAP hypothesis,
  Patel & Iversen 2014), and moving to the beat measurably *sharpens*
  auditory temporal prediction. Phase correction after a small timing
  perturbation is automatic and operates below conscious detection (gain
  α≈0.5 at 500 ms IOI; Repp 2005) — the body corrects for deviations the
  listener cannot report. The requester's "movement drains the resonance"
  intuition is a dissipation reading of exactly this loop.
- **What does NOT exist**: a fitted, quantitative "pleasure = f(prediction
  error × precision)" curve (the predictive-coding account is qualitative);
  a study perturbing *groove* microtiming while measuring EMG/motion-capture;
  and a matched-magnitude systematic-offset-vs-jitter A/B in one design.
  These are inference bridges, and the proposal names them as such.

## 3. The model

### 3.1 Excitation (layer 1) — the full signal through a dense IIR bank

No voice separation, by decision (refinement 3). The front end is
cochlea-shaped: **N ERB-spaced IIR band filters (N ≈ 40–128, time domain —
gammatone or cascaded biquads)** over the summed material, each followed by
rectification, compression and an envelope follower, decimated to a few
hundred Hz. What layer 2 receives is the **band×time envelope field
e(f, t)** — nothing is ever classified as kick or snare, and nothing is
discarded: a swept kick is a diagonal energy trajectory across the low
bands, a resonant tail is a slow band-local decay, a parallel sub layer is
simply more excitation where it physically is. Precedents: Scheirer 1998's
beat tracker (subband envelopes → resonators), every auditory-model front
end since, and — pointedly — Hove 2014's low-frequency timing superiority,
which originates in cochlear phase-locking, i.e. in exactly this
representation; the low-band weighting of §2.3 becomes a property of the
front end rather than a bolted-on rule. Discrete "events" exist only
downstream, at the readout layer, as band-local envelope maxima — so the
effective perceptual timing of a compound or swept sound *emerges* from the
band weighting instead of being asserted by an onset picker (the P-center
concern of §2.3, answered structurally).

**Per-band group-delay calibration is load-bearing, not a refinement**
(review finding 2.1). A gammatone-class IIR band has frequency-dependent
group delay — ~13 ms at 100 Hz against ~1 ms at 5 kHz, the size and the
sign-shape of the entire feel band — so an uncalibrated bank reports "the
low end is late" on a perfectly aligned click, and no P-center claim can be
founded on top of it. The delay has a closed form per band; it is
compensated before any timing statistic is computed, and M0 gates the
calibration on a broadband click (recovered Δμ(f) ≡ 0 across all bands).

### 3.2 The pendulum ensemble (layer 2)

The requester's body model, taken literally: the listener is a set of
**coupled pendulums**, and each pendulum p carries TWO frequency
characteristics that must never be conflated —

- a **spectral receptive field** w_p(f): which AUDIBLE bands drive it, and
- a **timeline resonance** ω_p: which movement rate it swings at,

plus damping and weak coupling to the others. Its drive is the band field
projected through its receptive field, and its dynamics (complex state z_p):

```
F_p(t) = Σ_f  w_p(f) · e(f, t)                        the pendulum's "ear"

ż_p = z_p·(α_p + iω_p + β|z_p|²) + k_p·F_p(t) + Σ_q c_pq·z_q
ω̇_p = −ε·k_p·F_p(t)·sin(arg z_p)                      adaptive, within its register
```

This merges what an earlier draft of this section kept apart (a "metrical
stack" plus a separate "body layer"): **the pendulums ARE the metrical-level
oscillators, each with its own spectral ear.** The mocap literature says the
body really is organized this way — movement decomposes into eigenmodes
locked to metrical LEVELS (torso sway at the bar, limb swing at the
half-bar, vertical bounce at the beat; Toiviainen 2010, 11 of 12
eigenmovements frequency-locked) — and the receptive fields have been
measured too: low-band flux drives head movement (r=.73), high-band
flux/percussiveness drives hands, pulse clarity drives the core (Burger
2013). A plausible default ensemble is **4–6 pendulums seeded from exactly
those tables** (bounce: ω≈beat, w(f) low-heavy; sway: ω≈bar, w(f) broad;
limbs: ω≈half-bar, w(f) high/percussive; plus one or two multi-bar units),
with the ~2 Hz body-resonance curve (van Noorden & Moelants) as the
ensemble's overall tempo weighting.

Three properties bought by the dynamics rather than by thresholds:

- **Entrainment**: within the Arnold tongue a pendulum phase-locks to its
  drive; tolerance bands *emerge* from coupling strength k_p, they are not
  hand-tuned cutoffs.
- **Two modes, one switch — and training freezes the STRUCTURE, never the
  clock** (review finding 1.3). Free-running = "listener without specific
  expectation". The **trained** mode ("this chorus grooves — hold it
  against the rest") freezes the *feel structure* — receptive fields,
  weights, the μ(f) offset pattern — while phase/frequency tracking stays
  adaptive. Freezing ω too would convert any tempo difference between
  reference and material (live drummers drift a few BPM routinely) into a
  phantom static lean, or lose lock entirely; the tool would report "laid
  back" for "the second chorus is 1 BPM faster". Training is a fit of a
  small, physically interpretable parameter set, not a black box.
- **Multi-bar feel**: the bar/2-bar/4-bar pendulums are the requester's
  "one-bar or multi-bar resonator characteristics" — a two-bar figure pumps
  the 2-bar pendulum in a way a one-bar loop cannot.

**Build order** (trap 3): linear resonators + adaptive frequency FIRST — they
already yield every readout below. The Hopf nonlinearity (β ≠ 0) only if the
linear tolerance behaviour proves too brittle; nonlinear banks have known
parameter-brittleness (spurious self-oscillation, bistability).

### 3.3 Readouts (layer 3) — the pendulum phases ARE the expectation

- **Resonance power** `R(t) = Σ_p w'_p |z_p(t)|²` — sustained pumping of the
  ensemble, rolling-normalized. **The heatmap backbone**, and under this
  model it literally reads as "how hard the body is being driven".
- **Per-event residual — against ONE common reference phase** (review
  finding 1.1). An event is a band-local envelope maximum, timed against a
  designated broad-field reference (the ensemble consensus at the beat
  level), giving a *signed residual in ms relative to the entrained
  expectation* — no grid, no tempo map, no template anywhere in the loop.
  Never against the pendulums the event's own bands drive: a
  band-specialized pendulum entrains to exactly the timing it would be
  asked to judge, and the residual self-cancels by construction. The gauge
  freedom is stated, not hidden: the absolute zero of μ(f) is set by the
  reference's receptive field; what is identifiable — and what everything
  downstream uses — is Δμ ACROSS bands and drift OVER time.
- **The timing profile across the spectrum**: per band region, circular
  statistics of the residuals — a curve **μ(f) (the feel: which parts of
  the spectrum sit early, on, late)** and **σ(f) (the jitter around it)**,
  plus their drift over time. This is §2.2's per-instrument-pair finding
  realized without segregation: Hofmann's stable hi-hat-vs-bass offset
  appears here as a stable high-band-vs-low-band Δμ — and everything a
  separation stage would have destroyed (sweeps, resonance, parallel
  layers) is still inside the measurement. Two honesty riders (review
  1.2/1.5): the μ/σ split is *tracker-bandwidth-relative* — the reference's
  coupling sets a cutoff below which slow feel modulation (phrase-level
  push/pull) lands in μ-drift, not σ — so the bandwidth is a stated,
  configurable parameter and M0 measures its rolloff; and statistics are
  pooled over a declared **region partition** (~8–12 band regions), because
  a per-band curve at 40–128 bands is not estimable from realistic event
  counts.
- **Compliance score** = resonance power, weighted by the §2.3 bands: a
  stable μ(f) structure inside ~JND…30 ms is *neutral-to-rewarded*, σ(f) is
  *penalized*, band-region offsets past ~40 ms fall out of the compliance
  category entirely (flagged as articulation, not scored as error), low
  bands weighted up (which the front end already does naturally).

### 3.4 Movement-as-dissipation — now structural, still unvalidated

Under the ensemble model this stops being a bolted-on fourth layer: the
damping term of each pendulum IS the movement outlet, and the power each
pendulum absorbs from its drive is per-body-part "predicted movement
energy" — sway energy, bounce energy, limb energy — maximal when the
material drives that pendulum's bands coherently at its rate. Honest
status is unchanged: the direct validation experiment (perturb a groove,
measure the body) does not exist (§2.4), so these per-pendulum energies are
an *interpretive display* over the same numbers, never a separate
load-bearing metric. But the display is no longer speculative garnish — it
falls out of the core for free, and it is the natural future home of a
per-body-part visualization seeded by the Burger/Toiviainen tables.

### 3.5 The counter-tension readout (refinement 4, formalized)

An entrained pendulum settles at a NATURAL phase lag δ_p relative to its
drive (fixed by detuning and damping — the steady state of a driven damped
oscillator). Material with a *feel* asks the pendulum to sit somewhere
else: the observed band-region timing implies a sustained phase relation
Δφ_p away from δ_p, and sustaining a displaced phase requires a sustained
quadrature force — exactly as holding a lean against gravity requires
constant muscle tension. Computable directly from layer-2 state:

```
c_p(t) ≈ k_p · F_p(t) · sin( Δφ_p(t) )      the counter-tension
```

- Its **mean** over a window is the *static* counter-tension — the physical
  signature of "laid back": a lean, held. Sign says which way the body
  leans (behind or ahead of its natural settling point).
- Its **variance** is *corrective* tension — the physical cost of jitter,
  the body constantly catching itself. Under the model this is the quantity
  that separates feel from error at equal displacement, and it is the
  physical reading of §2.2's systematic-vs-random distinction.

**Three qualifications from the adversarial review (finding 1.4), all
adopted.** First, c_p is meaningful only against a FROZEN reference: in a
free-running adaptive loop the adaptation law drives ⟨F·sin φ⟩ to zero —
the model "relaxes into the lean" and the static reading vanishes by
construction. The two-pass design (§4.2) provides the frozen reference in
every mode: c_p is a pass-2 readout, always. Second, the drive factor F is
a loudness confound — a crescendo with perfect timing would raise var c_p —
so the display separates the factors: ⟨sin Δφ⟩ (the lean) and ⟨F⟩ (the
drive) are shown apart, their product only as the narration. Third, stated
plainly: mathematically these are *energy-weighted residual statistics with
a physical narration*, not an independent measurement; the narration is
motivated (phase correction is automatic and sub-aware — Repp; instructed
laid-back playing is measurably louder — Danielsen 2015), computable for
free, and falsifiable the day someone runs the missing EMG/mocap
perturbation study.

## 4. Native integration

The elegant consequence of the grid-free model: **the analysis is a pure
function of the source audio**, so it fits the sidecar's content-hash keying
as-is. The tempo map enters only at the *edit-suggestion* boundary, never in
the analysis.

### 4.1 Aspects (the substrate — `twaspects.h` grows two entries)

- **`groove.res` v1** — per-hop `float32[nPendulums]` resonance power (plus
  one compliance scalar), hop = rate/100 like `loudness`. `nPendulums` is
  the ENSEMBLE size (~4–6), never the band count — at 6 pendulums a 5-min
  take is <1 MB; at 128 "levels" it would be ~15 MB of sidecar churn
  (review 4.3). Payload is the heatmap.
- **`groove.ev` v1** — per-event packed records
  `{uint64 pos, float32 residualMs, float32 confidence, uint16 region, uint16 flags}`,
  ascending; `region` indexes the declared band-region partition (§3.3, an
  analysis-side parameter); positions are frames of the ANALYZED WAVE —
  which for the bounce (§4.3) means track-timeline frames.

The params blob carries the full resonator config (levels, k, ε, band split,
weights) **and, in trained mode, the frozen trained state** — so
trained-vs-adaptive are different `paramsHash` keys and coexist in the store.
`SMARAGD_SIDECAR_DIR=off` must behave identically minus speed, as everywhere.

### 4.2 Analyzer

A pure, stateful-forward-pass function beside `twDetectOnsets`
(`twanalyzers.h`): `twGrooveAnalyze(chans, nCh, nFrames, rate, params)` →
both payloads. **Two passes, by design** (review finding 1.6, adopted as
the resolution to three problems at once): pass 1 runs the ensemble over
the material and estimates the stationary reference (entrained phase
trajectory, μ(f) structure); pass 2 scores every event against the FROZEN
pass-1 reference. That dissolves the estimator circularity (the reference
cannot absorb what pass 2 measures), eliminates the warm-up transient (no
permanently-red first bars while the adaptive loop converges), and gives
§3.5 its frozen reference in every mode — at 2× the cost of a cheap pass,
offline, still one job. Deterministic for a given build, cacheable. It is
self-contained:
the IIR bank and envelope followers are part of it, and it deliberately does
NOT consume the STFT-based `onsets` aspect — the front end is time-domain by
decision (§3.1). Cost is bounded: the pendulum layer runs on envelopes
decimated to a few hundred Hz, so the per-sample work is the filterbank, the
same order as one vocoder analysis pass.

### 4.3 Job & activation

**Opt-in per track/clip, never part of `SPlainWave::enqueueAnalysis()`'s
default import pass** — this analysis is meaningful on drum/rhythmic material
and costs a forward pass over the whole file. Activation (a `groove-analyze`
verb / a Track Detail toggle) schedules a self-owning closure on
`CaptureRevalidator::scheduleAnalysisJob` (the analysis lane; drained by
`pauseBackground()` so renders stay exact), writes the aspects, clears an
`analyzing_`-style badge, and pings `notifyCaptureRevalidated` — the exact
`SPlainWave` lifetime discipline, copied verbatim.

**The analyzed object is the INTERNAL BOUNCE — the track's post-FX signal —
by decision** (refinement 3 completed: the feel lives in the mix, and
dynamics processing and frequency distribution are part of it — a
compressor changes the envelope field, an EQ changes which pendulums are
fed). **But the bounce must NOT ride `RenderSession`** (review finding 4.1,
verified against the tree): the app's render path blocks the UI, suspends
every live lane (toggling analysis would cut a monitoring musician's
headphone feed), carries the wall-clock render watchdog, and
`pauseBackground()` drains the very analysis lane the groove job runs on.
The bounce is instead a **background track-scoped page consumer**: demands
via `CaptureRevalidator::requestGraphPages` on the track's root component,
pages read at the edge as they freeze — the same demand-driven shape the
readahead and the level meters already live on — assembled into an internal
wave that carries the aspects under its own content hash. That wave is
machine-local derived data (it lives beside the sidecar store, subject to
the same knobs; `SMARAGD_SIDECAR_DIR=off` keeps it in memory only). An
edit anywhere in the track's chain stales the bounce — observed via the
content epoch (a read, never a bump) — and the overlay must show staleness
(the `analyzing_`-badge pattern) and re-bounce on demand, never silently
display analysis of audio that no longer exists. Engineering order (§6):
the analyzer machinery lands on plain source waves first (M1, zero new
render plumbing — and permanently the cheap path for a raw drum print on
an empty chain, where bounce ≡ source), the bounce consumer immediately
after (M1b). The shipped analyzed object remains the bounce.

### 4.4 UI read path & the heatmap

- **The aspects are TRACK-LEVEL and the overlay speaks the TIMELINE
  domain** (review finding 4.2 — the v2 text had a position-domain bug:
  routing bounce data through a clip's `sourceToWarpedExact` would apply
  the warp map to data that is already post-warp). The read path: a
  track-owned analysis holder wrapping the bounce as an internal wave —
  so the sidecar read stays inside `objects/wave`, where the grant is —
  exposing `feelFlowForUi()` in the `onsetsForUi()` pattern verbatim
  (atomically-swapped shared_ptr slot, one `loadAny()` on first paint,
  MISS caches empty, job completion forces one reload).
- **The heatmap is a LANE overlay**, drawn by `STrackRendererInline` right
  after `drawChildSumOverlay` (the proposal-39 slot), mapping timeline
  position → aspect hop directly — placement arithmetic only, **no warp
  map anywhere in the paint path**. Per-column tint from the compliance
  scalar. Colour law inherited from proposal 39's pixel gate: strictly
  lighter than `laneFillColor()`, strictly darker than the clip body — a
  relation, not a palette. Nothing in the paint path may block or demand
  (timeline CONTRACT inv. 1); a missing or stale aspect paints nothing
  (stale additionally shows the badge).
- **RESOLVED: the overlay is the UI** — hot spots for editing, listening
  and overdubbing, on the lane where the work happens. No sub-lane; the
  richer per-band/per-pendulum views live in the Track Detail panel, not on
  the arranger.
- **The live "tuning device" readout** while listening: a Track Detail dock
  section (sibling of the meters) showing the compliance scalar, the
  per-pendulum energies and the μ(f)/σ(f) profile at the playhead, pumped
  from the meter tick — read-by-position from the aspect, like everything on
  that dock. It also hosts the controls: mode (adapt/trained),
  **learn-from-selection** (fit the ensemble from the in/out range —
  training is SECTION-SCOPED by this selection, because material that
  legitimately changes feel per section must be trained per range; at
  section scale the honest readout is the μ-drift display, not the
  compliance tint — review finding 3.4),
  feel-band width, and the pendulum ensemble itself (receptive fields,
  timeline resonances) — which is, verbatim, the original UX idea of
  "adjusting the assumed target by adjusting body movement
  characteristics". The physical readouts (§3.4/§3.5) — per-pendulum
  movement energy, counter-tension mean and variance — live here too.
- **Every perceptual constant is configurable**, in two tiers that must not
  be confused: *analysis-side* parameters (the IIR bank, the ensemble, k,
  ε) live in the aspect params blob — changing one re-keys and re-runs the
  analysis; *read-side* constants (the §2.3 bands: JND floor, feel-band
  width, fusion ceiling, band weights for the compliance scalar) are
  applied at READ time from `SOpt`, so tuning them retints the overlay
  instantly with no recompute. Both surfaces on an Edit → Options → Feel
  Flow page, defaults = the §2.3 literature anchors, each control naming
  its source number.
- Trained state is a USER ARTIFACT and belongs to the project — serialized
  inline on the owning track (owner-held, invisible to older loaders, the
  automation-lane discipline), never only in the machine-local sidecar.

### 4.5 The edit path (why this diagnosis is worth having)

- **Time**: per-event residuals are, after the fact, warp-marker corrections.
  A `suggest-groove-warp` pass composes the EXISTING
  `add-warp-marker`/`move-warp-marker` verbs into ONE undo macro, with a
  `strength` parameter (the `quantize-notes` convention) — and crucially it
  can target the *σ component only*: pull events toward the track's own
  established μ (keep the laid-back snare laid back; remove the wobble
  around it). That is the edit the literature endorses and the
  grid-quantize edit it forbids. Suggestions are previewed on the lane and
  applied explicitly, never automatically.
- **Dynamics**: residual-correlated level suggestions land as `cut:Gain`
  automation points (the P5 lane machinery) — motivated by the measured
  timing-dynamics coupling (laid-back hits are played louder, §2.2). v2.
- The tempo map is consulted HERE only, to express residuals in musical
  units for display and to let the suggestion UI snap; `beatOfFrame`/
  `frameOfBeat` wrappers on `twTempoMap` are the natural additions.

## 5. What this deliberately is NOT

- **Not a ranking, not a listener-success predictor — by PURPOSE, not just
  by disclaimer** (§1). The feature answers (a) is the material consistent
  with its own feel, and (b) what physical effect does the model predict.
  The user-facing name is **"Feel Flow"** — chosen over "groove compliance"
  precisely to refuse the judging reading. Senn 2018's ceiling (§2.3) is
  printed on the tin regardless: the tooltip says what the tool cannot know
  (whether the feel is good, whether a listener will like it).
- **Not a quantizer.** No path in this proposal moves audio toward a grid.
- **Not a syncopation/pattern advisor.** The inverted-U (Witek R²=.35–.43)
  is composed-pattern territory; Sioros 2022 shows the index alone
  under-determines it (inserting random syncopation does not restore
  groove). Out of scope by design.
- **Not real-time** in v1. The analysis is an offline aspect pass; the "live"
  readout reads precomputed data by position. A live adaptive resonator on
  the monitor path is conceivable on the proposal-21 pump but is a separate
  proposal's worth of RT discipline.

## 6. Milestones (sketch)

Each milestone lands behind explicit, testable acceptance criteria so that
implementation can be delegated to agents; M0's ACs are its gate list below,
and every later milestone gets its AC list written at its own kickoff,
never retroactively.

> **M0 EXECUTED 2026-08-20** (branch `feat/40-feel-flow`) — measured results
> in §11. Headline: the pendulum core EARNS ITS COMPLEXITY (decisively
> better than the baseline on fixtures d, e, g, h; equal on b, i; behind
> only on c before the σ fix, tied after). 8 of 11 AC groups green for the
> pendulum; the three failures are diagnosed, not mysterious, and two of
> them question the FIXTURE more than the model (§11.3). The warp-nudge
> listening spike ran and surfaced a finding that reshapes M4 (§11.4).

- **M0 — model spike, no UI, no aspect.** TWO estimators side by side: the
  pendulum core (§3) and a deliberately dumb BASELINE (band-region onset
  picking + a local pulse fit + median/MAD residual statistics). The user
  surface downstream (aspects, overlay, constants, suggestion macro) is
  estimator-agnostic, so **the pendulum core must beat the baseline on the
  fixture set to earn its complexity — if it does not, v1 ships the
  baseline behind the same surfaces** (review 5.3). `groove_test` (ctest)
  over *closed-form synthetic fixtures* (`gen_groove_fixture.py` beside
  `gen_auto_fixture.py`), ACs per fixture:
  - (a0) broadband aligned click — recovered Δμ(f) ≡ 0 across all bands
    (the group-delay calibration gate, §3.1);
  - (a) stable +15 ms high-vs-low band offset — recovered Δμ to ±1 ms;
  - (b) jitter swept in MAGNITUDE and in CORRELATION TIME — σ tracks
    magnitude monotonically, and the tracker-bandwidth rolloff is measured
    and recorded (§3.3's rider);
  - (c) tempo drift ±2 BPM — lock kept, the drift lands in μ-drift/ω, σ
    stays at the fixture's noise floor (the AC v2 forgot);
  - (d) a 2-bar figure — the 2-bar pendulum separates (a) from (d);
  - (e) swept vs static kick at the same nominal positions — stable timing
    for both (the requester's falsification fixture);
  - (f) two interleaved event streams with distinct stable offsets in
    OVERLAPPING bands — what band aggregation does to μ/σ is recorded; the
    acceptable answer may be "bimodality is DETECTED and flagged", never
    silently averaged (the front end's own adversarial fixture);
  - (g) events over sustained cymbal-wash-like broadband energy — σ must
    not explode;
  - (h) a fill burst and a 2-bar break — the re-entrainment footprint on
    the score is measured and bounded;
  - (i) train at 120 BPM, score at 121 — phantom μ ≈ 0 (the structure-only
    freeze gate, §3.2).
  Plus one REAL drum recording, measured and *recorded* (not asserted), the
  emergent-timing-vs-P-center measurement (§2.3), and the **warp-nudge
  listening spike** (review 5.2, runnable with existing verbs before any
  model code): a dozen hand-written ±10 ms `move-warp-marker` nudges on a
  real drum take through the real vocoder, rendered and LISTENED to — if
  σ-correction is audibly worse than the jitter it removes, M4 is rescoped
  to diagnosis-only now, not discovered later. M0 exit question: are the
  readouts stable enough to be worth a UI, and does the pendulum core earn
  its keep against the baseline.
- **M1 — aspects + analyzer + job + badge on plain SOURCE WAVES** (§4.1,
  §4.2; zero new render plumbing — and permanently the cheap path where
  bounce ≡ source, i.e. a raw drum print on an empty chain). Gate:
  determinism (same input → byte-identical payload within a build), store
  round-trip, version-orphan behaviour, `SMARAGD_SIDECAR_DIR=off` identity.

  **M1 ACs (kickoff 2026-08-20):**
  1. `twAspect` gains `GrooveRes`/`GrooveResVersion=1` (id `groove.res`)
     and `GrooveEv`/`GrooveEvVersion=1` (id `groove.ev`) with normative
     payload doc comments. `groove.res`: hopFrames = rate/100, record =
     `float32[nUnits+1]` LE (per-unit normalized resonance power + one
     compliance scalar in [0,1]). `groove.ev`: 20-byte packed LE records
     `{uint64 pos, float32 residualMs, float32 confidence, uint16 region,
     uint16 flags}`, ascending, positions in analyzed-wave frames.
  2. The params blob serializes every ANALYSIS-SIDE parameter (front end,
     ensemble, stats) in a stable binary layout; changing any one changes
     `hashParams` (asserted for at least two representative params).
  3. A pure wrapper produces both payloads from planar float input;
     ctest gates: encode→store→load→decode round trip byte-identical;
     byte-determinism across two runs; a file with a stale aspectVersion
     is orphaned on sight (deleted, reported as MISS), mirroring
     `sidecar_test`'s preview-v1 gate.
  4. `SPlainWave::enqueueGrooveAnalysis()` — OPT-IN (never called from
     `setWave`), the `analyzing_`-badge/closure-lifetime/
     `notifyCaptureRevalidated` discipline copied from
     `enqueueAnalysis()`; bails cleanly and logs when the revalidator is
     absent or the store is disabled (the `onsets` precedent — the §4.1
     "identical minus speed" ideal is deferred to the milestone that
     needs an in-memory path, recorded here).
  5. Verbs: `feel-flow-analyze` (non-undoable — analysis is not an edit
     to the arrangement) resolving a clip path to its wave and
     scheduling the job, and `assert-groove-aspect` (presence, record
     counts, a residual statistic and the compliance range against
     bounds), following the proposal-27 sidecar-verb wait pattern.
  6. qxa case `feel_flow_analyze.qxa` over the committed
     `tests/groove/a_offset15.wav`: analyze → both aspects present →
     asserted values in closed-form bounds; registered via the normal
     glob (re-configure is load-bearing).
  7. Gates: `./build.sh`; `groove_test` (still green, default mode);
     the new ctest gates; `action_roundtrip_test` unaffected;
     `check_layering.py` / `check_logging.py` clean; count reconciliation
     for the new qxa entry.
- **M1b — the internal-bounce consumer** (§4.3: background
  `requestGraphPages` demand, edge assembly, epoch-observed staleness).
  Gate: the bounce is byte-identical to a `RenderSession` render of the
  same track; an edit in the chain visibly stales the analysis and never
  silently serves the old one; activation during a live-monitoring session
  interrupts nothing.

  **M1b ACs (kickoff 2026-08-20, grounded in the seam scout):**
  1. **The bounce is a `RenderSession`-shaped loop WITHOUT the app-level
     wrappers.** The scout confirmed `pauseBackground()`, the live-lane
     suspend and the render watchdog are `SApplication::startRender`'s
     calls, not the session's — a `BounceSession` (tw303a/render) reuses
     the per-page loop (single-page demand → wait → `requestPage` with
     the `prevPage` chain — the shape that survives the non-caching-
     component hazard) on ITS OWN thread (never blocking a shared
     revalidator worker in `GraphDemand::wait`), writing float32 via the
     existing writer machinery.
  2. **Bytes → file → `setWave`, the recording path's precedent.** No
     in-memory wave ctor exists (`twSampleSource` is file-only, and the
     content hash is over DECODED planar bytes) — the bounce lands as a
     WAV in machine-local derived data beside the sidecar store; the
     analysis holder loads it by path, so hashing, sidecar keying and the
     UI-cache pattern are all unchanged. `SMARAGD_SIDECAR_DIR=off` ⇒ the
     bounce path logs and no-ops cleanly (the onsets precedent; §4.1's
     "identical minus speed" ideal stays deferred and recorded).
  3. **Per-chain pruning, never the global walk.**
     `releaseOldPagesGlobally` would prune a concurrently PLAYING graph's
     trail at the bouncer's position — the bounce calls the per-component
     `releaseOldPages(keepAfterPos)` (first production caller; gated
     today only by `graph_test`) on the bounced track's own four chain
     components, keeping the ≥4-page predecessor margin. Gate: bounded
     residency after a long bounce AND an untouched readahead during
     concurrent playback.
  4. **Staleness = the `SCut::contentEpochForCapture_` pattern verbatim**:
     snapshot `getRootComponent()->contentEpochNow()` at bounce; epoch
     moved ⇒ stale, reported via the badge/describe, never silently
     served. One counter suffices for a whole-track bounce (range edits
     still bump it).
  5. `feel-flow-analyze` gains `target=track`: bounce, then analyze the
     bounce file; aspects keyed by ITS content hash; positions in the
     aspect are track-timeline frames by construction.
  6. Byte-parity gate: on an idle project, the bounce file equals a
     `render` of the same solo track at the same format byte-for-byte
     (`cmp`), which also inherits the determinism-barrier question — if
     parity needs `beginRun()`, take it only when nothing is playing and
     record that.
  7. qxa gates: `feel_flow_bounce_parity`, `feel_flow_bounce_stale`
     (edit → stale → re-analyze), `feel_flow_bounce_while_monitoring`
     (RUN_SERIAL, capture backend: monitor stays up, zero
     `liveOwnedRefusals`, no device reopen) — plus layering/logging,
     targeted ctest, count reconciliation.

  **M1 + M1b EXECUTED 2026-08-20.** All ACs green. M1 measured: Δμ
  through the full aspect path = 17.0536 ms (matches §11.2's M0 value),
  1622 res / 635 ev records over `a_offset15`, params-hash sensitivity
  and version-orphan gated in `sidecar_test` §7. M1b measured: bounce vs
  plain render **byte-identical (3,113,852 bytes)**; staleness all four
  transitions; a bounce fired mid-monitoring completed in **18 ms** with
  monitored RMS steady (0.115478 vs 0.115470 target), zero refusals, one
  never-reopened capture; residency bounded (25 pages after bounce 1,
  +6 not +12 after bounce 2). Engine shape: `RenderSession::
  setPruneScope()` — empty keeps the export path byte-unchanged, so the
  bounce IS the render loop with per-chain pruning. Two finds paid for
  by measurement: the AC's "four chain components" missed a FIFTH cache
  (`twPluginChain` forwards to its last insert — every slot's insert is
  now in the prune scope), and **per-clip reader components are not
  pruned** — harmless on the fixtures, unbounded over a multi-minute
  track; documented in `sfeelflowbounce.h`, an M2-adjacent follow-up.
  One declared layering edge: `objects/track` → `render`/`sidecar`/
  `sources`, documented in `check_layering.py`. Suite: **256 registered
  = 253 run (all green) + 3 disabled**.
- **M2 — heatmap overlay + UI cache + pixel gate.** `qxa.groove_heatmap`
  via `assert-lane-overlay`'s discipline (the overlay colour relation), plus
  an `assert-groove` verb reading `describe()` numbers off the aspect.
- **M3 — tuning panel + trained mode + verbs** (`groove-analyze`,
  `set-groove-param`, learn-from-selection; trained state in the project
  file, `action_roundtrip_test` joins).
- **M4 — `suggest-groove-warp`** composing the warp verbs; gate: on fixture
  (b), suggestions at strength 1.0 reduce measured σ to ~0 while leaving μ
  untouched, one undo restores byte-identical anchors, and the RENDER moves
  (the gate must sit on the audio side of the seam, per house rule).
- **M5 — the full μ(f)/σ(f) profile view, the physical-readout panel
  (per-pendulum energies, counter-tension §3.5) and the pendulum-ensemble
  editor** in the tuning panel, plus the Options → Feel Flow page (§4.4's
  two config tiers).
- **M6+ (research, unscheduled)** — dynamics suggestions; the
  movement/body-part display (Burger/Toiviainen tables); live monitoring
  integration.

## 7. Traps foreseen (so they are cheap when met)

1. **Attribution is by band, not by instrument — and must say so.** The tool
   can report "the 2–4 kHz region runs 12 ms late of the low bands", never
   "the snare is late". That is a feature, not a gap — it is what was
   actually measured, and separation was rejected by design (refinement 3:
   the first swept kick would falsify a separation front end) — but the UI
   copy and docs must resist ever naming instruments.
2. **Multi-bar statistics are slow by construction**: a 4-bar oscillator
   collects ~20 evidence cycles in a 3-minute take. The heatmap's time
   resolution at long periods is inherently coarse — right for "this section
   sags", useless for "this fill". The UI must not imply otherwise.
3. **Nonlinear oscillator brittleness** — linear + adaptive-ω first (§3.2).
4. **Analysis-lane starvation**: a whole-file forward pass is minutes of
   samples; it must be chunked/yieldable enough not to starve preview
   recomputes (the recording path already paid for this lesson once).
5. **Do not touch `bumpRenderChainEpoch`** — this is a read-only analysis;
   repaint rides `captureRevalidated()` only. (The Explore pass confirmed
   the two funnels are different and the CONTRACTs say so.)
6. **`test_sawtooth.wav` cannot gate any per-voice claim** (byte-identical
   channels — trap 22's lesson generalizes: groove fixtures must be
   *constructed* with known offsets, hence `gen_groove_fixture.py`).
7. **The adaptive mode will happily entrain to a wrong metrical level** —
   half/double time, and 3:2 under shuffle (a swung hat has strong
   triplet-rate energy). Expose the locked level in `describe()` and the
   panel; never silently.
8. **The group-delay calibration is load-bearing** (§3.1): any change to
   the bank — order, spacing, envelope-follower time constants — without
   re-deriving the compensation reintroduces a ~12 ms low-late skew that
   reads as feel. Fixture (a0) is the permanent tripwire.
9. **Re-entrainment after fills and breaks is a systematic false positive
   exactly where drummers look first.** Two-pass removes the warm-up
   transient, not the mid-take one; fixture (h) bounds it, and the score
   must carry a CONFIDENCE DIP through such regions, never a red flag.
10. **Sidecar churn is shared-store churn**: every chain tweak re-keys the
    bounce analysis, and the LRU cap is one cap for every aspect — groove
    churn can evict `warp.pcm`/`onsets` entries other features rely on.
    Keep payloads small (`nPendulums`, never `nBands`), and revisit a
    per-aspect budget before M1b if measured churn warrants it.

## 8. Decisions and remaining questions

Resolved by the requester (2026-08-20):

- **UI = the clip overlay** (hot spots for editing/listening/overdubbing);
  no sub-lane; rich views in the Track Detail panel.
- **Analyzed object = the internal bounce**, the track's post-FX signal.
- **Opt-in analysis**, per track.
- **Name: "Feel Flow"** — deliberately non-judging.
- **Constants configurable** — source-level and an Options page (§4.4's two
  tiers).
- **Purpose = consistency + predicted physical effect**, never listener
  ranking (§1).

Still open:

- **DECIDE**: the presentation split — proposed: the OVERLAY shows the
  consistency axis (one tint), the PANEL shows the physical axes
  (per-pendulum energies, counter-tension mean/variance). One surface, one
  question each.
- **DECIDE**: the default pendulum ensemble — how many units, and whether
  the Burger/Toiviainen seeding (§3.2) ships as the factory preset or M0
  finds a better-behaved minimal set first.

## 9. Sources (the ones the numbers above rest on)

Microtiming/asynchrony: Frühauf, Kopiez & Platz 2013 (Musicae Sci., DOI
10.1177/1029864913486793); Davies, Madison, Silva & Gouyon 2013 (Music
Percept. 30:497); Senn, Kilchenmann, von Georgi & Bullerjahn 2016 (Front.
Psychol. 7:1487); Kilchenmann & Senn (motion-capture companion); Datseris et
al. 2019 (Sci. Rep. 9:19714); Nelias, Sturm, Albrecht, Hagmayer & Geisel 2022
(Commun. Phys. 5:145); Hofmann, Wesolowski & Goebl 2017 (J. New Music Res.
46:329); Danielsen et al. 2015 (JASA 138:2301); Câmara, Nymoen, Lartillot &
Danielsen 2020 (JASA 147:1028); "Bins, Spans, and Tolerance" (Music Theory
Spectrum 45:181); Danielsen, London, Langerød & Câmara (Ann. NY Acad. Sci.,
DOI 10.1111/nyas.70306); Friberg & Sundberg 1995 (JASA 98:2524); Friberg &
Sundström 2002 (Music Percept. 19:333); Gordon 1987 (JASA).

Embodiment/neuro/prediction: Janata, Tomic & Haberman 2012 (JEP:Gen 141:54);
Witek et al. 2014 (PLoS ONE 9:e94446); Sioros et al. 2022 (Music Percept.
39:503); Etani et al. 2018 (Front. Psychol. 9:462); Stupacher et al. 2013
(Brain Cogn. 82:127); Nozaradan et al. 2011/2012 (J. Neurosci.); Grahn &
Rowe 2009 (J. Neurosci. 29:7540); Matthews et al. 2019 (PLoS ONE
14:e0204539) / 2020 (NeuroImage); Burger et al. 2013 (Front. Psychol.
4:183); Toiviainen, Luck & Thompson 2010 (Music Percept. 28:59); Van Dyck et
al. 2013 (Music Percept. 30:349); Cameron et al. 2022 (Curr. Biol. 32);
Hove, Marie, Bruce & Trainor 2014 (PNAS 111:10383); Stupacher, Hove & Janata
2016 (Music Percept. 33:571); Senn et al. 2018 (PLoS ONE 13:e0199604);
Vuust & Witek 2014 (Front. Psychol. 5:1111); Koelsch, Vuust & Friston 2018
(TiCS); Patel & Iversen 2014 (Front. Syst. Neurosci. 8:57); Morillon &
Baillet 2017 (PNAS 114:E8913); Repp 2005 / Repp & Su 2013 (Psychon. Bull.
Rev.); Etani et al. 2024 review (Neurosci. Biobehav. Rev.).

Model class: Large (neural resonance theory; canonical model); Scheirer 1998
(JASA, resonator beat tracking); Righetti, Buchli & Ijspeert 2006 (adaptive
frequency oscillators); Grosche & Müller (PLP); van Noorden & Moelants 2001
(J. New Music Res., body resonance ~2 Hz).

Caveats carried from the surveys: pro-drummer IOI-SD figures (~3–10 ms) are
plausible but not primary-verified; the "~4 % of IOI" detection threshold is
approximate; the Câmara 2020 per-condition ms table is paywalled-unverified;
no direct groove-perturbation study with EMG/mocap exists; no fitted
error×precision curve exists.

## 10. Adversarial review log (2026-08-20)

An independent instance reviewed v2 COLD — no design-discussion context,
briefed to break the proposal, with the tree open. Its verdict: the
literature reading is solid and the UI engineering copies working
precedent; **two load-bearing weaknesses**, both fixable on paper and
neither fixable cheaply after M1: (i) a self-referential estimator whose
central identity — μ is feel, σ is error — was not identifiable as v2
specified it, and (ii) an analyzed-object acquisition path (`RenderSession`)
the feature could not live with (UI-blocking per edit, live-lane
suspension, watchdog, analysis-lane drain).

All seven demanded changes are folded into v3: (1) common-reference
residuals with a documented gauge (§3.3); (2) two-pass scoring (§4.2);
(3) structure-only training (§3.2); (4) group-delay calibration (§3.1);
(5) track-level aspects + timeline-domain paint — v2 had a genuine
position-domain bug, a double-applied warp map (§4.1/§4.4); (6) background
bounce acquisition + the M1/M1b resequencing (§4.3/§6); (7) §3.5 qualified
to pass-2-only with the loudness factor separated. The hardened M0 fixture
set — (a0), (f), (g), (h), (i), the correlation-time sweep, the warp-nudge
listening spike, the baseline comparison — is the review's second half.

Nothing in the review overturned a requester decision. Finding 5.3 ("the
dumb baseline may BE v1") stands as a live, honest outcome of M0 rather
than a rejection of the model: the surfaces are estimator-agnostic, so the
question is settled by measurement, not by taste.

## 11. M0 execution findings (2026-08-20)

What was built: `tw/sidecar/twgroove.h` / `src/twgroove.cc` (front end +
both estimators, pure C++17, no Qt/threads, in `tw_sidecar`),
`sidecar/tests/groove_test.cc` (ctest target `groove_test`),
`tests/tools/gen_groove_fixture.py` + 12 fixture WAVs and their
ground-truth `manifest.json` under `tests/groove/` (blind self-verify
26/26). Every free parameter is documented in the params structs.

### 11.1 The front end held its ACs — after a SECOND closed-form skew

The §3.1 group-delay compensation works as designed: per-region calibration
spread **0.938 ms** against the 1.0 ms bound, with the negative control
(compensation disabled) failing at **6.78 ms** — the gate was watched
failing. But implementation surfaced a second, independent 1/b-shaped skew
the review did not predict: the event picker fires on the flux
(the envelope's DERIVATIVE), whose peak provably precedes the envelope
peak by `√3/(2π·b)` for the 4-stage gammatone shape — up to ~9 ms at
40–130 Hz, the same low-late signature as the group delay itself. Both
corrections are closed-form, both applied, both permanent (trap 8 now
covers the pair). Offset recovery through the front end alone: **+15.985
ms** for a constructed +15 ms (in-code stimuli). Determinism: byte-identical
across runs.

### 11.2 The estimator A/B — the pendulum earns its keep

Both estimators (dumb baseline: autocorrelation tatum + windowed robust
local fit + median/MAD; pendulum: two-pass ensemble per §3.2/§4.2) run on
every fixture. Final AC table (pendulum / baseline):

| Fixture | AC | pendulum | baseline |
|---|---|---|---|
| a0 calibration spread ≤1 ms | **FAIL 4.53 ms** | FAIL 4.67 ms | see §11.3 |
| a Δμ = +15±1 ms | **FAIL +17.05** | FAIL +11.90 | see §11.3 |
| b σ monotone; white-12 ∈ [6,18] | **PASS 7.20** | PASS 9.49 | |
| c ramp σ ≤3 ms | **PASS 0.31** | PASS 0.18 | drift-free σ, §11.5 |
| d 2-bar ratio ≥1.5 | **PASS 1.65** | n/a | |
| e sweep-vs-static (falsification) | **PASS 3/3** (σ 0.011/0.015, Δμ 3.52) | FAIL (σ_static 10.5, Δμ 115.4) | |
| f bimodality modes 0/+20±3 | flag fires, modes **FAIL −78/+82** | flag fires, FAIL −113/+116 | see §11.3 |
| g wash σ bound | **PASS 0.21** | FAIL 49.2 | |
| h dip / re-lock ≤2 bars / σ ratio | **PASS** (re-lock 0.75 s) | partial n/a | |
| i trained phantom μ ≤2 ms | **PASS 0.15** | PASS 0.24 | |

The requester's falsification fixture (e, swept vs static kick) is the
decisive row: the baseline's onset-style picking fails it three ways; the
band-field pendulum passes all three — the no-separation front end doing
exactly what refinement 3 predicted. Everything byte-deterministic across
process runs.

### 11.3 The three remaining failures are DIAGNOSED, and two indict the fixture

- **a0/a**: the fixtures' compound clicks (80 Hz/30 ms + 4 kHz/10 ms +
  noise) conflate CALIBRATION with the stimulus's own envelope shape — a
  30 ms low burst is not an impulse, and its perceptual timing genuinely
  differs from its construction time (that is P-center physics, §2.3, not
  estimator error; the front end's own envelope-matched in-code stimuli
  pass at 0.94 ms). A per-region amplitude bleed gate (12 dB, implemented,
  documented) measurably does NOT fire here — within a region every event
  has identical amplitude, because the same click repeats every beat; the
  bleed is cross-region and uniform. **DECIDE**: re-cut a0/a with
  envelope-matched bursts so they measure calibration alone (and add a
  separate, assertion-free P-center measurement fixture), or accept ±5 ms
  as the honest bound for compound-stimulus region timing.
- **f**: an OCTAVE ERROR in period recovery — X (300 Hz) and Y (350 Hz)
  alternate, so autocorrelation prefers 2× the true pulse (measured
  0.500001 s vs 0.250 s), and the mode centers inflate downstream.
  Preferring smaller lags fixes f but broke i's tatum precision when
  tried — the two needs are in tension; a real resolution (multi-hypothesis
  or GCD-based period selection) is M0 follow-up work, recorded, not
  forced.
- **Baseline-only failures (e, g)** stand as the measured argument that
  the pendulum's shared dynamics earn their cost over per-window refits.

### 11.4 The warp-nudge listening spike — one finding reshapes M4

Run against the MAIN checkout's binary with existing verbs (no M0 code in
the loop), ±10 ms `add-warp-marker` nudges on the swept/static kick
fixture. Nudges land within ~1–2 ms of intent; edits are provably local
(regions outside the nudges byte-identical between nudged and control);
the only nudge artifact found is a quiet pre-transient dulling (−9/−10 dB
in a region 25–50 dB under the hit) on nudge-EARLIER swept kicks. **But:
placing ANY warp marker routes the whole clip through the vocoder path,
and the identity-anchor control render differs from the plain render at
difference-RMS −2 dB relative to signal (corr 0.688).** The honest A/B for
M4 is therefore nudged-vs-control, and the M4 UX must treat "first marker
on a virgin clip" as a MODE CHANGE (the clip's sound changes once,
globally), not a local edit. Listening files are staged for the requester;
the human verdict on both deltas is still owed. Not swept: other nudge
magnitudes, spacings, real drum material.

### 11.5 Two implementation findings worth more than their bug reports

- **Forward-Euler was silently unstable** at tatum-rate ω with dt = 5 ms:
  per-step growth 1.0029 DESPITE damping, compounding to ~3.7×10⁹ over
  38 s. The exact exponential-integrator step for the linear term fixed
  it, and with it most of the pendulum's early nonsense (h's re-lock,
  e's μ). Any future oscillator work here uses the closed-form step,
  never Euler.
- **σ is drift-relative by definition** (§3.3 said it; the code now does
  it): σ = robust spread of (residual − local drift-window median). A
  tempo ramp's slowly-moving mean is μ-drift, not jitter; conflating them
  cost the pendulum AC c until redefined. Fixture b confirmed the
  redefinition does not weaken the white-jitter gate.

Also fixed en route, all general: a window-anchor rounding tie that biased
the baseline by exactly T/2 on grid-aligned material; IOI-histogram tatum
recovery displaced by wash (autocorrelation of summed flux is now
primary); a hard-coded bar=8·tatum assumption (bar-scale units now seed
from the ~2 Hz body-resonance anchor); DC bias from half-wave-rectified
flux inflating slow-unit resonance (mean-removed drive, bar units only —
applying it to the reference unit broke ramp tracking, measured and
reverted).

### 11.6 Still owed from the M0 list

The REAL drum-recording measurement (no such material is in-tree — needs a
take from the requester), the human listening verdict on the spike files,
and the a0/a fixture DECIDE above. The M0 exit questions themselves are
answered: the readouts are stable enough to be worth a UI, and the
pendulum core has earned its complexity on measurement.
