# tw/body — CONTRACT

Purpose: the anthropometrics and the joint mechanics proposal 44 is built on —
mass, length, centre of mass and inertia per segment; compound-pendulum
composition; the natural-frequency and inertia/gravity-crossover closed forms;
and a driven damped second-order joint with per-axis degrees of freedom, hard
limits and an exact closed-form step.

Public headers: `tw/body/twbodymeasures.h`, `tw/body/twbodyjoint.h`.

Depends on: **nothing**. Pure, deterministic, single-threaded, header-clean —
no engine state, no I/O, no Qt, no threads, no `rand()`, not even `tw/core`.
That is deliberate: everything here is arithmetic over its arguments, so it can
be reasoned about and re-derived by anyone, which is exactly what an adversarial
review of it needed.

**Every constant in the segment table is UNSOURCED and carries a `VERIFY`
marker. AC2.1 is OPEN.** Read `twbodymeasures.cc`'s header for what was searched
and refused. The consequence for anyone writing a test here: assert SIGNS,
RELATIONS, IDENTITIES and CONSERVATION, and pin a magnitude only where it is a
closed form of the inputs. Never assert agreement with a remembered literature
range — proposal 44's own arm-frequency range was such a number and it produced
an unresolvable gate.

Invariants:

1. **Gravity acts on a segment's ABSOLUTE angle, never on its joint's relative
   one.** The equation is

   ```
   I th'' + c th' + [ k_pass -/+ mgd + m d L phi'^2 ] th
                     = -( I + m d L ) phi''  +/-  m g d phi  + tau_muscle
   ```

   with the upper sign for an inverted segment. The `+/- m g d phi` forcing is
   the half of gravity that belongs to the PARENT's angle, and omitting it is
   not a refinement — it is O(100 %) of the response. The falsifier, and it
   needs no constants: **a freely hanging arm on a leaning trunk hangs PLUMB.**
   Without the term it settles parallel to the leaning trunk. `body_joint_test`
   section 3a, and it is the reason `twBodyJointStep` takes a
   `twBodyParentMotion` rather than a bare acceleration array.

2. **A driven joint's resting place is NOT zero, and a gate that asks "has it
   come to rest" must measure from `twBodyJointEquilibrium()`.** An inverted
   head on a held lean DROOPS; a hanging arm settles at minus the lean.
   Measuring the ring or the arrest about zero is only the same question under
   the equation invariant 1 forbids, which is how the first build's arrest
   assertion came to be satisfiable.

3. **Every quantity is PER AXIS, and the axial axis is not the sagittal plane
   with a different name.** Twisting a segment neither raises nor lowers its
   centre of mass, so about its long axis there is **no gravity moment** — hence
   no `+/- mgd` term, no inverted-pendulum stability threshold and no crossover
   — **no parent lever** (this joint sits ON the parent's long axis, so a parent
   twisting produces no linear acceleration here), and **a different inertia**:
   `inertiaLong = m·r_long²`, with no parallel-axis `d²` term because the CoM
   sits on that axis. `twBodyJointInertia`, `twBodyJointStiffness` and
   `twBodyJointDamping` all take a `twBodyAxis` for this reason. A joint that is
   unstable in flexion can be perfectly stable in twist.

4. **`stiffnessScale` is expressed in units of the segment's own `m·g·d`, and
   about the long axis that unit is BORROWED.** There is no gravity moment there
   to supply a scale, so using `m·g·d` anyway keeps axial stiffness the same
   ORDER as sagittal without inventing a second unsourced constant. What it does
   NOT do is add or subtract a gravity term. State this wherever the number is
   quoted; it is a modelling choice, not physics.

5. **Damping is stored as an ABSOLUTE coefficient, never as a live ratio.**
   `dampingRatio` is a convenience read once against a REFERENCE stiffness
   (passive tissue plus the MAGNITUDE of the gravity moment, positive whichever
   side the mass sits). A ratio ties `c` to `√k`, so it would vanish exactly at
   an inverted joint's stability threshold — where it is needed most — and
   modulating stiffness would silently modulate damping. That last one is
   structural, not cosmetic: C4b makes stiffness a per-hop control variable, and
   the centripetal term already makes it vary within a run.

6. **`exactStep` handles NEGATIVE stiffness, and it is one discriminant, not a
   special case.** With `sig = c/2I` and `w2 = k/I` the roots are
   `-sig ± sqrt(sig² - w2)`; `k < 0` merely makes the discriminant large enough
   that one root turns positive, so the overdamped cosh/sinh branch is already
   the right closed form and the solution grows. Folding `k < 0` into "no
   stiffness" discards the DAMPING too and holds a joint that should fall over
   perfectly still — it reports the instability and does not exhibit it.

