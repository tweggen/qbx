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

16. **`dampingRatio` is NOT the effective damping ratio, and a resonance claim
    must read `twBodyJointDampingRatioAt()`.** Because `c` is absolute
    (invariant 5), the ratio the joint actually behaves with is
    `dampingRatio · sqrt(kRef/k)`. A softer joint is therefore effectively MORE
    damped and resonates less sharply. Consequence worth stating: stiffening
    does two things at once — it moves the resonance up AND sharpens it — and
    the resonant amplification over the static deflection is exactly
    `1/(2·zeta_eff)`, which is what the tests assert.

17. **A resonance gate measures amplification over the STATIC deflection, never
    against the response at some other frequency.** A soft joint's quasi-static
    response at `f_n/2` is large simply because it is soft; that ratio was
    measured at 1.5× and says nothing about resonance. Tried, wrong, recorded.

### The objective (`twbodyobjective.h`, C4b)

18. **The objective implements ONE SENTENCE and it is the requester's**, not a
    modelling convenience: *match is enabling the body to express its urge to
    move as intensely as it likes to, over an extended period of time.* Three
    words in it are load-bearing and each is a separate gate — **intensely**
    (the reward is achieved MOVEMENT, never alignment), **as it likes to** (a
    ceiling; overshoot earns nothing), **over an extended period** (effort is a
    BUDGET across a long window, so sustainment is the thing optimised).

19. **THERE IS NO PHASE TERM ANYWHERE, and adding one would make AC4b.2
    circular.** Resonance is expected to EMERGE from the budget — it is what
    lets a body hold a large amplitude inside a fixed metabolic cost — so
    rewarding alignment directly would be assuming the conclusion.

