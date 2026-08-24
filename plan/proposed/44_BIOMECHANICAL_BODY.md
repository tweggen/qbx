# Proposal 44 — The biomechanical body: measures, forces, counteraction

> **Status: CONCEPT DRAFT v1 (2026-08-24).** Direction set by the requester
> after reading proposal 40's model disclosure ("the physical model contains
> no masses and no limb lengths"): *"to achieve realistic numbers for the
> excitation and the spatial extent, yielding in counteraction, we require
> to compute the forces, hence the measures. Otherwise we would just
> arbitrarily bounce numbers."* This proposal is that requirement made
> concrete. Anthropometric values below are quoted from memory of the
> standard sources and are marked **VERIFY**: B0's first act is checking
> every number against the primary literature, the same two-pass survey
> discipline proposal 40 §2 used.

Prerequisite reading: `plan/proposed/40_GROOVE_RESONANCE.md` §3.2–§3.5 and
M3c/M3e (the pendulum ensemble, the exported dynamics, the puppet's Pose
seam — deliberately built as the display boundary this proposal plugs into).

## 1. What is wrong with the current model, stated as physics

Proposal 40's ensemble is five *independent* driven damped oscillators.
Each can entrain, resonate and decay — but the units share no forces, so:

- **No counteraction.** An arm swing cannot react on the torso; a torso
  lean cannot demand a stance correction. Momentum is not conserved
  because momentum does not exist — there is no mass to carry it.
- **No spatial extent.** Excursions are normalized per unit to its own run
  peak; "how far" is a display constant, not a length.
- **The counter-tension is a narration.** §3.5's c_p is an energy-weighted
  residual statistic *interpreted* as muscle tension. Without a mass held
  against gravity there is no Newton-meter behind the number.

The requester's original conception (proposal 40 §1, refinement 4) was a
**musculoskeletal claim, not a metaphor**. This proposal builds the
skeleton the claim needs.

## 2. Architecture: keep the neural layer, add the plant

The literature split (proposal 40 §2.4) is real and the design follows it:
temporal prediction is MOTOR (ASAP hypothesis, Patel & Iversen; Morillon &
Baillet), but execution is MECHANICAL. A purely passive body does not
dance — it needs an intention signal — and a purely neural oscillator does
not push back — it needs a body. So:

- **Layer 2 (existing, unchanged): the entrainment ensemble** becomes
  explicitly the *neural/motor-intention* layer: its per-unit phase and
  amplitude are the predictive clock.
- **Layer 2b (NEW): the biomechanical plant** — a multi-segment body with
  real masses and lengths, driven by muscle torques, subject to gravity
  and joint mechanics, integrated as an ODE. The ensemble's output becomes
  the plant's *muscle excitation*; the receptive fields keep their role as
  activation routing (kick/bass excites the legs/pelvis musculature,
  highs the arms — Burger 2013 unchanged).
- **Readouts move down a level**: joint angles → the puppet Pose (the M3e
  seam, unchanged in shape); muscle torques and powers → *dimensioned*
  counter-tension (N·m) and movement energy (W, J); the stance
  controller's sustained torque → the "held lean" with units.

## 3. The measures

Segment parameters as fractions of total body mass **M** and stature
**H** — the standard tables (de Leva 1996's adjustment of
Zatsiorsky-Seluyanov; Winter, *Biomechanics and Motor Control of Human
Movement*; lengths after Drillis & Contini 1966). Defaults M = 75 kg,
H = 1.75 m; both are USER PARAMETERS — the correlate-with-my-body
experiment becomes literal. **Every value below: VERIFY in B0.**

| Segment | Mass (%M) | Length (%H) | CoM from proximal (%L) | r_gyr about CoM (%L) |
|---|---|---|---|---|
| Head + neck | ~6.9 | ~0.130 (head height) | ~50 | ~30 |
| Trunk (C7→hip) | ~43.5 | ~0.288 | ~45 | ~37 |
| Upper arm (each) | ~2.7 | 0.186 | ~58 | ~28 |
| Forearm (each) | ~1.6 | 0.146 | ~46 | ~28 |
| Hand (each) | ~0.6 | 0.108 | ~79 | ~30 |
| Thigh (each) | ~14.2 | 0.245 | ~41 | ~33 |
| Shank (each) | ~4.3 | 0.246 | ~45 | ~26 |
| Foot (each) | ~1.4 | 0.152 (length) | — | — |

Plus two non-anthropometric constants with their own literature: **leg
stiffness** in bouncing/hopping stance (Farley, Ferris et al. — order
10–30 kN/m for two-legged bounce; VERIFY) and passive **joint
stiffness/damping** for ankle/hip/shoulder (posture literature; VERIFY).

## 4. The model

**Degrees of freedom (first build, deliberately small):**

| DOF | Plane | Mechanism | Replaces ensemble unit |
|---|---|---|---|
| Vertical bounce z | vertical | whole-body mass on leg-spring (stiffness k_leg, damped) | `bounce` |
| Torso lean θ_t | frontal (side-to-side) | inverted pendulum on the pelvis, PD-stabilized | `sway` |
| Arm swing θ_a (L/R) | sagittal | compound pendulum (upper arm + forearm + hand) about the shoulder, shoulder rides ON the trunk | `limbs` |
| Head nod θ_h | sagittal | small pendulum on the trunk top | `reference` |
| Pelvis shift x | frontal | CoM excursion over the base of support, PD-bounded | `twobar` |