7. **The model is LINEAR in the joint angle, and exactness of the INTEGRATOR is
   not exactness of the MODEL.** `exactStep` is exact to ~2e-15 against RK4 for
   coefficients held constant across a step; the equation it steps is a
   linearisation. A hanging joint softens ~1.2 % at 25°, an inverted one with
   linear tissue hardens ~4.6 %, and the linear droop OVERSTATES the nonlinear
   one (25° against 20.4° at the module's own defaults). Quote which is which.

8. **Centripetal stiffening is not optional above ~1 Hz.** `m·d·L·φ′²` always
   ADDS and grows as the square of the rate: against the neck's 5.773 N·m/rad it
   is +39 % at 1 Hz and **+618 % at 4 Hz**. So the effective resonance under a
   fast drive sits above `twBodyJointNaturalHz`, and anything tuning a stiffness
   toward a drive rate (C4b) must account for it rather than discover it.

9. **A CONSTRAINED axis does not move — zero, not "very stiff".** That is what
   makes a hinge a hinge rather than a tightened ball, and it is asserted, not
   assumed.

10. **`twBodyJointState::limitHits` counts DWELL, not arrivals.** It increments
    every step spent pressed against a stop, so a second at the limit at
    dt = 0.2 ms scores 5000. Read it as "did it ever reach the stop"; never as
    a count of events.

11. **`twBodyCrossoverHz`'s narrative holds for a HANGING segment only.** For a
    part whose mass sits above its joint, gravity is a BIAS that must be met at
    every frequency including DC — it does not average out and no rate makes it
    irrelevant. What the comparison means there is the narrower claim that
    inertia dominates the OSCILLATORY torque. It is meaningless about a long
    axis (invariant 3).

12. **`radiusGyrLong` is VERIFY by a WEAKER route than the columns beside it.**
    The other four are remembered table values; this one comes from a stated
    geometric model (uniform solid cylinder, `r_long = R/√2`) over an assumed
    slenderness. An assumption written down, not a citation. Nothing may gate on
    its VALUE — `body_measures_test` asserts only identities, orderings and
    scaling laws over it.

13. **`tau_muscle` is SPLIT, and the postural half is a GAIN, not a torque.**
    Proposal 44 §8 q3 settled the ensemble as driving the plant with TORQUE, so
    nothing in the musical signal knows which way is down, and an inverted
    segment needs a standing hold just to stay up. Left in one lump that hold
    rides inside the musical torque as a DC offset — an opinion about posture
    smuggled in as a constant, in a model built to answer exactly that
    objection. So:

    ```
    tau_muscle = tau_postural( body, pose )  +  tau_active( ensemble )
    ```

    `tau_postural` is DERIVED with no free parameter — whatever cancels
    `m·g·d` — and it lives in `posturalGain` rather than in the `activeTorque`
    array **because it is feedback on the same absolute angle gravity acts on**.
    Scaling BOTH gravity terms (the stiffness one and the parent-angle forcing
    one) by `(1 − gain)` makes the cancellation exact at any `dt`; an explicit
    torque sampled at the top of a step leaves a residual that grows with `dt`,
    and "fully compensated" would then not hold still, so nothing could gate it.

    **Scale BOTH or neither.** Compensating only the stiffness moves the
    equilibrium, which is the natural mistake and is watched failing. A
    correctly *partly* toned free joint still settles plumb — the gain changes
    the strength of the pull, not its direction — and only at gain 1 does the
    preferred position disappear altogether.

14. **`stiffnessScale > 1` is a claim about an UNACTIVATED joint and about
    nothing else.** With postural tone an inverted joint has no stability
    threshold at all: a live person's head is held up by muscle, not by
    ligament. Never quote the threshold as a fact about people.

15. **The postural hold COSTS torque, and an effort term must count it.**
    `twBodyPosturalTorque()` exists for C4b's objective: an effort measure
    counting only the active torque would report a body straining against a
    beat as cheaper than one merely standing leant over. It is LINEARISED to
    match what the coefficients apply — a compensation computed against the
    true sine would not cancel what the equation integrates, and the two must
    agree or nothing is gateable.

Known debt:

- **No child→parent reaction.** A parent drives its child; the child's reaction
  torque does not act back. So an arm swing cannot counter-rotate the trunk, and
  proposal 44's counterswing gate (AC4.2) is NOT met. Named, not faked.
- **No gyroscopic cross-terms BETWEEN axes.** A joint turning fast about two
  axes at once is approximated.
- **Torque-actuator muscles**, per proposal 44 section 6's standing limit.
- **AC2.1 open** — see the header note above.

How to test: `ctest -R "body_measures_test|body_joint_test"`. Both are plain
executables with no fixtures and no Qt; either can be built and run alone.