20. **Every quantity is a RATIO, because otherwise §8 q3's reasoning is void.**
    That decision went to torque so the model would rest on nothing body- or
    species-specific; an objective measured in radians or joules would smuggle
    it straight back. Movement is in RANGE-LENGTHS PER SECOND (travel over the
    joint's own ROM), effort in units of the segment's own `m·g·d`, urge in
    fractions of that track's own maximum.

21. **Movement is TRAVEL, not excursion** — `mean|θ̇|`, not peak angle. "Hard
    and sudden" is the requester's phrase and peak excursion cannot express it:
    half the range at 2 Hz is twice as intense as full range at 0.5 Hz, and the
    metric says so. Closed form: `achieved = 4·f·(A/ROM)`, so **full range once
    a second is exactly 4.0** — which is what `maxUrge`'s anchor IS.

22. **Effort counts the POSTURAL torque, and this is the one thing that cannot
    be recovered later from the trajectory.** Omit it and a body straining
    against a beat scores cheaper than one merely standing leant over. The two
    torques ADD before squaring — muscle does not know which half of its own
    tension is postural.

23. **`lambda` is a LAGRANGE MULTIPLIER, not a weight.** It prices the budget
    VIOLATION, never the effort itself, so the ranking is independent of it once
    it binds — swept over 1 … 400 in the test. This is the substantive
    difference from the `match − lambda·effort` form C4b was first written with,
    and the reason the review's measure-dependence finding does not reach it.

24. **Exactly TWO free parameters, both physiological, both stated as ANCHORS
    rather than numbers** so a reader can disagree with them precisely:
    `maxUrge` = 12.0 = *full range, three times a second*; `effortBudget` = 1.0
    = *a torque equal to the segment's own weight moment*. Both VERIFY. Only one
    binds at a time — the budget when the body cannot keep up, the urge when
    the music is not asking much — and **which one bound is an OBSERVATION**
    (`budgetBound`), not a setting. Worth knowing when reading any match: a
    full-range 2 Hz head-bang is 8.0, so at `maxUrge` 12 the ceiling is OFF most
    of the time and the BUDGET is what binds.

25. **Silence is not a performance, and it is not a failure either.** A window
    with no urge scores match 0 and blames nobody. Scoring it 1 would make
    standing still through a quiet passage optimal and would dominate any long
    window containing silence.

26. **`urgeNorm` IS AN ABSOLUTE SCALE, COMPARABLE ACROSS TRACKS** — not each
    track against its own peak. Under a per-track rule a gentle ambient piece
    and a rock build-up both drive the body to its ceiling at their own loudest
    moment, and *"this music demands more movement than that music"* becomes
    inexpressible — while being exactly the claim proposal 44 exists to test.
    Consequence, and it is why the field is clamped rather than trusted: values
    above 1 were impossible under the old rule BY CONSTRUCTION and are not
    under this one. They clamp to 1, which the ceiling makes harmless.

27. **The sub-window is 8 s and the SCALE IS A CLAIM, not a tolerance.** At
    120 BPM that is four bars — a PHRASE. A dancer who pauses two seconds in
    every eight is scored as sustaining perfectly; one who sits out a whole
    phrase is not. At a 1 s sub-window the same dancer reads **0.75** and
    ordinary phrasing would register as failure. Gated as that pair.

28. **A sub-window is worth its own LENGTH, never one share of the window.** A
    ragged tail scored against a full sub-window's urge cost a 24.001 s run a
    QUARTER of its match — to one millisecond. Not an edge case: a real hop grid
    will almost never divide a window evenly, so every run would have paid it.
    Found by the sustainment case reading 0.250 where the closed form says
    0.333, *after* this contract had already warned about the failure mode.
    `windowSec` 24 and `subWindowSec` 8 divide evenly on purpose as well.

29. **A GATE FOR A CLAMP OR A WEIGHTING MUST DRIVE THE CASE IT PROTECTS.** The
    first versions of 26's and 28's gates passed under their own sabotage: the
    ragged tail MET its urge (so weighted and unweighted both scored 1) and no
    sample ever carried `urgeNorm > 1`. Both now under-deliver and over-ask
    respectively. Recorded because a gate that cannot fail is worse than none —
    it reports coverage that does not exist.

30. **`urgeNorm`'s SOURCE is the DRIVE, and the three obvious candidates are
    all circular.** `perUnitPower` (|z|²) and `dissip` (2|α||z|²) are the
    resonator's RESPONSE, so either as "what the music asks for" makes urge and
    achieved the same quantity and match trivially 1. What survives is
    `hypot(support, tension)` — the two `groove.dyn` phase channels are
    `k·F·cos φ` and `k·F·sin φ`, so their magnitude divides the resonator's
    phase, and with it its response, straight out.

31. **THEN DIVIDE BY `k`, because `k` is OURS and `F` is the music.** Measured
    across six committed fixtures: twobar's raw drive reads ~2× every other
    unit's and its coupling is 3.5 against their 1.5. On `a0_broadband_grid`
    the raw p99s are 0.02915 / 0.02896 / 0.06757 for reference / sway / twobar
    and after the divide **0.01944 / 0.01931 / 0.01931** — the same number
    three times. The whole apparent difference was a model constant presented
    as a property of the material. `twGrooveCounterTension::k` carries it, in
    memory only, needing no aspect bump.

32. **Urge is a LEVEL, not an onset train.** Raw `F` is a half-wave-rectified
    envelope difference, so its MEDIAN over every committed fixture is exactly
    0.00000 — zero between onsets. A per-hop urge would be zero nearly always
    and every window mean meaningless. `twBodyUrgeTauSec` = 1 s is DERIVED:
    longer than the fastest metrical period (reference at ~4 Hz, 0.25 s) so it
    does not track onsets, shorter than the 8 s sustainment sub-window so a
    build-up inside a phrase is visible. **A single sample of the smoother is
    not its level** — the ripple is ~exp(−period/τ), measured 24 % of the mean
    at 4 Hz onsets, and asserting an instantaneous sample equals the mean is a
    mistake this file's own gate made and failed 11 % low on.

33. **`twBodyUrgeReference` is a THIRD free parameter and it is the weakest.**
    Unlike `maxUrge` and `effortBudget` it is a CALIBRATION, not a physiology.
    Derived from a measurement (smoothed p99 drive 0.00030–0.00262 across 30
    unit/fixture pairs) plus one stated and arguable assumption (the fixtures
    run −19…−23 dBFS RMS; a commercial master at ~−10 carries ~3× the linear
    envelope). It cannot change WHICH motions the model produces — it
    multiplies every unit's urge equally — only how much of the ceiling a track
    reaches. A full-scale broadband onset-train calibration fixture would
    replace the assumption with a measurement and is **not in the tree**.

34. **This module never computes `urgeNorm` from the analysis.** Deciding which ensemble quantity
    means "urge" is a modelling claim and belongs where it can be argued with,
    not buried in an accumulator. It is an input (C5 supplies it).

Known debt:

- **No child→parent reaction.** A parent drives its child; the child's reaction
  torque does not act back. So an arm swing cannot counter-rotate the trunk, and
  proposal 44's counterswing gate (AC4.2) is NOT met. Named, not faked.
- **No gyroscopic cross-terms BETWEEN axes.** A joint turning fast about two
  axes at once is approximated.
- **Torque-actuator muscles**, per proposal 44 section 6's standing limit.
- **AC2.1 open** — see the header note above.

How to test: `ctest -R "body_measures_test|body_joint_test|body_objective_test"`. Both are plain
executables with no fixtures and no Qt; either can be built and run alone.