Note the planes differ per DOF — lateral sway, sagittal arm swing — so
this is NOT one planar model. First build: per-DOF planar dynamics **with
the momentum coupling terms written explicitly** (shoulder acceleration
enters the arm equation; arm reaction torque enters the trunk equation;
trunk lean shifts the CoM the stance controller must catch). Full 3D
rigid-body dynamics is a later rung nothing here forecloses.

**Forces:**
- Gravity on every segment (this is what makes a lean COST something).
- Muscle torque per DOF: `τ = G · excitation(t)` — excitation from the
  entrainment layer's phase/amplitude through the receptive-field routing;
  the gain G (N·m per unit excitation) is the one genuinely free scale,
  fitted once in B0 so peak excursions land in the motion-capture range
  (Toiviainen's eigenmovement amplitudes) — fitted, stated, and then
  FROZEN, never per-track.
- Passive joint stiffness + damping (literature values).
- The **stance controller**: a PD regulator holding the trunk upright and
  the CoM over the base of support. Its output IS the counter-tension —
  now in N·m, with the static part (holding a lean) and the corrective
  part (catching jitter) exactly as §3.5 defined them, but computed from
  mechanics instead of narrated from phase.

**Emergent resonance as the validation gate.** With real measures the
natural frequencies are CLOSED FORMS, and they must land where the
movement literature already measured them — that is the whole difference
between computed and "arbitrarily bounced" numbers:
- arm as compound pendulum about the shoulder: ~0.9–1.1 Hz at H = 1.75
  (walking-arm-swing range — VERIFY against the pendular-limb literature);
- vertical bounce: f = (1/2π)·√(k_leg/M) ≈ 2–3 Hz at 20 kN/m / 75 kg —
  bracketing van Noorden & Moelants' ~2 Hz body-resonance anchor, which
  proposal 40 could only *assert* and this model *derives*;
- the seeded ensemble periods stop being body claims entirely — they
  remain the neural layer's metrical grid, which is what they measured
  well all along.

**Numerics.** Semi-implicit (symplectic) Euler or RK4 at the envelope rate
(~200 Hz), exact exponential step for every linear sub-part — proposal 40
§11.5's forward-Euler lesson is a standing house rule. Deterministic,
pure, no Qt, no threads: a pass-3 function beside the pass-1/pass-2
pendulum code.

## 5. Integration into the existing pipeline

- **A third analysis pass**: after pass 1/2, run the plant over the same
  envelope field + entrainment output. Export per-hop joint state as a new
  aspect (`body.pose` v1: per DOF {angle, velocity, muscleTorque}), keyed
  by the same content/params hashes plus M/H in the params blob (a
  different body is a different analysis — correct, and cheap because the
  store already keys on params).
- **The puppet displays the plant** — `sFeelFlowPoseAt` grows a branch
  that prefers `body.pose` when present and falls back to the M3e direct
  mapping when not. The Pose seam was built for exactly this swap.
- **The metric lab gains dimensioned rows**: stance torque (N·m, static +
  corrective separately), per-limb mechanical power (W), ground reaction.
  The 0.5-centered signed display convention carries over.
- M/H (and later per-segment overrides) on the M5 Options page.

## 6. Honest limits, named now

Torque-actuator muscles, not Hill-type models; a PD stance controller with
literature-plausible, not subject-fitted, gains; no feet-leave-the-ground,
no steps, no full 3D in the first build; and the standing ceiling from
proposal 40 §2.3 — this predicts *physical response of a modeled body*,
never listener enjoyment. The direct validation experiment (perturb a
groove, measure a real body) still does not exist; what changes is that
every number on the way there now has units.

## 7. Milestones (sketch — ACs at each kickoff, per house rule)

- **B0 — measures + closed forms, no integration.** Verify every table
  value against primary sources; implement the anthropometric model
  (M, H → segment masses, lengths, inertias); ctest gates on the
  closed-form natural frequencies against the literature ranges above.
  The "realistic numbers" gate, before any motion.
- **B1 — the plant.** The ODE with gravity, muscle drive, stance
  controller; driven by RECORDED entrainment output from the existing
  analysis; gates: energy conservation with drive off, bounded response
  with drive on, the counterswing (arm impulse ⇒ measurable trunk
  reaction of opposite angular momentum — THE counteraction gate, closed
  form from the momentum balance).
- **B2 — the aspect + the puppet switch** (`body.pose`, the Pose-seam
  branch, v1-orphan discipline as always).
- **B3 — dimensioned metric rows + M/H personalization UI.**

## 8. DECIDE (requester)

1. First build scope: the 7-DOF table above, or trimmed further?
2. M/H entry: global option, or per-project (a project "performed by" a
   body)?
3. Does the entrainment layer's output drive the plant as TORQUE (pure
   intention → force) or as a moving SET-POINT the PD controller tracks
   (intention → posture target)? The second is closer to how the motor
   literature describes rhythmic movement; the first is simpler. B0/B1
   can A/B them against your own body-correlation once the puppet shows
   both.
