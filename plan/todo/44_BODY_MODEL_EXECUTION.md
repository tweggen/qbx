# Proposal 44 — EXECUTION PLAN: correcting the body model and the derived groove computations

> **Status: PLAN v2 (2026-08-27), not started.** v1 was reviewed by a Fable
> instance; v2 is the revision. Six defects it found are fixed here and named at
> the bottom under "What the review changed", including one — every ensemble unit
> is already taken, so re-mapping the head to any of them makes two body parts
> draw the IDENTICAL series — that invalidated v1's recommended option for C1. Derived from
> `plan/proposed/44_BIOMECHANICAL_BODY.md` (the concept) and
> `plan/proposed/44_BIOMECHANICAL_BODY_VERIFICATION.md` (the measurement of
> what ships today). This document is the executable half: milestones **C0–C6**,
> each with acceptance criteria a Sonnet instance can implement and check
> without re-deriving the design.
>
> Precedent for the split: `plan/todo/08_PLUGIN_HOSTING_EXECUTION.md`.

## How to use this document

Every milestone is **independently landable** and ships behind its own PR. A
milestone is DONE when every AC is green AND the standing gates below pass.
ACs are written to be mechanically checkable: each names the file, the command,
and the number.

**Standing gates, every milestone, no exceptions:**

| Gate | Command |
|---|---|
| Module boundaries | `python3 tools/check_layering.py` |
| No direct stdout/stderr | `python3 tools/check_logging.py` |
| The suite | `ctest --test-dir smaragd/build -j4 --output-on-failure` |
| Count reconciliation | `ctest -N` vs the run's summary — MEASURE, never quote |

**Watched-failing is mandatory** (house rule). Every new AC that asserts a fix
must be shown to FAIL on the pre-fix binary, and the PR body must say how it was
sabotaged. An AC that passes both before and after is not a gate; delete it or
replace it.

**A PR body must say what was NOT gated.** Physical plausibility, aesthetics and
anything needing a real body are permanently ungated here; say so rather than
letting a green suite imply otherwise.

---

## THE INVARIANT THAT GOVERNS THE WHOLE PLAN

> **The entrainment ensemble is the NEURAL layer and does not move. Everything
> corrected here is the PLANT and the DISPLAY.**

Proposal 44 §2 argues this on design grounds; here it is also the risk control
that makes the plan landable. The ensemble's residual/confidence/compliance
outputs are the one part of proposal 40 that measured well, they are what the
heatmap and the metric lab are built on, and they are keyed into the sidecar
store. If the plant changes them, no one can tell a model improvement from a
regression.

**AC-INV (applies to C0–C6, asserted in every PR):** across the whole plan the
`groove.res`, `groove.ev` **and `groove.dyn`** payloads are **byte-identical**
for the fixture set, `GrooveResVersion`/`GrooveEvVersion` stay at **1** and
`GrooveDynVersion` at **2**, and every pre-existing `feel_flow_*.qxa` case passes
with its **pinned numbers unchanged**.

`groove.dyn` is in the invariant because the pose reads `cosPhi` out of it
(`sfeelflowpose.cpp:73-84`) — v1 of this plan omitted it and would have let a
dyn-format change through unnoticed.

**The check needs a probe mode that does not yet exist, and building it is the
first task of C0.** `body_probe` gains `--hash-payloads`, printing exactly three
lines — the SHA-256 of `resPayload`, `evPayload`, `dynPayload`, and nothing else,
no fixture path or geometry that could drift. The invariant is then checked on
those three hashes alone, which leaves the probe's human-readable sections free
to change when the probe itself is edited (AC0.4 and AC1.2 both require that).

```sh
sh smaragd/tw303a/sidecar/tools/build_body_probe.sh /tmp
for f in smaragd/tests/groove/*.wav; do /tmp/body_probe "$f" --hash-payloads; done   # before
# ... land the milestone ...
for f in smaragd/tests/groove/*.wav; do /tmp/body_probe "$f" --hash-payloads; done   # after: identical
ctest --test-dir smaragd/build -R "feel_flow|groove" -j4 --output-on-failure
```

Hashing over the WHOLE fixture directory is deliberate even though several
fixtures recover a wrong-regime tatum (`e_static_only` recovers 0.6 s, four
metrical levels off). A wrong regime makes the KINEMATICS meaningless, which is
why the probe tells you to discard such a run — but the payload bytes are still
a valid determinism check, and wider coverage is free here.

The single deliberate exception is **C1**, which changes one display-side name
mapping and therefore changes the pose numbers — never the payloads. It is called
out there.

---

## C0 — The kinematic chain (display only) — **DONE 2026-08-28**

> **Executed.** `app/model/sfeelflowskeleton.{h,cpp}` is the one pure function;
> `sfeelflowpuppet.cpp` computes no geometry (no `rotateAbout`, no trig, no
> `M_PI`); `assert-puppet-skeleton` + `qxa.feel_flow_puppet_chain` gate it;
> `body_probe --hash-payloads` exists and its SHA-256 is validated against three
> coreutils vectors including a multi-block one. Measured at the default 200x400
> box: sway 0 / ±0.5 / ±1.0 gives trunk 0 / ±10 / ±20 with head stub, shoulder
> bar and both arms EQUAL to it at every angle, and the same angles out of a
> 90x140 box. Composition is a sum: sway=nod=arm=1 gives trunk 20, headStub 30,
> armL 55, armR −15. **Watched failing** by rebuilding head/shoulders/arms
> against 0 (the shipped world-coordinate construction): actions #2–#7 fail
> reporting 0.0000 against a trunk of ±10/±20, while sway=0 still passes —
> which is why the case sweeps five angles instead of asserting one.
> Suite: 324 registered / 319 run / 5 disabled, **319 passed**, 299 s at `-j4`,
> Qt 6.4.2 / Ubuntu 24.04.


**The defect.** `sfeelflowpuppet.cpp` rotates the neck about the pelvis, then
builds every segment above it in WORLD coordinates from the neck's new position,
so the head, the shoulder bar and both arms inherit the neck's translation and
none of its rotation. Measured: torso 20°, head and shoulders **0.000°**
(verification §9.1). This is a drawing bug and waits on nothing.

**The fix, and the reason it is shaped this way.** Do NOT patch `paintEvent`.
Extract the skeleton into **one pure function** that the painter and the gate
both call, so paint and assertion cannot drift — the same lesson proposal 41 M7
paid for with `tagChipRect()` and the take-lane fix paid for again with
`fillBodyByMaterial()`.

- New: `main/model/include/app/model/sfeelflowskeleton.h` + `src/`.
  `app/model` is the lowest app layer, so `timeline` (the widget) and `testkit`
  (the verb) can both reach it. It must NOT include `app/objects/track` — take
  the joint scalars and the box as plain arguments, exactly as
  `sclipwindowgeometry.h` does.
- Signature: `SFeelFlowSkeleton sFeelFlowSkeletonFor( const SFeelFlowJoints &j, const QRectF &box )`
  returning named joint points **plus each drawn segment's world angle in
  degrees**. Build every segment in **torso-local** coordinates: rotate its
  pre-rotation point about the pelvis by the trunk angle before applying the
  joint's own rotation.
- `SFeelFlowPuppetWidget::paintEvent` becomes a consumer of it and computes no
  geometry of its own.

**AC0.1** `sFeelFlowSkeletonFor` is the only place segment geometry is computed
IN THE APP. `main/timeline/src/sfeelflowpuppet.cpp`'s `paintEvent` contains no
trigonometry and no coordinate arithmetic — no `sin`/`cos`/`atan2`, no
`rotateAbout`, no `M_PI`: it consumes named joint points and draws them. Check by
reading, not only by grep — a grep for `rotateAbout` alone is satisfiable while
still computing geometry inline.

**AC0.2** New verb `assert-puppet-skeleton` (in `main/testkit`, beside
`assert-feel-flow-pose`) takes the joint scalars as EXPLICIT ATTRIBUTES
(`sway=`, `nod=`, `arm=`, `bounce=`, `hip=`, plus a box size) and evaluates
`sFeelFlowSkeletonFor` on them directly. It reports every segment's world angle
through a `describe()` grammar with ONE spelling.

> **This is a gate on the pure function, reached through the verb, and it has to
> be.** v1 said the case would "drive the pose to a known trunk angle" — nothing
> in the repo can do that. The pose is analysis-derived (`sFeelFlowPoseAt` over
> decoded aspects), no verb sets it, `assert-feel-flow-pose` only READS, and no
> fixture can be authored to land sway on exactly ±0.5 of full scale. Explicit
> attributes make the geometry testable without an analysis at all, which is the
> same discipline `tagChipRect()` established.

**AC0.3** New case `smaragd/tests/cases/feel_flow_puppet_chain.qxa` asserts,
at trunk lean ∈ {0, ±0.5, ±1.0} of full scale, that **head-stub, shoulder-bar and
both arm-root angles each equal the trunk angle to within 0.01°**. On the pre-fix
binary the non-trunk angles read 0.000° and the case FAILS at four of five rows.

**AC0.4 — REPLICA MAINTENANCE, not a gate.** `body_probe` is explicitly not a
gate, and it CANNOT call `sFeelFlowSkeletonFor`: the probe is engine-side
(`build_body_probe.sh` compiles it with `sidecar/include` + `core/include`, no
Qt) and `check_layering.py` rule 1 forbids a `tw303a` file including an `app/`
header. Its §G table is therefore a HAND REPLICA and must stay one. This AC is
that the commit updates all THREE replicas the probe carries and that each is
labelled `// REPLICA of <file>` at its site:

| Probe replica | Mirrors | Goes stale at |
|---|---|---|
| `body_probe.cc` §G skeleton math | `sfeelflowpuppet.cpp` geometry | **C0** |
| `body_probe.cc:79-81` display constants | `kSwayDeg`/`kNodDeg`/`kBounceFrac` | C0, C3 |
| `body_probe.cc:248` `partUnit[]` | `kPartUnitName` in `sfeelflowpose.cpp` | **C1** |

**AC0.5** AC-INV holds, using the new `--hash-payloads` mode. No aspect version
moves. `feel_flow_puppet.qxa`'s pinned pose numbers are **unchanged** — C0 touches
geometry, never the pose.

