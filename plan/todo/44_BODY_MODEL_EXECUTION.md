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

## C0 — The kinematic chain (display only)

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

## C1 — The head's metrical level

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

## C2 — The measures (44 B0): anthropometrics and closed forms, no motion

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

## C3 — The DOF set: the three anatomical axes, and a neck

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
| 3 | Torque drive vs moving set-point (44 §8 q3) | C4 | A/B once the puppet can show both |
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