**NOT gated by C0:** whether the figure looks right; the head still has no
independent lag or counter-rotation (it is now welded to the trunk, which is
correct-but-crude and is C4's job); pixel aesthetics.

---

## C1 — The head's metrical level — **DONE 2026-08-28, with AC1.2 NOT MET**

> **Executed as option D.** The head is no longer mapped to any unit
> (`kPartUnitName`'s head entry is empty); it is DERIVED from the trunk through
> a one-pole lag, tau = 0.126 s, gain 0.5, integrated from the start of the
> material on every call so the pose stays a pure function of the immutable
> snapshot. `reference` goes back to being the pure residual gauge. The head
> borrows the trunk's energy. Re-pinned `feel_flow_puppet.qxa`: headNod
> **0.9262 -> -0.2210**, energySum **2.0951 -> 1.3862**, the other four
> components bit-identical. AC-INV verified across **all 12 fixtures**, all
> three payloads byte-identical.
>
> **AC1.2 IS NOT MET AND THAT IS THE MILESTONE'S MAIN RESULT.** It expected the
> head to read ~2.50 (option A) or ~0.39 (option D) against a 3.0 bound. The
> `--assert-crossover` mode, once written to measure what is DRAWN rather than
> the seeded omega, reports **head 7.02 and trunk 10.48** on `h_fill_break` --
> both FLUNG. The AC was written on the premise that the trunk oscillates at its
> seeded 0.500 Hz. It does not: see the verification note's new 9.10. Every part
> is drawn oscillating near the tatum rate because `arg(z)` is drive-locked off
> resonance, so deriving the head from the trunk cannot get it under a bound the
> trunk itself misses by 10x.
>
> C1 still delivers the reported symptom -- the head is subordinate to the torso,
> at half its amplitude and lagging it -- and the neck lag turns out to be the
> first thing in this feature that says "a heavy thing cannot follow fast
> wiggles". But the bound needs the JITTER addressed, which is a new milestone;
> see "C1a" below.


**The defect.** The head is driven by `reference`, the tatum-rate gauge — the
FASTEST unit in the ensemble. Measured 3.999 Hz against the torso's 0.500 Hz,
ratio 7.997:1, correlation +0.024. Its inertial torque is **10× its gravity
term**, i.e. it is driven **3.16× above its own crossover** (verification §9.2,
§9.3). It also contradicts proposal 40 §2's own citation (Burger 2013: low-band
flux ↔ head speed, r = .73 — that is the LOW-HEAVY unit, `bounce`).

**AND EVERY UNIT IS ALREADY TAKEN.** `kPartUnitName`
(`sfeelflowpose.cpp:12-18`) maps one unit to each of the five parts:
`bounce`→pelvis, `sway`→torso, `limbs`→arms, `reference`→head, `twobar`→hip. So
re-pointing the head at any EXISTING unit makes two body parts draw the
**identical series** — same power, same `cosPhi`, same displacement. v1 of this
plan recommended `bounce` without noticing that it would make the head bob in
exact lockstep with the pelvis, which is a visible loss of information, not a fix.

**DECIDE — requester, before this milestone starts.**

| Option | Head driven by | Drawn f | τ_inert/τ_grav | Degenerate with | Aspect bump |
|---|---|---|---|---|---|
| A | `bounce` | 2.0 Hz | 2.50 | **pelvis** | no |
| B | `limbs` | 1.0 Hz | 0.63 | **arms** | no |
| C | a NEW low-heavy beat unit | 2.0 Hz | 2.50 | none | **yes — all three groove aspects** |
| **D (recommended)** | **the TRUNK, through a one-pole lag** | trunk's 0.5 Hz | **0.39** | none | no |

**D is the answer, and it is the requester's own sentence made mechanical.** The
head's world angle becomes the trunk's angle plus a small lagging neck offset:

```
headWorld(t) = trunkWorld(t) + neckGain * ( trunkWorld(t) - lag(trunkWorld, tau)(t) )
tau = 1 / (2*pi*f_cross_head) = 0.126 s      neckGain <= 0.5
```

A first-order lag, not a plant — but it is physically motivated, it is
non-degenerate, it costs no ensemble change and no aspect bump, and it delivers
"the head is subordinate to the torso" directly. It is also the natural stepping
stone to C4's real second-order dynamics, which replace it.

`reference` then drives NO body part and goes back to being the pure residual
gauge §3.2 designed it as — it is already surfaced as confidence/compliance in
the panel, so nothing is lost from the readout, only from the puppet.

C is the only other non-degenerate option and it is the expensive one: a new unit
re-keys `groove.res`/`ev`/`dyn` together (they share one params hash —
`twaspects.h:218/260/344`) and therefore breaks AC-INV by construction. Do not
take it without a reason D cannot serve.

**AC1.1** The head's drive is implemented per the decision. If D: the lag lives
beside the pose function, is a pure function of the trunk series, and
`kPartUnitName`'s head entry becomes explicitly empty with a comment saying why.
The mapping stays BY NAME for every other part; an absent name still leaves a part
at 0 with no fallback to a neighbour (`sfeelflowpose.h` point 3 unchanged).

**AC1.2** A new `body_probe` mode `--assert-crossover` exits non-zero if either
ROTATIONAL DOF's `τ_inertial / τ_gravity` exceeds **3.0** at its drawn amplitude.
Post-C1 the head reads **≈2.50 under option A, ≈0.39 under D**; pre-C1 it reads
**10.01** and the check FAILS.

> Do not write the assertion as `<= 2.5`. The exact value under option A is
> **2.5003** — a gate spelled `<= 2.5` fails on its own success case. 3.0 is the
> bound; 2.50 is an expectation.

**AC1.3** `--assert-crossover` **names its fixture** and refuses to report when
the recovered ladder is not the 0.25 s tatum. This is load-bearing for the
watched-failing claim: on `e_static_only.wav` the ladder recovers at tatum 0.6 s
(measured: reference 1.667 Hz), so the head ratio there is **1.739** and the check
**passes on the broken binary**. Use `h_fill_break.wav` or
`a0_broadband_grid.wav`. The check covers the head and the trunk only, and says
so: `bounce` is a force ratio in g under its own bound, and `hipShift`/`armSwing`
are translations with no defined τ_gravity.

**AC1.4** `feel_flow_puppet.qxa`'s pinned pose numbers are **re-measured and
re-pinned in the same commit**, with old and new values both in the case header
and the reason stated. This is the one deliberate pose change in the plan. If
option A or B is taken against the recommendation, the header must ALSO state that
two columns are now identical by construction and that this is expected, not a
copy-paste error.

**AC1.5** The probe's `partUnit[]` replica (`body_probe.cc:248`) and its §F labels
are updated in the same commit — otherwise §B and §F silently measure a mapping
that no longer ships (AC0.4's table).

**AC1.6** AC-INV holds: all three groove payloads byte-identical, no version bump
(options A, B, D).

**NOT gated:** that the resulting head rate is *right* for a human — defensible
from the ω² argument and the cited literature, but no body was measured; and under
D, the choice of `neckGain` and the one-pole form, which are an interim stand-in
for C4 and are stated as such.

---

## C1a — The drawn phase is drive-locked, not metrical — **DONE 2026-08-28**

> **Executed as the omega branch, chosen by the requester, and the DECIDE below
> is settled.** The export is a PHASE, not raw omega: pass 1 runs a
> power-weighted PLL (`phi += omega*dt + K*sin(arg(z) - phi)*dt`, K = omega/(2*pi*8))
> and `groove.dyn` **v3** appends `cosMet`/`sinMet`. Computing it in the
> analysis, where omega and z are both in hand, keeps the pose O(1) and pure --
> exporting omega would have forced the consumer to integrate with state.
>
> The lock term is SELF-LIMITING, which is what makes a fixed gain safe: `|K
> sin(...)| <= K`, so an unlocked unit's metrical rate can never be dragged more
> than **K = 2.0% of its own omega** off, under any drive whatever.
>
> **THAT BOUND IS THE CLAIM. The first version of this line said the pull term
> "averages to ~0 over a cycle of phi and the correction cancels itself", and
> that is FALSE near the lock edge** -- found by an adversarial review of C4a's
> maths, 2026-08-30. Analytically the mean pull for a detuning `Delta` outside
> the lock range is `Delta - sqrt(Delta^2 - K^2)`, which is a maximum, not a
> zero, exactly where `Delta` is a little above `K`: simulated, the mean of
> `sin(eps)` is **0.49 at 2.5% detuning** and 0.21 at 5%, falling to 0.05 only
> by 20%. So a unit detuned by a few lock ranges (roughly 2-6% of its omega)
> phase-SLIPS, with long near-locked dwells and a beat as slow as
> `sqrt(Delta^2 - K^2)` -- a sub-0.1 Hz breathing of `phaseMetric` rather than a
> cancellation. `omega` itself adapts within `[omega0/sqrt(2), omega0*sqrt(2)]`,
> so a unit can be CARRIED INTO that window by its own adaptation.
>
> The design is sound on the bounded reading and the measured lock behaviour
> below is unaffected -- what is retired is the zero-mean argument for why. The
> other two properties the review confirmed independently: the lock range is
> `|Delta| <= K` (2.0% of omega) and the linearised pull-in time constant is
> `1/K` = exactly the 8 periods the constant names, with `K` proportional to
> `omega` making both period-invariant as intended.
>
> **AC-INV EXCEPTION, exercised exactly as scoped:** measured over all 12
> fixtures, `groove.res` and `groove.ev` are **byte-identical** (0 differing
> lines) and `groove.dyn` changed on all 12 (24 lines). `GrooveDynVersion`
> 2 -> 3; v2 entries orphan on sight and re-analyse once, as the M3c bump
> already did.
>
> **Measured.** Every part lands on its metrical level: bounceY **2.00** against
> a 2.0 seed, sway **0.50**/0.5, armSwing **0.99**/1.0, hipShift **0.25**/0.25,
> on both fixtures. AC1a.1 met: `--assert-crossover` passes on `h_fill_break`,
> `a0_broadband_grid` and `a_offset15` -- head ratio **0.15-0.21**, trunk
> **0.38-0.39**, both CARRIED. The PLL holds alignment to a few ms where a unit
> is genuinely entrained (reference: -5.1 / -4.3 / +7.8 ms end error on a 250 ms
> period, max 16.7 ms) across three fixtures including the tempo-drift one.
>
> **Two bugs found and fixed on the way, both worth knowing:** the decoder's
> per-unit offset stayed at the old 6-float stride (`u * 24`) while the record
> grew to 8, so channels scrambled ACROSS units and decoded to values outside
> [-1,1] -- caught because a cosine's rms must be 0.707 and one unit read 4.37.
> And `--assert-crossover` measured the head's TRUNK-RELATIVE offset, which
> after C1 is correctly near zero, so it was reporting a near-zero signal's
> noise as a frequency; `f_cross` asks what a segment's motion in WORLD space
> costs.
>
> AC1a.2 is VOID: the time constants come from the unit's own omega, so C1a did
> not need C2 after all. AC1a.3 met -- `feel_flow_puppet.qxa` re-pinned with old
> and new values and the reason.


**The defect, stated as physics.** A unit's resonance lives in `|z|`; its
`arg(z)` off resonance is dominated by the forced response at the drive rate.
The pose draws `sqrt(power) * cosPhi`, so every body part oscillates at roughly
the tatum however slow its metrical level -- measured up to **16x** its seed
(verification 9.10). The design's claim that the parts move at their metrical
levels holds for the ENVELOPE and not for the DISPLACEMENT.

**Do not "fix" this in the analysis.** `arg(z)` is correct for what pass 2 uses
it for (scoring an event against a common reference phase) and the payloads are
under AC-INV. This is a READ-SIDE defect.

**Candidate fix, and it generalises what C1 already built:** give every part its
own one-pole lag with a time constant from its OWN f_cross, so each body part
low-passes its drive the way a mass would. C1's neck is that mechanism applied
to one joint. Cheap, display-side, no aspect change.

**DECIDED (requester, 2026-08-28): the omega branch.** Measured before choosing,
three ways of turning a unit's state into a drawn displacement, over three
fixtures:

| | drawn rate | amplitude | verdict |
|---|---|---|---|
| raw `cos(arg z)` | drive rate for every unit | — | what shipped |
| one-pole lag | material-dependent: good for sway/twobar on one fixture, **no change** for bounce on another (3.884 Hz against a 2.0 seed) | collapses (sway rms 0.216 -> 0.055) | rejected |
| resonator at omega | partial: sway/twobar good, **bounce 3.297 Hz** against 2.0 | preserved | rejected |
| **coherent/PLL phase** | **every unit within ~2% of its seed** | preserved | **chosen** |

The lag was the plan's own recommendation and the measurement rejected it.

**AC1a.1** `--assert-crossover` reports every rotational DOF under **3.0** on
`h_fill_break` and `a0_broadband_grid`. Pre-fix it reads head 7.02, trunk 10.48.

**AC1a.2** The per-part time constants come from `twBodyCrossoverHz` (C2), not
from hardcoded numbers -- so this milestone lands AFTER C2.

**AC1a.3** AC-INV holds; `feel_flow_puppet.qxa` re-pinned once more, with the
old and new values and the reason, as C1 did.

## C2 — The measures (44 B0) — **DONE 2026-08-28, with AC2.1 NOT MET**

> **Executed.** New `tw_body` module (`tw_module(body … DEPS tw_core)`,
> `'body': ['core']` in `check_layering.py`) with `twBodyMeasures`,
> `twBodyPendulumHz`, `twBodyCrossoverHz` and a `compound()` that composes
> consecutive segments by the parallel-axis theorem. `body_measures_test`
> registered; `body_probe` now LINKS tw_body and its two hardcoded crossover
> constants are gone.
>
> **AC2.1 IS NOT MET AND CANNOT BE MET IN THIS ENVIRONMENT.** The VERIFY gate
> is the point of this milestone, and it needs primary literature. Every route
> was refused by the egress proxy: the de Leva 1996 PDF, Winter's anthropometry
> chapter, the HAS-Motion Visual3D wiki, an NCBI/PMC reference table, ExRx,
> arxiv and Wikipedia. Web SEARCH works but returns model-written summaries;
> taking a number from a summary launders a memory rather than checking it, so
> none was taken. **Every constant is marked `VERIFY` with the search recorded
> beside it**, which is the branch AC2.1 itself allows. Finishing it needs a box
> with journal access; a value that moves will move a test, by design.
>
> **AC2.2 is RECORDED, not validated**, for the same reason and per this plan's
> own correction: section 4's 0.9-1.1 Hz arm range is itself a VERIFY-flagged
> memory, and the closed form over these constants gives **0.7346 Hz** for the
> full arm (upper + fore + hand, which is what section 4 defines) against
> **1.0332 Hz** for the upper arm alone. Both are pinned as regression values
> with an explicit "not validated" label, and the test asserts the SANITY
> relation (the longer compound is slower) so the 40 % gap is believable rather
> than a transcription slip. Asserting an unverified range is exactly what made
> the v1 milestone unresolvable.
>
> **AC2.3 MET:** head **1.2640 Hz at 10°**, trunk **0.8002 Hz at 20°**,
> reproducing the verification note to 0.01 Hz — and the amplitudes are now
> asserted alongside, since the crossover depends on them through sin θ/θ and a
> crossover quoted without one is unreproducible.
>
> **AC2.4 MET:** monotone in M and H at (50, 1.55) and (110, 2.00), plus two
> sharper claims — pendulum frequency is **independent of body mass** (1e-9) and
> scales as **1/sqrt(H)** exactly.
>
> **Watched failing** under two sabotages: dropping sin θ/θ from the crossover
> fails the amplitude-monotonicity assertion; dropping the parallel-axis d²
> term fails four assertions at once (head 2.457 against 1.264, upper arm 2.376
> against 1.033). Suite 325 registered / 320 run / 5 disabled, **320 passed**,
> 285 s at `-j4`.


**New module** `tw303a/body/` → `tw_body`, linking `tw_core` only. Pure,
deterministic, no Qt, no threads, no I/O — the sidecar rules. Register in
`tw303a/CMakeLists.txt` via `tw_module(body ... DEPS tw_core)` and add the edge
to `tools/check_layering.py`.

`tw/body/twbodymeasures.h`: `twBodyMeasures { M, H }` → per-segment
`{ mass, length, comFromProximal, radiusOfGyration, inertiaAboutProximal }`.

**AC2.1 — THE VERIFY GATE, and it is the point of this milestone.** Every value
in proposal 44 §3's table is checked against a PRIMARY source and the citation —
author, year, table number — is written **beside the constant in the header**.
Any value that cannot be sourced is either replaced with one that can be, or
kept and marked `// UNSOURCED` with what was searched. The PR body lists every
value that moved. Proposal 44 §3 currently carries a blanket **VERIFY** flag;
this milestone removes it or explains why not.

**AC2.2 — the closed forms, and ONE of them is a trap v1 walked into.**
`body_measures_test` (ctest) asserts the closed-form natural frequencies against
ranges taken from PRIMARY sources, cited in a comment beside each assertion:

- **vertical bounce** `f = (1/2π)√(k_leg/M)`: **2–3 Hz** at 20 kN/m / 75 kg.
  Computed from §3's own values: **2.599 Hz**. Inside. No action.
- **arm as a compound pendulum about the shoulder**: 44 §4 quotes **0.9–1.1 Hz**
  and flags it **VERIFY**. Computed from §3's own table, it does not land there,
  and which arm you mean decides it:

| Pendulum | m | d (CoM from shoulder) | I | f |
|---|---|---|---|---|
| **full arm** (upper + fore + hand) — what 44 §4's DOF table says | 3.675 kg | 0.3381 m | 0.5721 | **0.7346 Hz** |
| upper arm alone | 2.025 kg | 0.1888 m | 0.0890 | **1.0332 Hz** |

> v1 of this plan pinned 0.9–1.1 Hz AND forbade widening it, which made the
> milestone unresolvable: the DOF 44 §4 defines is the full arm, and the full
> arm's closed form is 0.7346 Hz. Turning a **VERIFY**-flagged number into a hard
> build gate is the defect; AC2.1 is the milestone that exists to resolve it.

So the AC is: **verify the range against a primary source, then assert the
closed form for the DOF the plant will actually integrate lands in it.** If the
sourced range and the computed value disagree, the milestone resolves WHICH is
wrong — the range (remembered for a different pendulum) or §3's table values —
and the PR body says which, with the source. Do not widen a range to fit, and do
not pin a value whose provenance is a memory.

**AC2.3** `f_cross` per DOF is a public function
(`twBodyCrossoverHz( segment, amplitudeRad )`) — the closed form
`√(m·g·d·sin θ / (I·θ)) / 2π` — unit-tested at the verification note's own
numbers **and at the amplitudes that produce them**, since `f_cross` depends on
amplitude through `sin θ / θ`: head **1.264 Hz at 10°**, trunk **0.800 Hz at 20°**,
both ±0.01 Hz at M = 75, H = 1.75.

**AC2.4** Scaling is exercised, not assumed: `twBodyMeasures{ M, H }` at
(50, 1.55) and (110, 2.00) produces monotone masses and lengths and f_cross
values that move in the right direction. No hardcoded 75/1.75 anywhere outside
the defaults.

**AC2.5** AC-INV holds trivially — C2 adds a module and changes no existing path.

**NOT gated:** whether the sourced values describe the *requester's* body; whether
a torque-actuator idealization is adequate (44 §6 names it as a standing limit).

---

## C3 — The DOF set: the three anatomical axes, and a neck — **MODEL + PROJECTION DONE 2026-08-28**

> **DECIDE 2 settled by the requester: neither of the plan's two options.** The
> plan offered a 3/4 OBLIQUE PROJECTION of a 2D figure or TWO ORTHOGONAL VIEWS.
> The requester asked instead for the model to be **technically 3D** with a
> **wireframe projection** — which is better than either, because an oblique
> projection of a 2D figure still has no third axis to project.
>
> **Done:** every joint is a point in body space; the three anatomical
> rotations are named by what they do (`rotFlex` about x = flexion/extension,
> `rotLateral` about z = lateral flexion, `rotAxial` about y = axial rotation)
> and compose through one `trunkFrame`, so a child inherits its parent's
> orientation as a transform rather than by copying an angle.
> `sFeelFlowProject` is a separate orthographic camera producing wireframe
> polylines — orthographic on purpose: a verification device should not add
> perspective foreshortening to the thing being verified. The head is a real
> wireframe sphere (three great circles, generated in 3D and projected), so
> head facing is readable; the ground is a plate so the vertical bounce has
> something to read against; painter's-algorithm ordering by depth.
>
> **THE ARMS MOVED PLANE, and that is a correctness fix the verification note
> called for:** an arm swing is SAGITTAL and was drawn as lateral abduction, a
> jumping-jack. `armL`/`armR` now stay at the trunk's inherited lean and the
> swing appears in `armSwingL`/`R`.
>
> **Still NOT driven:** `trunkFlex` and `trunkTwist` exist, default 0, and are
> wired through the model, the projection and the verb — but no pose component
> commands them. C3's remaining half is finding a drive, not building an axis.
> A reader must not mistake "the axis exists" for "the body bends forward".
>
> **Measured.** Three axes independently observable: `sway=1` → trunk 20 / flex
> 0 / twist 0; `flex=1` → 0 / 25 / 0; `twist=1` → 0 / 0 / −20. And they COMPOSE
> as rotations, not as numbers: `sway=1 flex=1` gives **21.8802 / 25.0000 /
> −8.7447** — a cross-term neither command asked for, which is the signature of
> real composition. An arm commanded 35° under a 20° lean reads **36.6915°** in
> world space (`atan2(sin35, cos35·cos20)`), where a componentwise fake would
> read exactly 35. C0's inheritance identity survives the rewrite unchanged.
>
> **Watched failing:** replacing the composed `trunkFrame` with two independent
> flat angles fails five assertions. One bug found and fixed: `shoulderBarDeg`
> sign-flipped, because body space is y-UP while the 2D original measured it in
> screen space, y-down.
>
> Suite 320 passed, 274 s at `-j4`. **NOT gated:** what the figure looks like,
> the camera angle, and whether a wireframe is a better verification device than
> a filled one — that is the requester's judgement and the reason this was built.


**The defect.** All five shipped DOF live in one screen plane, so the model
**side-bends about the fore-aft axis** and has neither sagittal flexion nor axial
rotation (verification §9.7). The requester's stated core interaction — the trunk
bending FORWARD and returning upright, the head falling with it and being pulled
back up — has no degree of freedom at all, and proposal 44 §4's own table would
not deliver it either: it puts trunk lean in the FRONTAL plane.

**The correction.** Extend the DOF set, in this order of value:

| DOF | Plane | Axis | Status |
|---|---|---|---|
| trunk **flexion/extension** | sagittal | mediolateral | **NEW — the requester's ask** |
| trunk lateral flexion | frontal | fore-aft | exists (`sway`) |
| head flexion/extension | sagittal | mediolateral | **NEW** |
| head lateral flexion | frontal | fore-aft | exists (`headNod`, misnamed) |
| head axial rotation | transverse | vertical | **NEW, C4+ — completes the spherical joint** |
| vertical bounce, hip shift, arm swing | — | — | exist |

**DECIDE — requester.** The figure is drawn front-facing, where sagittal motion
is foreshortened to nothing. Two ways out:

- **(i) 3/4 oblique projection (recommended)** — one figure, both planes legible,
  one drawing. Costs a projection matrix in the skeleton function.
- **(ii) two orthogonal views side by side** — front and side. Unambiguous,
  needs twice the dock width, and reads as an engineering drawing rather than a
  body.

**AC3.1** `SFeelFlowPose` gains the new components. The three "no motion"
outcomes (`invalid` / `past the material` / `genuinely still`) stay DISTINCT and
`sfeelflowpose.h`'s doc is updated to say so for the new fields too.

**AC3.2** `sFeelFlowPoseDescribe`'s grammar is extended, ONE spelling, APPEND-only
— never reorder — so every existing `contains=` keeps matching (the
`describeHead()` `Amode=` precedent, proposal 37 P6). **One token is excepted:
`headNod`, renamed per AC3.4.** v1 asserted both "never rename" and "rename
`headNod`", which cannot both hold: the pinned line in
`feel_flow_puppet.qxa:129` literally contains `headNod=0.9262`. Renaming only the
struct field while keeping the grammar token would split the field name from its
spelling and break the ONE-spelling doctrine this plan enforces everywhere else.
So the rename is COORDINATED — field and grammar together — and carries a re-pin
in the same commit, exactly the way AC1.4 does it for C1: old and new values in
the case header, reason stated.

**AC3.3** The skeleton function projects per the chosen option. New case
`feel_flow_puppet_planes.qxa` drives `assert-puppet-skeleton` with EXPLICIT joint
attributes (AC0.2 — there is still no way to command the analysis-derived pose)
and asserts that a pure **sagittal** trunk command moves the head's projected
position and leaves the shoulder-bar WIDTH unchanged, while a pure **axial**
command changes the shoulder-bar width (foreshortening) and leaves the trunk
angle unchanged. That PAIR is what separates the three axes; either assertion
alone passes on a one-plane model.

**AC3.4** `headNod` is RENAMED to name the plane it actually drives, in the struct
AND the grammar together, with the re-pin AC3.2 describes.

**AC3.5** AC-INV holds. The new DOF are driven from existing units until C4; no
aspect changes.

**NOT gated:** which unit should drive trunk flexion (C4 makes it a plant output,
so do not spend a decision on an interim mapping); deep-view aesthetics.

---

## C4a — The plant, first half: a joint with inertia — **DONE 2026-08-28,
## CORRECTED 2026-08-30 after an adversarial review of the maths**

> **READ THE CORRECTION SECTION BELOW BEFORE THE ORIGINAL NOTES.** The first
> build's equation of motion was missing a term that is O(100 %) of its own
> headline number, and three of the numbers quoted in this section were
> properties of the truncated equation rather than of a neck. They are kept,
> struck through in prose, because knowing which number moved and why is the
> only way the next person can tell a regression from the fix.
>
> **Executed.** `tw/body/twbodyjoint.h` — a joint carrying the segment below
> it, driven by its PARENT's motion, with per-axis DOF masks
> (`twBodySpherical()` for a neck, `twBodyHinge()` for a knee), hard range-of-
> motion limits with dissipative stops, and an EXACT closed-form step for the
> linear second-order system in every damping regime.
>
> **The requester's own test is the central AC**: torso flexes 25° then STOPS →
> the head nods **forward 57.800°** (was 44.985° before the correction) and
> rings — 13 crossings of its own equilibrium. Its control is the same input
> through the shipped FIRST-ORDER lag: **max forward +0.000000°** across
> τ ∈ {0.03, 0.126, 0.5}. A lag is a filter; what the requester describes is a
> mass. That pair is the whole argument and neither half can pass alone.
>
> Also gated: energy conserved to **1.281e-11** over 60 s AND pointwise flat
> (band [0.115455, 0.115455] J — the step is exact, not merely stable); arrest
> to **0.0000 %** of peak DEVIATION in 6 natural periods; a hinge does not move
> on its constrained axes under a 40 rad/s² drive on all three; limits never
> exceeded; an overdamped joint overshoots far less (23.157° against 57.800°).
>
> **A sign error found and fixed via the requester's pushback on 45°:** stiffness
> used `m·g·d` as RESTORING, but the head's mass sits ABOVE the atlanto-occipital
> joint — gravity is DESTABILISING, an inverted pendulum. Treating it as
> restoring makes an upright head appear stable with no muscle tone at all.
> `invertedPendulum` now carries the sign, and both sides of the stability
> threshold are asserted: below 1× the gravity moment an inverted joint has
> NEGATIVE total stiffness and reports no natural frequency, while a hanging
> segment is stable on gravity alone.
>
> **NOT met, deliberately:** proposal 44's counterswing gate (AC4.2). This build
> has parent→child driving and no child→parent reaction, so an arm cannot yet
> counter-rotate the trunk. Named, not faked. Also per-axis decoupled — each
> axis has its own inertia, stiffness and drive, but there are no gyroscopic
> cross-terms BETWEEN axes. And not wired to the pose: C4a is the mechanism.

### THE CORRECTION (2026-08-30) — what an adversarial review found

A Fable instance was asked to review the mathematics of `tw/body` and
`sfeelflowskeleton` adversarially, deriving everything itself rather than
taking the comments' word for anything. Verdict: **HAS ERRORS**. It confirmed
`exactStep` to **≤ 2e-15** against a 200 000-step RK4 in all three damping
regimes, confirmed `compound`, confirmed the `driveCoef` coefficient and its
sign, confirmed the inverted-pendulum sign and the `ks = 1` threshold, and
reproduced independently every pinned number in `body_measures_test` and
`feel_flow_puppet_chain` (0.7346, 1.0332, 1.2640, 0.8002, 21.8802, 25.0000,
−8.7447, 36.6915). What it found wrong was the EQUATION, not the arithmetic.

**1. Gravity was applied to the RELATIVE angle. It acts on the ABSOLUTE one.**
The correct planar EOM carries a second forcing term the first build had no
way to express, because `twBodyJointStep` was passed only the parent's
acceleration:

```
I th'' + c th' + [ k_pass -/+ mgd + m d L phi'^2 ] th
                  = -( I + m d L ) phi''  +/-  m g d phi
                                               ^^^^^^^^^ missing
```

The one-sentence falsifier, and it needs no constants: **a freely hanging arm
on a leaning trunk settled PARALLEL TO THE LEANING TRUNK instead of hanging
plumb.** It is now section 3a of `body_joint_test` — measured −25.0000°
relative, 0.0000° absolute, against a 25° trunk lean — and it fails on the
pre-correction equation by exactly the lean.

The consequence for C4a's own headline: an inverted head on a held lean does
not ring down to trunk-aligned, it **DROOPS**. Peak 44.985° → **57.800°**, and
the resting relative angle 0 → **+25.000°** (= φ/(ks−1) at ks = 2). Section 3
now asserts the ring about that equilibrium and asserts the equilibrium is
positive; the arrest gate (section 5) measures deviation FROM it, because
measuring from zero is only the same question under the truncated equation.

The reviewer's own nonlinear integration gives **62.3° / +20.4°**; this module
is LINEAR in the joint angle (which is what allows a closed-form step at all)
and gives 57.800° / +25.000°. The linear model OVERSTATES a large droop, and
the header now says so — exactness of the INTEGRATOR is not exactness of the
MODEL.

**2. Centripetal stiffening `m d L phi'^2 th` was absent, and it dominates at
the rates this feature is about.** Measured against the neck's k = 5.773
N·m/rad, at a 25° sway: **+10 % at 0.5 Hz, +39 % at 1 Hz, +155 % at 2 Hz,
+618 % at 4 Hz**, quadrupling per doubling exactly. It always stiffens, so the
effective resonance under a fast drive sits ABOVE `twBodyJointNaturalHz` — a
fact C4b has to build on rather than discover.

**3. `exactStep`'s negative-stiffness branch hid the instability it existed to
model.** `k < 0 → w = 0` discarded the stiffness AND the damping, so an
inverted head below its own threshold, displaced 0.2 rad and left completely
alone, **sat at exactly 0.200000 forever** while the true system diverges as
cosh(λt) with λ = 5.04 /s. The module reported the instability and did not
exhibit it. The branch is now the analytic continuation of the overdamped one
(`wd = sqrt(σ² − k/I)`, cosh/sinh, one root positive) and section 2a measures
0.2 rad → **2.6210 rad in 1 s**.

**4. Damping as a RATIO tied c to √k.** So c would vanish exactly at the
stability threshold, where an inverted joint needs it most, and — the reason
this is structural rather than cosmetic — modulating stiffness would silently
modulate damping, which is precisely what C4b makes a per-hop control. The
stored quantity is now an absolute coefficient computed once from the ratio at
a REFERENCE stiffness (passive tissue plus the MAGNITUDE of the gravity
moment, positive whichever side the mass sits). Section 6c asserts c is
unchanged by centripetal stiffening, is non-zero at ks = 1, and does not move
when the inverted flag flips.

**5. The axial axis was given sagittal-plane physics wholesale** — an error
the reviewer found unasked. Gravity exerts no moment about a segment's own long
axis (twisting the head neither raises nor lowers its CoM), the parent's lever
is zero there (this joint sits ON the parent's long axis), and `inertiaProx` is
a TRANSVERSE inertia carrying a parallel-axis `d²` term that does not exist for
twist. Every quantity is now per-axis: `twBodyJointInertia`,
`twBodyJointStiffness` and `twBodyJointDamping` all take a `twBodyAxis`.
Measured on the neck: **I 0.09107 vs 0.01641 kg·m²**, k ratio **exactly 2.0**
(= ks/(ks−1), i.e. twist carries no gravity term), **4.222 Hz vs 1.267 Hz** —
a head twists 3.3× faster than it nods, which it does. A joint unstable in
flexion is still perfectly stable in twist, and section 6a asserts that pair.

`twBodySegment` gained `radiusGyrLong` / `inertiaLong` to support this. It is
**VERIFY by a weaker route than the four columns beside it**: the standard
tables carry a real longitudinal gyration column and this environment cannot
reach it, so the value comes from a STATED GEOMETRIC MODEL — a uniform solid
cylinder, r_long = R/√2, over an assumed slenderness per segment. An assumption
written down, not a citation. Everything `body_measures_test` asserts about it
is a relation or an identity (I_long = m·r², strictly below I_prox, a colinear
compound is a PLAIN SUM, M·H² scaling, a slender shank's ratio 37.5 exceeds a
stubby head's 5.6), so no gate rests on the number itself.

**Watched failing, six sabotages, one per fix** — each reverting exactly one
correction and each biting only its own assertions: dropping the `±mgd·φ`
forcing (4 failures, the plumb arm among them), restoring `k < 0 → w = 0`
(2), reusing the transverse inertia for twist (1), giving the long axis a
gravity moment (2), dropping the centripetal term (5), and tying damping back
to √k (2).

**Two doc-level defects the review also found, both fixed:**
`twbodyjoint.h` said an overdamped joint "cannot overshoot" as an absolute,
which the module's own ζ = 1.4 run contradicts (**23.157° forward** — true of
free decay, false under a bipolar drive); and `limitHits` counts DWELL, not
arrivals (19 770 steps at the stop in section 7). Also
`sfeelflowskeleton.cpp`'s `rotAxial` comment stated a sign the arithmetic
contradicts, and `depthOf` dropped the elevation term from the painter order.

**Still not fixed, and named:** the model stays LINEAR in the joint angle (a
hanging joint softens ~1.2 % at 25°, an inverted one with linear tissue hardens
~4.6 %) and there is still no child→parent reaction. The reviewer's caveat that
the crossover's "above it the weight is beside the point" narrative is false
for an INVERTED joint — gravity there is a bias that must be met at DC — is
recorded in `twbodymeasures.h` rather than fixed, because the formula is right
and only the prose was wrong.

## C4 — The plant (44 B1): gravity, coupling, arrest

The ODE, in `tw/body`, driven by RECORDED entrainment output. Semi-implicit
(symplectic) Euler or RK4 at ~200 Hz, exact exponential step for every linear
sub-part — proposal 40 §11.5's forward-Euler lesson is a standing house rule and
`twgroovependulum.cc`'s own comment records what it cost.

Three ACs carry this milestone, and each is a closed form, not a plausibility
judgement.

**AC4.1 — CONSERVATION.** With muscle drive OFF and damping OFF, the **secular
drift** of total mechanical energy over a 60 s integration is **< 0.1 %** —
measured as the linear trend of the per-cycle mean, NOT as the maximum
instantaneous deviation. A symplectic integrator does not conserve energy
pointwise: it holds a bounded oscillation of order `ω·h`, which at 200 Hz and a
~1 Hz DOF is 1–3 % and would fail the naive reading while being exactly the
correct behaviour. If a pointwise bound is wanted instead, mandate RK4 and say so.
This AC is first because it fails loudly on forward Euler, which
`twgroovependulum.cc:157-176` records the cost of.

**AC4.2 — COUNTERACTION** (44 §7 B1's gate). An impulsive arm torque produces a
trunk reaction of **opposite** angular momentum, magnitude equal to the arm's to
within **1 %**, computed from the momentum balance in closed form. With the
coupling term deleted the trunk does not move and the AC fails.

**AC4.3 — THE BASE-ACCELERATION GATE, and it is the one the requester actually
asked for.** The neck base ACCELERATES when the trunk pitches, and the head's
inertia resists it:

```
tau_neck = I_head * ddtheta_head  +  m_head * d * a_base  +  gravity
                                     ^^^^^^^^^^^^^^^^^^^ this term
```

Command a trunk flexion step. Assert that the head's world angle **LAGS** the
trunk's by a phase consistent with the closed-form second-order response, that
the lag has the correct SIGN (the head falls behind going in, overshoots coming
back), and that the peak neck torque matches the closed form to within 2 %.
**Deleting the middle term alone must fail this AC** — that is the sabotage to
run, and it is what separates a head attached to a body from a head welded to
one (which is all C0 gives).

**AC4.4 — ARREST** (new, from verification §9.5). At a drive step to zero:
every DOF reaches **< 5 % of its pre-step RMS within one movement cycle** of its
own frequency, and **no DOF's per-cycle envelope increases** after the step.

> "Envelope", not "every sample" — v1 said the latter and it collides head-on
> with AC4.3, which REQUIRES an overshoot on the return. Bound the decay of the
> cycle envelope; individual samples inside a cycle may rise.

**Sabotage, named because it is not obvious:** run the arrest assertion over
`body_probe`'s CSV trace of the CURRENT model across `h_fill_break`'s break
window — `/tmp/body_probe smaragd/tests/groove/h_fill_break.wav 18.22 22.22
trace.csv` already emits it. The shipped model retains 26–58 % and `hipShift`
grows to **291.5 %**, so both clauses fail on that trace.

**AC4.5** The head is a **spherical (3-DOF) joint** at the base — flexion/
extension, lateral flexion, axial rotation. The real neck is a seven-vertebra
column; a single ball joint is the standard reduction and is inside this
project's own "not hyper-exact". Assert all three DOF respond independently to
their own commanded torque and that a pure command on one leaves the other two
within numerical noise.

**AC4.6** Purity: no Qt, no threads, no `rand()`, no `Date`/clock. Same
integration run twice is **bit-identical**. AC-INV holds — the plant is a
separate pass and touches no groove payload.

**AC4.7** The gates are registered as a ctest target: `tw_module_test(body ...)`
in `smaragd/tw303a/CMakeLists.txt`, following the sidecar's own registration, and
the new `.qxa` cases force a re-configure (`./build.sh`) so the `CONFIGURE_DEPENDS`
glob picks them up — an unregistered case reports all-green having never run,
which this repo has actually hit.

**NOT gated:** Hill-type muscle models (44 §6 names torque actuators as the
limit); subject-fitted PD gains; feet leaving the ground; steps; full 3D.

---

## C4c — The postural / musical torque split — **DONE 2026-08-30**

> **The first consequence of §8 q3, executed.** `twBodyJoint::posturalGain`
> (0..1, default **0** — an unactivated joint, which is what every other field
> in that struct describes) plus `twBodyPosturalTorque()`. Both gravity terms
> are scaled by `(1 − gain)`, so `tau_muscle = tau_postural + tau_active` with
> the postural half exact at any `dt` and the ensemble's output reaching
> `activeTorque` alone. Invariants: `tw303a/body/CONTRACT.md` 13-15.
>
> **A GAIN, not a torque, and that is the design.** Postural tone is feedback
> on the same absolute angle gravity acts on, so it belongs beside the term it
> cancels. Applied as an explicit torque sampled at the top of a step it leaves
> a residual growing with `dt`, "fully compensated" would not hold still, and
> nothing could be gated.
>
> **The gate is a PAIR**, because either half alone is satisfiable by a
> constant: at gain 0 a free arm HANGS PLUMB (−25.0000° against a 25° lean); at
> gain 1 the same arm STAYS WHERE IT IS PUT (0.6000 rad → **0.6000 rad** after
> 30 s). No model that ignores the gain produces both.
>
> **One property worth knowing because it surprises:** a PARTLY toned free
> joint still settles plumb, at every gain from 0 to 0.75. Scaling both gravity
> terms alike changes the strength of the pull, not its direction — only at
> gain 1 does the preferred position vanish. Compensating the stiffness alone
> moves the equilibrium, which is the natural mistake, and is watched failing
> (S8 below: −0.582 / −0.873 / −1.745 rad instead of −0.436).
>
> **`ks > 1` is now explicitly a claim about LIGAMENT, not about a person.**
> The ks = 0.6 joint section 2a watches fall over (0.2 → 2.6210 rad in 1 s) is
> perfectly stable once toned: 0.2 → **0.0017 rad**. Section 3c asserts the
> pair, so the threshold cannot be misread as a statement about people.
>
> **And the hold COSTS torque**, which C4b's objective must count: a fully toned
> head on a 25° lean holds **−2.5188 N·m**, half the tone exactly half the
> torque, an unactivated joint exactly zero (it droops instead), an upright one
> exactly zero (nothing to hold). An effort term counting only the active torque
> would report a body straining against a beat as cheaper than one standing
> leant over.
>
> **Watched failing, three sabotages:** ignoring the gain (5 failures),
> compensating the stiffness but not the parent-angle forcing (4), and a
> postural-torque readout that ignores how much is actually compensated (2).
>
> **NOT done:** nothing yet SUPPLIES `tau_active` — the ensemble is not wired to
> a joint at all, which is C5. And `posturalGain` is a per-joint constant here;
> whether tone is itself modulated by the drive is C4b's question — **now
> answered YES, see C4b below.**

### C4c/2 — CAN THE PLANT DO WHAT THE REQUESTER DESCRIBES? Asked BEFORE C4b

Because a controller that optimises toward a motion the plant cannot produce is
built on sand. The description, verbatim (2026-08-30, on the *Enter Sandman*
build-up): raise postural tone between trunk and head, drive the head down HARD,
and have it *"bounce up again like a ball thrown forcefully on the floor"*, to
then push it down again — the bounce being what enables a **harsher next
strike**. Sections 10 and 11 of `body_joint_test` measure exactly that.

**Yes, and here is the number.** Same 6 N·m strike for 60 ms, trunk held at 20°:

| | rests at | travels down | back to rest |
|---|---|---|---|
| braced (ks 8, tone 1.0) | **0.00°** (trunk-aligned) | 7.44° | **78 ms** |
| slack (ks 1.5, tone 0) | **+40.00°** (already drooped) | 25.94° | **361 ms** |

**MY FIRST READING OF THE CLAIM WAS WRONG AND IS RECORDED SO IT IS NOT
RE-TRIED.** I wrote it as *"a braced head bounces, a slack one does not — its
own weight eats the return"*, and the model says otherwise: an underdamped joint
RINGS whether or not it is toned, so the slack head comes back past its resting
place too. What actually separates them is **where they rest** and **how fast
they turn around** — 4.6× here — and those are the two things "hard and sudden"
names. The braced head also travels LESS far, so the bounce is not the same
motion done harder.

**And the mechanism is resonance, measured against closed forms** so nothing
rests on a provisional constant. Resonant amplification over the static
deflection is exactly `1/(2·zeta_eff)`:

| | f_n | zeta_eff | static | at f_n | Q | closed form |
|---|---|---|---|---|---|---|
| ks 2, untoned | 1.267 Hz | 0.2598 | 19.85° | 38.20° | **1.925** | 1.925 |
| ks 2, toned | 1.792 Hz | 0.1837 | 9.93° | 27.01° | **2.722** | 2.722 |
| ks 8, toned | 3.584 Hz | 0.1591 | 2.48° | 7.80° | **3.143** | 3.143 |

**A SECOND WRONG READING, also recorded:** asserting that the peak beats the
response at HALF the rate does not work and was tried — a soft joint's
quasi-static response at `f_n/2` is large simply because it is soft, giving a
ratio of 1.5× that says nothing about resonance. The amplification over the
STATIC deflection is the honest measure.

**Two consequences that matter for C4b:**

- **Postural tone alone raises the resonance**, by exactly `sqrt(ks/(ks−1))` —
  here `sqrt(2)`, 1.267 → 1.792 Hz. For an INVERTED joint, cancelling gravity IS
  stiffening it. Co-contraction takes it to 3.584 Hz, 2.83× the untoned rate.
  **That range is what C4b tunes across**, and without it "the body tunes toward
  the beat" would not be a thing a body could do.
- **A softer joint is effectively MORE damped** (`zeta_eff = dampingRatio ·
  sqrt(kRef/k)`, because `c` is absolute — invariant 5). So stiffening does two
  things at once: it moves the resonance up AND sharpens it. Both help the
  bounce. **C4b's optimiser must read `twBodyJointDampingRatioAt()`, never the
  stored ratio** — the new accessor exists for exactly that.

Watched failing: a `zeta_eff` that reports the stored ratio (5 failures), and
postural tone reaching the forcing but not the stiffness (9).

## C4b — Stiffness is EMERGENT, not a constant (requester, 2026-08-28)

**The correction.** C4a treats joint stiffness as a per-joint constant. It is
not a property of the joint: a dancing body's stiffness is dominated by ACTIVE
muscle co-contraction, driven by the rhythmic excitation the sound produces, and
the body settles on the least muscle action that still does the job.

**Two consequences, and the second is the interesting one.**

1. **Do not source it from the literature.** Passive joint stiffness is measured
   on a RELAXED joint, which is the wrong regime, so AC2.1's sourcing task does
   not apply to this field the way it applies to a mass or a limb length. C4a's
   header now says so at the constant.
2. **If stiffness tracks the drive, so does each joint's natural frequency** —
   and a body minimising muscle effort has a reason to tune omega_n TOWARD the
   rate driving it, because a driven oscillator needs least torque for a given
   amplitude at resonance. That would make **resonance with the music an
   EMERGENT property of impedance modulation** rather than something seeded into
   an ensemble. That is a materially different claim from proposal 40's, and it
   is closer to what the van Noorden & Moelants ~2 Hz body-resonance anchor
   would predict if the anchor is a consequence rather than an input.

### The two open questions, ANSWERED by the requester 2026-08-30

**Q1 — is postural TONE itself modulated by the drive? YES, and ANTICIPATORY.**
In the requester's words, on a rock build-up: tone is raised *in anticipation of*
a heavy movement — high tone between trunk and head, legs supporting — *in order
to* make the hard sudden nod possible and have the head bounce back up, so that
the next strike can be harsher still.

Three things follow, and the third is the one that constrains the design:

1. `posturalGain` is a CONTROL VARIABLE, not a per-joint constant. It joins
   `stiffnessScale` in AC4b.1 rather than sitting outside it.
2. It is not a separate knob from stiffness in its EFFECT — C4c/2 measured tone
   alone moving the resonance by `sqrt(2)` — but it IS separate in its MEANING:
   tone cancels gravity, co-contraction adds spring. They compose.
3. **IT LEADS THE DRIVE. It is FEED-FORWARD, not a response to the current
   excitation.** A body braces during the build-up, before the hit. That makes
   it a genuinely different claim from a reactive controller, and it is
   testable: the tone envelope should RISE AHEAD of the excitation it is bracing
   for. The natural non-opinion source is the ensemble's own aggregate power
   integrated over a window, which leads any individual hit by construction —
   but WHICH signal is chosen is an AC of its own, because picking one that
   already contains the answer would be circular.

**Q2 — what is MATCH? NOT phase coherence.** The requester's definition:
*enabling the body to express its urge to move as intensely as it likes to, over
an extended period of time — tens of seconds.* That is a materially different
quantity from either candidate I proposed, and better:

- It is an **achieved-intensity** measure, not an alignment measure. The reward
  is how much movement the body actually gets, not how well-timed it is.
- **"As intense as it LIKES TO"** means there is a target set by the music — an
  urge — and a natural CEILING. Exceeding the urge is not extra reward. So the
  sensible form is a ratio: **the fraction of the available drive the body
  converts into sustained movement.** That is dimensionless, which is what keeps
  it body- and species-independent, which is the whole reason §8 q3 went to
  torque.
- **"Over an extended period"** is what makes the effort term BITE. Effort is
  not a secondary penalty to be weighed against the reward — it is a BUDGET over
  the window. You cannot thrash at maximum effort for thirty seconds. So the
  natural form is constrained rather than weighted:

  ```
  maximise   integral over window of  min( achieved, urge )
  subject to integral over window of  effort  <=  budget
  ```

  with `lambda` as its Lagrange multiplier rather than as a free taste
  parameter — which is a materially better answer to "declare the one free
  parameter" than tuning `lambda` directly.

**THIS RESOLVES THE MEASURE-DEPENDENCE COMPLETELY, and by a stronger route than
the one recorded above.** The reviewer's escape route — elastic recovery making
mean power independent of `k` — was an argument about the EFFORT term. Under
this objective the reward is sustained AMPLITUDE and the constraint is a
METABOLIC BUDGET, and **resonance is precisely what lets a body hold a large
amplitude inside a fixed budget.** Off resonance you either move less or run
out. The resonance incentive is now structural rather than an artefact of which
effort integral was picked.

### AC4b.3 — the metrics, PROPOSED and IMPLEMENTED 2026-08-30

`tw/body/twbodyobjective.h`, gated by `body_objective_test`. Pure, no ensemble
needed, so the definitions can be argued with before anything is wired to them.
Invariants: `tw303a/body/CONTRACT.md` 18-26.

| Quantity | Definition | Why |
|---|---|---|
| **achieved** | mean `\|dθ/dt\|` over the joint's own ROM — **range-lengths per second** | A ratio, so a mouse and a person can both do "three a second"; §8 q3 went to torque exactly so nothing would be body-specific, and an objective in radians voids that. TRAVEL, not excursion: "hard and sudden" is the requester's phrase and peak angle cannot express it. Closed form **`4·f·(A/ROM)`** |
| **urge** | `urgeNorm(t) × maxUrge`, `urgeNorm` ∈ [0,1] on an **ABSOLUTE scale comparable across tracks** — full scale, never the track's own peak | Requester, 2026-08-30. Per-track normalisation makes a gentle ambient piece and a rock build-up equally demanding at their own loudest moment, so *"this music demands more movement than that music"* becomes inexpressible — while being exactly the claim proposal 44 exists to test. `twbodyobjective` never computes `urgeNorm`: which ensemble quantity means "urge" is a modelling claim and belongs where it can be argued with (C5) |
| **effort** | mean squared **total** muscle torque in units of the segment's own `m·g·d` | Dimensionless and body-normalised. The review settled squared torque as one of the two measures that support the resonance conclusion, and it is the standard metabolic proxy. **Includes the postural torque** — the one term that cannot be recovered from the trajectory later |
| **match** | `Σ min(achieved_i, urge_i) / Σ urge_i` over **sub-windows** | `min()` is "as intense as it LIKES to" — overshoot earns nothing. Sub-windows are "over an extended period": a window MEAN cannot tell a steady body from one that thrashed for a third of the time |
| **window** | **24 s**, sub-window **8 s** | Both DERIVED from constraints meeting, not picked. 24 is SIX periods of the ensemble's slowest unit (twobar, 0.25 Hz) — the shortest window in which that unit is a sustained fact rather than an event — AND exactly three sub-windows, so no sub-window is ragged. **8 s is the requester's value and the SCALE IS A CLAIM**: at 120 BPM it is four bars, a PHRASE, so pausing within a phrase is free and sitting out a phrase is not |

**Exactly TWO free parameters, both physiological, both stated as ANCHORS rather
than numbers** so a reader can disagree with them precisely — `maxUrge` = **12.0**
= *full range, three times a second* (requester's value); `effortBudget` = 1.0 =
*a torque equal to the segment's own weight moment*. Both VERIFY. Worth knowing
when reading any match: a full-range 2 Hz head-bang is **8.0**, so at 12 the
ceiling is OFF most of the time and the BUDGET is what binds — which is the
point of choosing 12 over 4. **Only one binds at a time** — the
budget when the body cannot keep up, the urge when the music is not asking much
— and which one bound is an OBSERVATION (`budgetBound`), not a setting.

**`lambda` is the constraint's LAGRANGE MULTIPLIER, not a weight**, and that is
the substantive change from the form C4b was first written with. It prices the
budget VIOLATION, never the effort itself, so **the ranking is independent of it
once it binds** — swept over 1 … 400 in the test. Nobody has to source it.

**NO PHASE TERM ANYWHERE**, and adding one would make AC4b.2 circular: resonance
is expected to EMERGE from the budget (it is what lets a body hold a large
amplitude inside a fixed cost), so rewarding alignment directly would assume the
conclusion.

**Measured** (`body_objective_test`): achieved is `4·f·(A/ROM)` to 0.01 at every
rate and amplitude tried; half the range at 2 Hz (**4.000**) beats full range at
0.5 Hz (**2.000**); doubling the movement past the urge scores **exactly the
same** match as meeting it; two bodies with **identical total travel** (both
6.000 rl/s) score **1.000 against 0.333**, the bursting one having danced 8 s of
24 with a `weakestSub` of **0.000**; and the phrasing pair is what gates the 8 s
scale — a dancer who pauses 2 s in every 8 reads **1.000** at an 8 s sub-window
and **0.750** at a 1 s one.

**Watched failing, ten sabotages, one per design decision:** achieved as peak
excursion (20 failures), no ceiling (3), one window mean instead of sub-windows
(3), effort ignoring the postural torque (2), lambda pricing the effort rather
than the violation (1), silence scoring a perfect match (1), sub-windows
unweighted (2), `urgeNorm` unclamped (2), a 1 s sub-window (3), and `maxUrge`
back at 4.0 (2).

### A DEFECT THE RETUNE EXPOSED, and a lesson about gates

**A ragged trailing sub-window was scored against a WHOLE sub-window's urge**, so
a 24.001 s run of a 24 s window lost **a quarter of its match to one
millisecond**. Not an edge case waiting to happen: a real hop grid will almost
never divide a window evenly, so every run would have paid it. Found by the
sustainment case reading **0.250** where the closed form says **0.333** — and
the module's own header had already warned about exactly this failure mode while
the code committed it. A sub-window is now worth its own LENGTH.

**And the first versions of two new gates PASSED UNDER THEIR OWN SABOTAGE**,
which is the more useful finding. The ragged-tail case appended a tail that MET
its urge, so weighted and unweighted both scored 1; the clamp case never fed a
`urgeNorm` above 1, so clamping was unobservable. Both now under-deliver and
over-ask respectively. Recorded because a gate that cannot fail is worse than no
gate — it reports coverage that does not exist, which is the thing this repo's
watched-failing rule exists to catch, and it caught it here only because the
sabotage pass was run on the NEW assertions rather than assumed from the old
ones.

### `urgeNorm`'s SOURCE — proposed, measured and implemented 2026-08-30

**The three obvious candidates are all circular.** `perUnitPower` (|z|²) and
`dissip` (2|α||z|²) are the resonator's RESPONSE, so using either as "what the
music asks for" makes urge and achieved the same quantity and match trivially 1.

**What survives is the DRIVE**, and the two `groove.dyn` phase channels were
already storing it: `support = k·F·cos φ`, `tension = k·F·sin φ`, so

```
music(t)  =  hypot( support, tension ) / k
```

— the magnitude divides the resonator's phase, and with it its response,
straight out; the divide removes the model's own coupling constant.

**THE DIVIDE IS NOT COSMETIC, and the measurement is the argument.** Across six
committed fixtures twobar's raw drive reads about twice every other unit's, and
its `k` is 3.5 against their 1.5. On `a0_broadband_grid` the raw p99s are
**0.02915 / 0.02896 / 0.06757** for reference / sway / twobar and after the
divide **0.01944 / 0.01931 / 0.01931** — the same number three times. The whole
apparent difference was a model constant presented as a property of the
material. `twGrooveCounterTension::k` now carries it; in-memory only, **no
aspect bump**, and AC-INV verified byte-identical on three fixtures.

**Urge is a LEVEL, not an onset train.** Raw `F` is a half-wave-rectified
envelope difference and its MEDIAN over every fixture is exactly **0.00000** —
zero between onsets — so a per-hop urge would be zero nearly always. Smoothed
with a one-pole at `twBodyUrgeTauSec` = **1 s**, DERIVED: longer than the
fastest metrical period (0.25 s) so it does not track onsets, shorter than the
8 s sustainment sub-window so a build-up inside a phrase is visible — which is
exactly what the requester's account of a rock intro is about.

**The calibration, and it is the weakest number in the model.**
`twBodyUrgeReference` = **0.008** is a THIRD free parameter, and unlike the
other two it is a calibration rather than a physiology. Measured: the smoothed
p99 drive spans **0.00030–0.00262** across 30 unit/fixture pairs. Assumed, and
this is the arguable half: the fixtures run −19…−23 dBFS RMS while a commercial
master at ~−10 carries ~3× the linear envelope. So the committed fixtures land
at urgeNorm **0.037–0.327** and a loud master near 1. It cannot change WHICH
motions the model produces — it multiplies every unit's urge equally.

**Watched failing, four sabotages:** the coupling not divided out (6 failures),
the drive taken from `support` alone so the phase is not removed (4), no
smoothing (2), and `urgeNorm` unclamped (1).

**NOT done:** a proper calibration fixture — a full-scale broadband onset train
— which would replace the mastering-level assumption with a measurement. And
the plumbing: `SFeelFlowUiData` does not yet carry `k` through to the app, which
is C5's.

**STILL OPEN:** nothing in `urgeNorm`'s definition. And the metrics are per JOINT and per WINDOW; combining joints into one
body-wide objective, and running it as a sliding window rather than a block, are
both AC4b.1's business.

**AC4b.1** Stiffness AND postural tone become per-hop control variables driven
by the same excitation that drives the motion, not `twBodyJoint` constants. Tone
is FEED-FORWARD and must be shown to lead.

**AC4b.2 — THE CLAIM WORTH TESTING.** Given a fixed metrical drive, the settled
stiffness lands where the joint's natural frequency is NEAR the drive rate.
Assert convergence from both sides (start too stiff and too floppy) and that the
settled `twBodyJointNaturalHz` is within a stated band of the driving rate.
**Watched failing:** a constant stiffness cannot converge on anything, so the
test must be shown to fail against C4a's model.

> **THE BAND IS ONE-SIDED, AND THREE CORRECTIONS TO THE PROSE ABOVE (adversarial
> review, 2026-08-30).** The claim was checked and it HOLDS for the stated
> effort measure — for `I th'' + c th' + k th = tau` at amplitude A and rate ω,
> the peak torque `A·√((k−Iω²)² + (cω)²)` really is minimised at `k = Iω²`, with
> floor `cωA`. Three things the claim as written does not carry:
>
> 1. **Co-contraction has a cost of its own, and it biases the optimum BELOW
>    resonance.** With effort = `½⟨τ²⟩ + β(k − k_pass)²`, the optimum moves
>    `k/k_res` = 0.999 → 0.986 → **0.883** as β grows. And a joint whose PASSIVE
>    stiffness already exceeds `Iω²` cannot tune down at all — co-contraction
>    only adds — so it stays detuned high. AC4b.2 must therefore expect a
>    one-sided, below-drive band, not a symmetric one.
> 2. **Co-contraction also raises damping** (muscle viscosity scales with
>    activation), which raises the resonant torque floor `cωA` itself and
>    flattens the optimum further. Ignored by the claim.
> 3. **The centripetal term (see C4a's correction) means the effective
>    resonance under a large fast drive is NOT `twBodyJointNaturalHz`.** The
>    tuning target for an inverted joint is `k_pass = Iω² + mgd` plus that
>    contribution, and at 2–4 Hz the contribution is 155–618 % of k.

**AC4b.3** The effort criterion is explicit and its units stated — an integral
of squared muscle torque over a window, minimised subject to the motion
remaining bounded. Whatever is chosen, it is ONE function and both the
simulation and the assertion call it.

> **AC4b.3 MUST BE SETTLED BEFORE AC4b.2 MEANS ANYTHING, and the review is why.**
> The resonance conclusion is MEASURE-DEPENDENT, not merely measure-flavoured:
> squared torque supports it and so does non-recoverable work (`|τθ′|` with the
> negatives unrecovered), but for **net mechanical work with perfect elastic
> recovery the mean power is `½cω²A²` independent of k — there is no resonance
> incentive at all.** Picking the measure is picking whether the claim is even
> testable, so it is a prerequisite rather than a parallel task.
>
> **THE REQUESTER'S OWN ASSUMPTION SUPPLIES IT, AND IT IS NOT AN EFFORT MEASURE
> AT ALL** (2026-08-30, alongside the §8 q3 decision). The objective is
>
> ```
> maximise   MATCH( ongoing movement , trigger )   −   lambda * EFFORT
> ```
>
> — *"a match of ongoing movement with trigger is satisfying, and the brain
> maximises this satisfaction incrementally"*, together with the earlier
> *"the body would gravitate to an energy saving amount of muscle action"*. Two
> statements, one two-term objective, and it is exactly the β-weighted form the
> reviewer simulated for co-contraction cost — so its finding (the optimum sits
> BELOW resonance, one-sidedly, `k/k_res` down to 0.883 as the cost term grows)
> transfers directly and is what AC4b.2's band must expect.
>
> **What this changes is that the measure-dependence problem no longer bites the
> conclusion.** The reviewer's escape route — elastic recovery making mean power
> independent of `k` — was an argument about the EFFORT term alone. The reward
> term is not an effort measure: at resonance the body gets the largest excursion
> per unit torque and the most stable phase relation to the drive, so a
> match-maximising objective has a resonance incentive whatever the effort
> measure turns out to be. Effort only decides HOW FAR BELOW resonance the
> optimum settles, not whether one exists.
>
> **Still to define concretely, and it is AC4b.3's remaining work:** what MATCH
> is, quantitatively. The natural candidates are the phase coherence between the
> joint's motion and its driving unit, or the correlation of the two over a
> window. Whichever is chosen it is ONE function, called by both the simulation
> and the assertion, per the AC as originally written. `lambda` is the one free
> parameter of the whole scheme and it must be declared as such rather than
> tuned quietly.

**AC4b.4** AC-INV holds; if this reaches the pose, `feel_flow_puppet.qxa` is
re-pinned with old and new values and the reason, as C1/C1a did.

### 44 §8 q3 — SETTLED 2026-08-30: **TORQUE (option A)**

**The requester's decision, and the reasoning is the load-bearing part**, because
it constrains what this model may and may not contain from here on:

> The thesis is that a particular set of dance moves is an effect of NEUROLOGICAL
> PROGRAMMING and not a consequence of socialisation. Grounding the motion on
> muscle ACTIVATION, with the stimulus derived from literature, is what keeps the
> requester's own opinion out of the model — a posture target would be a claim
> about what a body is *trying to look like*, which is exactly the thing the
> thesis is trying to show is not chosen. And the mechanism is not assumed to be
> homo sapiens only: humans alone vary in size and mass by age, so a primitive
> expressed in RADIANS OF POSTURE is species- and body-specific, while a
> primitive expressed as an ACTIVATION is not.

**THE ONE ASSUMPTION THE MODEL IS ALLOWED**, in the requester's own words: *a
match of ongoing movement with trigger is satisfying, and the brain maximises
this satisfaction incrementally.* Everything else must be derived. Anything a
future milestone wants to add is measured against that sentence.

**This needs NO engine change.** `twBodyJointStep`'s `muscleTorque` parameter is
already exactly option A, and always was; option B is what would have needed a
new field. The decision costs nothing to implement, which is worth knowing before
weighing the three consequences below.

**Consequence 1 — the postural hold must be SPLIT OUT, and doing so does not
break the no-opinion property.** Under a torque reading, an inverted segment
needs a standing DC torque simply to stay upright (C4a's correction made that
bias explicit and measurable: a head on a 25° lean droops 25° at ks = 2 with no
muscle at all). That torque cannot come from the ensemble — the ensemble knows
nothing about gravity, and a musical signal riding on a large DC offset would be
an opinion smuggled in as a constant. So the total is

```
tau_muscle = tau_postural( body, current pose )   +   tau_musical( ensemble )
```

and the first term is **DERIVED, with no free parameter**: it is whatever
cancels `m·g·d·sin(absolute angle)` at each joint, a quantity the measures module
already computes. It is gravity compensation, a physical requirement, not a
stylistic choice — which is what keeps it out of scope for the socialisation
objection. Only `tau_musical` carries the ensemble's output.

**Consequence 2 — a "sudden stop" is a NEGATIVE torque, not the absence of one,
and the requester's own assumption supplies it.** Under a set-point reading a
stop is free (the target simply stops moving and the joint is pulled to rest).
Under a torque reading, drive going to zero means the segment COASTS and then
settles at its gravitational equilibrium — for a head, a droop, not a stop. So
an arrest has to be an ACTIVE BRAKING torque. That is not a problem the decision
creates, it is a prediction it makes: under "maximise the match, incrementally",
a body that anticipates a stop invests torque in arriving stopped, and the size
of that investment is a measurable consequence rather than an animation
parameter. **This is now the sharpest observable the whole model produces**, and
it is what the requester's original report (2026-08-27, "movement seemed linear
to me and not honoring weight, sudden stops that people do") was about.

**Consequence 3 — amplitude now scales INVERSELY with stiffness, which makes
C4b's optimisation sharper rather than weaker.** Under option B, doubling the
stiffness leaves the amplitude alone and only tightens the tracking; under A it
HALVES the motion. So stiffness is no longer a free dial that changes only the
feel — it trades directly against excursion, which is what gives
"least muscle action that still does the job" something to be least *about*.

**Consequence 4 — BODY SIZE BECOMES A PREDICTION, and that is an asset.** Under
A the same torque on a heavier body produces less motion and a lower crossover
frequency, so `twBodyMeasures`'s M and H stop being cosmetic and become the
independent variable of a real experiment — proposal 44 §3's
"correlate-with-my-body" idea, but as a falsifiable claim rather than a
convenience. It is also the reading under which the cross-species argument above
is even expressible.

**The honest cost, stated plainly:** "derive the stimulus from literature" is now
a sourcing task on TORQUE magnitudes — EMG-scale numbers — and AC2.1 is already
open and already blocked in this environment for the far easier case of segment
masses. Expect the musical torque's SCALE to be provisional for some time. What
is NOT provisional, and this is the point of the decision, is its SHAPE: the
time course comes from the ensemble, and a scale factor is one number that
multiplies everything equally, so it cannot change which motions the model
produces — only how large they are.

## C5 — The `body.pose` aspect and the puppet switch — **THE SEAM IS DONE
## 2026-08-30; THE PLANT PASS THAT FILLS IT IS NOT**

> **What landed:** the aspect, its own params blob, the orphan discipline, the
> pose seam's one new branch, the layering edges, and every gate above except
> the ones that need a producer. `tw/sidecar/twbodyposeaspect.h`,
> `twAspect::BodyPose` v1, `SFeelFlowUiData::pose`, `feelflow_pose_test`,
> `sidecar_test` section 8.
>
### THE PLANT PASS — **DONE 2026-08-30**, and the chain closes

`tw/sidecar/twbodyplant.h`, gated by `body_plant_test` over a REAL analysis:
the production front end and ensemble run over a click train, then the plant.
It is the first gate that can fail for a reason none of the others can see.

**Four joints, seven DOF**, which the per-axis work of C4a's correction is what
makes possible:

| joint | segment | axes → DOF |
|---|---|---|
| LEG | compound(Thigh…Foot) at the hip | Flex ← "bounce" → BounceY; Lateral ← "twobar" → HipShift |
| TRUNK | Trunk at the pelvis | Lateral ← "sway" → Sway; Flex/Axial undriven |
| ARM | compound(UpperArm…Hand), levered off the trunk | Flex ← "limbs" → ArmSwing |
| NECK | HeadNeck at the atlas, levered off the trunk | Lateral ← **NOTHING** → HeadNod |

**THE HEAD IS MAPPED TO NO UNIT AT ALL, and that is the whole proposal in one
line of a table.** The requester's report (2026-08-27) was that the head moved
with the tatum gauge rather than with the torso. C1 took it off that gauge and
hung it on the trunk through a first-order lag; C4a showed a lag CANNOT produce
the described nod because it has no momentum. Here the head is a real segment
on a real joint whose PARENT IS THE TRUNK. **Measured: peak head nod 0.0212 rad
(1.21°), head/trunk correlation 0.9289** — related, and not welded. A rigid
pair reads exactly 1.0000.

**THE TORQUE SCALE IS DERIVED, NOT CHOSEN**, so the plant adds no free
parameter: `torqueScale = rom·k/Q`, i.e. **full urge at the joint's own
resonance is exactly full range of motion** — which ties the plant's output to
`maxUrge`'s own anchor ("full range, three times a second") by construction.
Measured to 1e-9 against the closed form: **47.990319 N·m** for sway.

**Measured, end to end:** 1200 hops, 4 driven DOF, peak urge 0.1621, peak sway
0.0542 rad; a 110 kg / 1.95 m body gives 0.0555 and 0.0232 against 0.0542 and
0.0212, so **M and H really are key material** and AC5.1's nesting argument is
not decoration; silence gives **exactly 0.000e+00** on every DOF; the payload
is byte-deterministic across two runs.

**And the METRICAL LADDER SURVIVES THE PLANT:** drawn rates **sway 0.625 Hz,
bounce 1.958 Hz** against a seeded ladder of 0.5 and 2.0, with the fixture's
tatum at 4.0 Hz. Nothing is dragged up to the tatum by the machinery in
between.

**Watched failing, seven sabotages.** Three bit immediately: the neck given no
parent (the head reads 0.0000), the head welded to the trunk (correlation
exactly 1.0000), and the body ignored as a parameter. **Two did NOT, and both
needed a better gate rather than a better sabotage** — a flat torque scale of
60 N·m moved peak sway from 0.0542 to 0.0793 rad, comfortably inside any
plausible excursion bound, and leaving the coupling in the drive breaks no
bound at all. Both are now asserted AS CLOSED FORMS (the scale against
`rom·k/Q` to 1e-9; the peak urge against an independent recompute over the
mapped units), and both then bit. **An excursion bound cannot catch a wrong
scale**, and that is the lesson.

**A SEVENTH DOES NOT BITE AND THE REASON IS A FINDING, not a gap.** Swapping
`cosMet` for `cosPhi` — C1a's own defect — moves the drawn rates only from
0.625 / 1.958 to 0.542 / 2.583 Hz, nowhere near the tatum. **The plant filters
the defect out**: a joint is itself a resonator with a low natural frequency,
so a 4 Hz forcing produces a response dominated by the joint's own omega
whatever phase channel drove it. The plant is strictly MORE ROBUST to C1a's
defect than the direct mapping was — which is why the direct mapping needed
C1a and this does not. The phase choice stays gated where it is observable, at
C1a's mapping, and is recorded here as not gated at this level.

**WHICH SEGMENT STANDS FOR WHICH DOF IS PROVISIONAL and says so.** A bounce is
really knee flexion carrying the upper body; a hip shift is really a standing
sway about the ankles. Both are modelled as axes of the leg's own compound
pendulum. What is NOT provisional is the physics on each axis — a real
segment's inertia, the right gravity sign, absolute damping, postural tone, and
a parent that actually drives its child. An anatomically exact chain needs the
child-to-parent reaction `tw/body`'s CONTRACT already names as missing.

**STILL NOT DONE: the STORE PLUMBING.** `SFeelFlowTrackBounce` does not yet run
the plant, store the payload, or decode it back into `SFeelFlowUiData::pose`.
So the chain is complete and gated end to end in the engine, and the PUPPET is
still C3's — every run takes AC5.4's fallback. That is the last piece of C5 and
it is plumbing rather than design: the producer, the store key, the reload.
>
> **AC5.6's OWN PRESCRIPTION IS BACKWARDS, and the sabotage proved it.** The AC
> says to compare `twGrooveAnalysisParams::serialize()` at defaults and at
> M = 80, and that "the M = 80 comparison fails while the defaults comparison
> still passes, which is the whole point". Built and run, the hazard does the
> OPPOSITE, for two reasons: a groove params object has no M, so asking its
> serializer to ignore one is vacuous and passes under the sabotage; and at
> M = 80 the hazard's bytes are IDENTICAL to the correct design's — both give
> the groove blob followed by two f64s. They differ only **AT DEFAULTS**,
> because a body appended to the SHARED serializer has to be
> additive-when-non-default to stay invisible to the fixture corpus, so at
> 75 kg it writes nothing while the correct design writes M and H
> unconditionally. **Being invisible at defaults is precisely what made the
> hazard dangerous, and it is what the defaults length check sees.** Corrected
> in `sidecar_test` with the reasoning beside it; the AC text above is left as
> written so the correction is legible.
>
> **Measured / verified:** AC-INV holds — all three groove payloads
> byte-identical across three fixtures against the pre-branch build (nine
> hashes). The body blob is the groove blob's bytes verbatim plus exactly two
> f64s, at defaults and at M = 80. A groove change re-keys `body.pose` too
> (correct — the plant is driven by the ensemble); a body change re-keys only
> `body.pose` (also correct — the ensemble does not know the body exists). Full
> suite **323 run, 0 failures**.
>
> **Watched failing.** AC5.6: the prescribed sabotage (M/H appended to the
> shared serializer) — 1 failure, the defaults length check. AC5.3/AC5.4, four
> sabotages: the branch never taken (6 failures), two DOF indices swapped (2),
> the ragged-grid guard removed (1), the plant angle unclamped (1). A fifth —
> "the branch always taken" — is **not constructible**: `pose.size() == nHops`
> already implies non-empty (nHops is proven non-zero upstream), so removing
> the `!empty()` guard produces equivalent code. Recorded rather than dressed
> up as a passing sabotage; the guard is kept as documentation of intent.
>
> **NOT gated:** anything that needs a producer — the plant's own trajectories,
> the encoder, the store round-trip through a real analysis, and AC5.4 against
> a REAL re-analysis rather than against the same snapshot with and without a
> pose vector. The C++ test is the strongest form available without one, and it
> is stronger than a `.qxa` would be for this particular question: a script
> cannot construct an `SFeelFlowUiData`, only reach a pose through a real
> analysis, which would make the comparison depend on the analysis rather than
> on the branch.

## C5 — The `body.pose` aspect and the puppet switch

**AC5.1** New aspect `body.pose` v1 in `twaspects.h`: per hop, per DOF
`{ angle, velocity, muscleTorque }`. It carries its **OWN params blob** — the
shared groove blob's bytes ⊕ M ⊕ H — and `twGrooveAnalysisParams::serialize()` is
**not touched**.

> **This is the plan's sharpest shared-state hazard and v1 walked into it.** v1
> said "keyed by the existing content/params hashes plus M and H". The groove
> params blob is SHARED by all three groove aspects (`twaspects.h:218`, `:260`,
> `:344` — "same twGrooveAnalysisParams key, same hash"), so the natural
> implementation, appending M/H to that serializer, re-keys `groove.res`/`ev`/`dyn`
> as well. Worse, it would pass every gate in this plan: the additive-when-
> non-default rule means nothing is serialized at defaults, so the fixture set
> stays byte-identical — and then the first user who sets M = 80 triggers a full
> groove re-analysis for a parameter the ensemble does not consume. That is
> exactly the plant-changes-the-neural-layer coupling AC-INV exists to forbid, and
> it would have been invisible.

**AC5.2** V1-orphan discipline, as every other aspect: an older/absent version
MISSES and is deleted, never adopted at the wrong stride. Gated in
`sidecar_test` the way `PreviewPeaksVersion` 1→2 already is.

**AC5.3** `sFeelFlowPoseAt` grows ONE branch: prefer `body.pose` when present,
fall back to the M3e direct mapping when not. The Pose seam was built for exactly
this swap and its shape does not change.

**AC5.4** With no `body.pose` on disk the pose numbers are **byte-identical to
C3's**. That is what makes the fallback safe to ship before every fixture has
been re-analyzed.

**AC5.5** AC-INV holds: all three groove payloads byte-identical, versions still
1 / 1 / 2.

**AC5.6** `twGrooveAnalysisParams::serialize()`'s output bytes are **unchanged by
C5 at every parameter setting, including non-default M and H**. Check: a unit test
in `sidecar_test` comparing `serialize()` pre/post at defaults AND at M = 80, plus
one `groove.res` store key unchanged across an M/H change. **Sabotage:** append
M/H to the shared serializer instead — the M = 80 comparison fails while the
defaults comparison still passes, which is the whole point.

**AC5.7** The plant pass's HOST module is named (the M1b precedent is app-side in
`main/objects/track`), and that module's engine allowlist in
`tools/check_layering.py` gains `'body'`. The tw-side DAG entry
(`'body': ['core']`) is separate and also required; `tw_sidecar → tw_body` is
acyclic since `tw_body` depends only on `tw_core`.

---

## C7 — THE PLANE, AND WHY THE HEAD IS INVISIBLE (requester, 2026-08-31)

Two user reports on a slow hip-hop track: **no observable head movement**, and
**circular / left-right hip motion with no forward-back upper body** — "moving
the upper body half looks like the number-one reaction for a standing person."

### Cache was ruled out first, and it is not the cause

Three independent reasons: `GrooveDynVersion` went to **3** on this branch
(C1a), and the aspect version is part of the store key, so an older sidecar
MISSES and re-analyses; the head reads **no aspect channel at all** (since C1 it
is derived from the trunk by read-side code); and `body.pose` is new, with the
plant unwired regardless.

### What is actually visible, measured

`body_probe` now reports the DRAWN nod in degrees — the head's WORLD angle looks
healthy while the RELATIVE nod, the only thing an eye reads as "the head
moving", is what matters. Across the committed fixtures: **nod 0.86–1.04° rms
(peak 3.0–3.9°) against a sway of 3.5–5.1° rms (peak 10.5–18.4°)** — the head
nods 20–25 % as many degrees as the trunk leans. Sub-pixel on a wireframe.

And it is **worse on a slow track by construction**, because the shipped neck is
a one-pole lag whose output is proportional to the trunk's RATE:

| trunk rate | drawn nod, as % of the sway in degrees |
|---|---|
| 0.25 Hz | 9.7 % |
| 0.33 Hz (slow hip-hop) | 12.6 % |
| 0.50 Hz (the fixtures) | 18.4 % |
| 2.00 Hz | 42.3 % |

### THE PLANE: the cited literature is SILENT on it

`plan/proposed/40_GROOVE_RESONANCE.md` carries every citation this model rests
on. Grepped for `sagittal`, `frontal`, `lateral`, `fore-aft`, `anteroposterior`,
`mediolateral`: **zero occurrences.** Toiviainen 2010 fixes a metrical LEVEL,
Burger 2013 and Hove 2014 fix a band→PART mapping, van Noorden & Moelants fix a
TEMPO. **Not one fixes a PLANE.**

So the lateral trunk was never the evidence-based default — it is what C0
happened to build, with incumbency rather than support. Both readings sit at the
same evidential level: none. Toiviainen's eigenmovements come from a PCA over
marker positions and therefore DO carry a direction; that table is not in this
repo and the egress proxy refuses Frontiers, PMC, doi.org and Wikipedia. It is
an AC2.1-class task.

`sfeelflowskel::kTrunkSagittal` is the one switch, with all of the above beside
it. **Requester decision: SAGITTAL. It ships `false` until its gate exists** —
see the constant, and the next section.

### A GATE THAT WOULD HAVE PASSED VACUOUSLY, caught before shipping

Flipping the switch makes `feel_flow_puppet_chain`'s five inheritance rows plus
three axis rows assert **0 against 0**: every readout they compare
(`headStubLeanDeg`, `shoulderBarDeg`, `armLeanL/RDeg`) is FRONTAL, and a
sagittally-driven trunk leaves them all at zero. Eight assertions passing while
measuring nothing. **Blocked on that rather than shipped.**

What has to land first is small and specific: a **sagittal head-stub readout**
on `SFeelFlowSkeleton`, the twin of `headStubLeanDeg`. `trunkFlexDeg` and
`armSwingL/RDeg` already exist; only the head stub is missing. Then the constant
flips and the case re-pins onto the sagittal columns.

### THE LOOSE NECK: tried, MEASURED, and it does not work on this path

Requester: *the head is no stiffly jointed limb but a rather loosely jointed one
with a considerable mass* — so it should visibly overshoot. The physics agrees,
and the mechanism is BAND, not scale:

| neck `stiffnessScale` | its own resonance | gain at a 0.5 Hz trunk sway |
|---|---|---|
| 2.0 (C4a's default) | 1.267 Hz | r = 0.39, **1.17** |
| 1.2 | 0.567 Hz | r = 0.88, **2.90** |

At the stiff setting the neck sits far ABOVE the rate that drives it, responds
quasi-statically, and the head simply RIDES the trunk. That is why nothing was
visible.

**But replacing the one-pole with `twBodyJointStep` on this path FAILS, and the
measurement is decisive.** The head pegged at the clamp (**nod rms 8.4°, peak
10.000° = exactly the range**) at EVERY stiffness from 0.2 to 3.0, with rms
barely moving — saturation, not resonance. The cause:

| | rad/s² |
|---|---|
| angular acceleration the DRAWN trunk series delivers | **mean 55.1, peak 1973.8** |
| what a clean 0.5 Hz / 18° sway demands | **3.10** |

**~640× the real motion, and all of it envelope noise.** The drawn trunk is
`sqrt(power) · cosMet` and `sqrt(power)` steps between 10 ms hops; its second
derivative is dominated by those steps. A second-order joint is an
acceleration-driven device, so it hears the noise and nothing else. (This was
found once already on this branch, retracted a "9° nod" result, and was walked
into a second time — hence recording it here rather than in a commit message.)

**The consequence is architectural: the neck cannot be bolted onto the drawn
series.** It needs the trunk's angle to come out of an INTEGRATOR — smooth by
construction — which is exactly what `twBodyPlantRun` produces and why the
plant's own head nod behaved sensibly. **So the head is blocked on C5's store
plumbing, and that is now measured rather than argued.**

### THE ONE-WAY CASCADE: named in standard terms, and its error sized

Requester's concern: is a multi-mass system coupled by differential equations
not chaotic, and does the neck's finite length not convert the head's momentum
into reaction on the body?

**On chaos: no, and provably not.** The model is LINEAR (small-angle) and
ONE-WAY (parent drives child, no reaction). A linear system has no sensitive
dependence on initial conditions and cannot be chaotic. A fully-coupled
nonlinear double pendulum can be; this is not one, by construction.

**On the reaction: the concern is exactly right and here is its size.** From our
own segment table — head 5.175 kg, `I` about the atlas 0.09107 kg·m²; trunk
32.625 kg, `I` about the hip 2.81270 kg·m²; neck lever 0.504 m — per 1 rad/s² of
head angular acceleration the reaction on the trunk is **0.321 N·m**, perturbing
the trunk's own acceleration by **11.4 %** of the head's. Not negligible, not
dominant. **11.4 % is the honest error bar on the one-way cascade at this
joint.**

**And the gap has a standard name.** Featherstone's Articulated Body Algorithm —
what every ragdoll runs underneath (PhysX, Bullet, Havok, ODE) and what OpenSim
and AnyBody use with real muscle models — has an OUTWARD pass (velocities and
accelerations propagate parent → child) and an INWARD pass (forces propagate
child → parent).

> **We have implemented the outward pass and not the inward one.**

That is not an approximation invented here; it is half of a standard algorithm,
and the missing half is precisely the reaction and constraint forces the
requester described. It also makes the upgrade a known algorithm with open-source
reference implementations to validate against, rather than an open question —
which is the same argument that made the Fable review worth commissioning.

### Order of work

1. The sagittal head-stub readout, then flip `kTrunkSagittal` and re-pin.
2. C5's store plumbing — now the measured blocker for a visible head.
3. Only then the inward pass, gated against Bullet or OpenSim, with the 11.4 %
   figure as the thing to show is actually fixed.

---

## C6 — The derived groove computations become dimensioned

**The defect.** Proposal 44 §1: the counter-tension is a narration. `c_p` is an
energy-weighted residual statistic *interpreted* as muscle tension; with no mass
held against gravity there is no Newton-metre behind it.

**AC6.1** `twGrooveCounterTension` gains DIMENSIONED companions computed from the
plant: stance torque in **N·m**, reported as **static** (holding a lean) and
**corrective** (catching jitter) SEPARATELY, exactly as proposal 40 §3.5 defined
them but computed from mechanics. Per-limb mechanical power in **W**.

**AC6.2 — THE ONE THAT MATTERS.** The existing dimensionless series —
`compliance`, every `twGrooveDeriveMetrics` row, the residual report — are
**byte-identical** to pre-C6. The dimensioned rows are ADDITIVE. If a dimensioned
row is to REPLACE a dimensionless one in the heatmap, that is a separate,
explicitly-decided change with its own before/after comparison, never a side
effect of this milestone.

**AC6.3** The metric lab's new rows carry their UNIT in the id and the label, and
the 0.5-centred signed display convention carries over. A row whose value is not
in [0,1] must not reach a LUT that assumes it is — assert the clamp.

**Task 6.4 (NOT an AC).** M and H reach the Options page (44 §5, M5's page).
Demoted from an AC because it has no mechanical check and blocks on open DECIDE 4:
Options pages are a known headless gap in this repo — `assert-midi-options` is the
single precedent, and nothing builds the Audio page off screen. Either write a
`describe`-based assertion first, or ship it hand-verified and say so in the PR.

**AC6.5** AC-INV holds for everything not explicitly listed in AC6.1.

**NOT gated:** whether the N·m figures describe a real performer; whether
counter-tension predicts anything a listener feels — proposal 40 §2.3's standing
ceiling is unchanged by any of this, and the direct validation experiment
(perturb a groove, measure a real body) still does not exist.

---

## Dependencies and suggested order

```
C0 ──────────────────────────────► independent, land first (drawing bug)
C1 ──────────────────────────────► independent (one mapping + a re-pin)
C2 ──────────────────────────────► independent (new module, no existing path)
        └──► C3 ──► C4 ──► C5 ──► C6
```

C0, C1 and C2 are parallelizable across three Sonnet instances with no shared
files. C3 depends on C2 only for the measures it displays; C4 depends on C2 and
C3; C5 and C6 are strictly serial after C4.

## Standing decisions the requester owes, before the milestone that needs them

| # | Decision | Blocks | Recommendation |
|---|---|---|---|
| 1 | What drives the head | C1 | **D** — the TRUNK, through a one-pole lag. Every unit is already taken, so A/B are degenerate and C costs an aspect bump |
| 2 | Projection: 3/4 oblique, or two orthogonal views | C3 | **(i)** 3/4 oblique |
| 3 | Torque drive vs moving set-point (44 §8 q3) | C4 | **SETTLED 2026-08-30 — TORQUE (A).** See C4b; needs no engine change, and it makes body size a prediction rather than a parameter |
| 4 | M/H global or per-project (44 §8 q2) | C6 | global first, per-project later |

## What the review changed (v1 → v2)

A Fable instance reviewed v1 against the code and returned **NOT READY**. Six
defects, all fixed above; recorded here so a reader can see what the plan used to
claim, and so the same mistakes are not re-introduced.

1. **AC-INV was unimplementable.** Its check hashed `body_probe`'s stdout, which
   never contains payload bytes at all — so the `groove.ev` half was checked by
   nothing — and AC0.4/AC1.2 both REQUIRE that stdout to change, so the invariant
   would have gone red on green milestones. `groove.dyn` was missing from the
   invariant entirely, though the pose reads `cosPhi` out of it. → `--hash-payloads`.
2. **C1's recommendation was wrong.** Every ensemble unit is already mapped
   one-per-part, so re-pointing the head at `bounce` would have made the head and
   the pelvis draw the identical series. → option **D**, the trunk with a lag.
3. **AC2.2 was unreachable.** It pinned 0.9–1.1 Hz for the arm AND forbade
   widening, but 44 §4 defines the arm as upper+fore+hand, whose closed form from
   §3's own table is **0.7346 Hz**. It turned a VERIFY-flagged memory into a hard
   build gate. → verify the source first; assert the DOF actually integrated.
4. **AC5.1 would have re-keyed the groove aspects.** The params blob is shared by
   all three; appending M/H to it passes every gate at defaults and re-analyzes
   everything the first time a user changes M. → `body.pose` gets its own blob,
   plus AC5.6 to assert it.
5. **AC0.3 and AC3.3 had no mechanism** — nothing in the repo can drive the pose
   to a chosen angle. → `assert-puppet-skeleton` takes explicit joint attributes.
6. **AC3.2 and AC3.4 contradicted each other** on renaming `headNod`. → a
   coordinated rename with a re-pin, and AC3.2 excepts that one token.

Also from the review: AC1.2's `<= 2.5` failed on its own success case (2.5003);
its watched-failing passed on the broken binary for wrong-regime fixtures
(`e_static_only` → ratio 1.739), so the fixture is now pinned; AC4.1's drift bound
was ambiguous between secular and pointwise; AC4.4's "no excursion increases"
collided with AC4.3's required overshoot; AC0.4 was demoted from a gate to replica
maintenance, since the probe is engine-side and physically cannot call an
`app/model` function; AC6.4 was demoted to a task.

**What the review confirmed as correct:** the milestone decomposition and the
display/plant split; that `app/model` is reachable by both `timeline` and
`testkit` with no `check_layering.py` edit; that `tw_module(body … DEPS tw_core)`
fits the DAG acyclically; that AC-INV's byte-identity is *achievable* (payload
generation is deterministic); and the 2.50 / 0.63 / 10.01 crossover arithmetic.

## What this plan does not attempt

It does not make the puppet a validated biomechanical simulation, and it does not
close proposal 40 §2.3's ceiling: this predicts the physical response of a
MODELED body, never listener enjoyment. It does not fit anything to the
requester's own body beyond M and H. And it does not revisit the entrainment
ensemble — by design, per the invariant at the top, because that is the part that
measured well and the part everything else is built on.
