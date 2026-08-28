# Proposal 44 — verification of the CURRENT body model (2026-08-27)

> Measured, not asserted. A companion note to
> `plan/proposed/44_BIOMECHANICAL_BODY.md`, which it does not modify: 44 is the
> concept draft for the plant, this is the measurement of what ships today.
> Section numbers below are 44's unless said otherwise.

Requested after watching the M3e puppet: *"the head is subordinate in terms of
movement to the bending of the torso… also movement seemed linear to me and not
honoring weight, sudden stops that people do."* Both readings are correct. This
section is the measurement behind them, so §7's milestones can be scoped against
numbers instead of impressions.

**The instrument** is `tw303a/sidecar/tools/body_probe.cc`, an offline probe like
`clap_probe`/`vst3_probe` and **not** a gate — built by its own
`tools/build_body_probe.sh` rather than as a CMake target, since it links
tw_sidecar's three groove sources and nothing else. It runs the
REAL shipped chain — `twGrooveAnalyzeFrontEnd` → `twGrooveBuildAspectPayloads` →
`twGrooveDecode{Res,Dyn}Payload` → the exact `sFeelFlowPoseAt` formula — over a
committed fixture, then measures the kinematics of the five-part pose the puppet
actually draws. Every "expected" figure it prints beside a measurement is a
CLOSED FORM for a pure sinusoid, never a taste judgement.

Canonical run, quoted throughout:

```
body_probe smaragd/tests/groove/h_fill_break.wav 18.22 22.22
  recovered tatum = 0.2501 s
  seeded ladder: reference=3.999Hz bounce=1.999Hz limbs=1.000Hz
                 sway=0.500Hz twobar=0.250Hz
```

`h_fill_break` is the right fixture precisely because it contains **two bars of
true digital silence** (frames 864000–1056000 + 10452 pad = 18.22–22.22 s), which
is the only "sudden stop" in the fixture set.

> **A fixture in the WRONG regime measures the mis-seed, not the model.** Every
> unit's period is a multiple of the recovered tatum, so a tatum recovered at the
> wrong metrical level puts the whole ensemble at the wrong rate. Measured while
> building the probe: a hand-written 120 BPM backbeat (kick 1 & 3, snare 2 & 4,
> hats on eighths) recovers **tatum = 1.0 s — four metrical levels too slow** —
> because the kick/snare alternation is a stronger autocorrelation peak than the
> hats. That is the octave/meter ambiguity §3.2 already names as genuinely hard,
> not a defect found here; it is why the probe takes a fixture path and prints
> the ladder first, and why a run not showing the ladder above must be discarded
> rather than read.

### 9.1 The puppet has NO kinematic chain above the pelvis

This one is not physics at all — it is `sfeelflowpuppet.cpp`'s geometry, and it
is the direct cause of the reported symptom. `neck` is correctly rotated about
the pelvis by the sway angle, and then **every segment above it is built in
WORLD coordinates from the neck's new position**:

```cpp
headC     = rotateAbout( neck, QPointF( neck.x(), neck.y() - headR*1.35 ), nod );
shoulderL = QPointF( neck.x() - shoulderW, neck.y() + torsoLen*0.06 );
```

`(neck.x, neck.y − r)` is straight up in the WORLD, not along the torso axis, so
the head, the shoulder bar and both arms inherit the neck's **translation** and
none of its **rotation**. Replicating the paint math exactly (probe §G):

| sway | torso lean | neck→head lean | shoulder bar | arm lean |
|---|---|---|---|---|
| 0.0 | 0.000° | 0.000° | 0.000° | 0.000° |
| 0.5 | **10.000°** | **0.000°** | **0.000°** | **0.000°** |
| 1.0 | **20.000°** | **0.000°** | **0.000°** | **0.000°** |
| −1.0 | **−20.000°** | **0.000°** | **0.000°** | **0.000°** |

A torso at 20° with a bolt-upright head and a perfectly horizontal shoulder bar
is a figure with no spine — the head reads as gimbal-mounted, which is exactly
"the head is not subordinate to the torso". **This is independently fixable and
does not wait on any of §7**: build the segments in torso-local coordinates
(rotate each pre-rotation point about the pelvis by the sway angle before the
joint's own rotation). It is not a substitute for the plant — it makes the head
follow the torso RIGIDLY, with no neck lag and no counter-rotation — but it is
the difference between a wrong drawing and a crude one.

### 9.2 The head is driven by the FASTEST unit in the ensemble

`reference` → head nod is stated in proposal 40 M3e AC 4 and implemented
faithfully. `reference` is the tatum-rate gauge, so:

| part | unit | measured f |
|---|---|---|
| head nod | `reference` | **3.999 Hz** |
| pelvis bounce | `bounce` | 1.999 Hz |
| arm swing | `limbs` | 1.000 Hz |
| torso lean | `sway` | 0.500 Hz |
| hip shift | `twobar` | 0.250 Hz |

**head : torso = 7.997 : 1**, three octaves apart, `corr(headNod, sway) = +0.024`
at zero lag and `−0.372` at best lag. They are statistically independent because
they are two independent oscillators three octaves apart — no amount of drawing
fixes that.

Two things make this a design question rather than a tuning one. A head nodding
**four times a second** is not a thing a human body does. And the mapping
contradicts §2's own cited evidence: Burger 2013 measured **low-band flux ↔ head
speed, r = .73**, which points at the LOW-HEAVY unit (`bounce`) — the head was
given the Broad tatum gauge, which is the one unit §3.2 deliberately made
*neutral* so it could serve as the residual reference. It looks like the head got
whichever unit was left over.

### 9.3 The force check — the head is FLUNG, the torso is CARRIED

The probe's §F applies the §3 table (still **VERIFY**-flagged) to the puppet's
own display amplitudes, 75 kg / 1.75 m:

| DOF | driven at | amp | I (kg·m²) | τ inertial | τ gravity | **ratio** | **f_cross** |
|---|---|---|---|---|---|---|---|
| head nod (`reference`) | 3.999 Hz | 10° | 0.0911 | **10.03 N·m** | 1.00 N·m | **10.00** | **1.264 Hz** |
| torso lean (`sway`) | 0.500 Hz | 20° | 2.8127 | 9.69 N·m | 24.83 N·m | **0.39** | **0.800 Hz** |
| pelvis bounce (`bounce`) | 1.999 Hz | 0.105 m | — | 1242.6 N | 735.8 N | **1.69 g** | 1 g @ 1.54 Hz |

`f_cross` is the closed form `I·θ·ω² = m·g·d·sin θ` — the frequency at which a
DOF's inertial demand equals its own gravity moment. **Below it a part is
carried**, by muscles working against its weight; **above it the motion is
inertial and the weight is beside the point.** It is the single most actionable
number in this section, because it says which metrical level a body part may
physically occupy — and it needs nothing but §3's table and an amplitude.

This is the requester's "not honoring weight" as a number. The torso is properly
**gravity-dominated** (ratio 0.39) — a lean costs something, which is what a lean
should do. The head is **inertia-dominated by an order of magnitude** (ratio 10),
continuously, forever. Inertia goes as ω², so the whole factor of 10 is the head's
frequency assignment: put the head on the beat instead of the tatum and it drops
to 2.5; on the half-bar, to 0.6.

**That is the strongest single argument in this proposal's favour**, and it is
available WITHOUT the plant: §3's table plus the puppet's own amplitudes already
tell you which metrical level a body part can physically belong to. The measures
pay for themselves before any ODE is integrated.

### 9.4 The motion is sinusoidal BY CONSTRUCTION — and rests less than a sine wave

Not a measurement but a proof: a pose component is `sqrt(unitPower) · cosPhi` =
`A(t)·cos(φ(t))`. Within one cycle the shape is a cosine; the only freedom is the
envelope. The measurement is what the envelope does about it. Restricting to each
part's longest stretch with energy above 60 % of its run peak (probe §A2):

| part | steady hops | dwell10% | crest(a) | skew(a) | kurt(a) |
|---|---|---|---|---|---|
| **pure sinusoid** | — | **0.0638** | **1.4142** | **0** | **1.5000** |
| headNod | 1512 (15 s) | **0.045** | 2.556 | −0.730 | 3.089 |
| sway(torso) | 108 | 0.120 | 2.654 | −0.155 | 4.952 |
| hipShift | 254 | 0.098 | 1.339 | −0.008 | 1.390 |
| bounceY, armSwing | 94, 61 | *never steady for 1 s* | | | |

The head, the only part that sustains a steady stretch, spends **less** time near
zero speed than a pure sine wave does (0.045 vs 0.0638). It never hangs, never
arrests, never dwells — it is in continuous motion at 4 Hz for fifteen seconds.
Acceleration skew is −0.73 where a body under gravity would be strongly one-sided.

**And the whole-run numbers must not be misread as weight.** Over the full run
`kurt(a)` reads 8.4–10.4 and `dwell10%` 0.51–0.86 for the four non-head parts,
which *looks* body-like — rare impulses among long coasts. §A2 shows why it is
not: those parts **never hold energy for even one second**. Their apparent
gesture IS the envelope collapsing and recharging, not a movement cycle. The two
sections exist to separate exactly these, and reporting §A alone would have
credited the model with a physicality it does not have.

### 9.5 Nothing stops at a break — one part LURCHES

Four seconds of true digital silence (probe §D):

| part | rms pre | rms 1st half | rms 2nd half | rms post | 1st half / pre |
|---|---|---|---|---|---|
| bounceY | 0.4907 | 0.2835 | 0.2139 | 0.3575 | **57.8 %** |
| sway | 0.5078 | 0.1336 | 0.1995 | 0.4062 | 26.3 % |
| armSwing | 0.3359 | 0.1601 | 0.1014 | 0.0727 | 47.7 % |
| headNod | 0.3719 | 0.0965 | 0.0477 | 0.4512 | 25.9 % |
| hipShift | 0.1642 | **0.4786** | 0.4478 | 0.1995 | **291.5 %** |

Two things. The decays are the **linear 1/e ring-down** and nothing else — a
damped resonator has no other option, whatever the material does; a real body
arrests itself in about one movement cycle, and that arrest is muscular work,
the single most legible thing a dancer does at a break. And `hipShift` **grows
to 291 %**: the break is itself a step in the drive, which excites the slowest
(0.25 Hz) unit, so the widest and heaviest part of the figure swings hardest at
the exact moment the music stops. That is not merely "no stop" — it is the
opposite of what a body does.

### 9.6 There is no coupling to correct — confirmed at the integration loop

`runPass1` integrates each unit in its **own** loop over hops, referencing no
other unit's state. The off-diagonal correlations (probe §C) are 0.02–0.37 and
are **shared drive**, never a force one part exerts on another:

```
                bounceY  sway   armSwing  headNod  hipShift
bounceY(pelvis)   1.000  0.366    0.294     0.076    0.159
sway(torso)       0.366  1.000    0.174     0.024    0.143
headNod           0.076  0.024    0.024     1.000    0.009
```

§1's "no counteraction … momentum is not conserved because momentum does not
exist" is exact, and the head column (0.076 / 0.024 / 0.024 / 0.009) shows it is
the head that is most completely disconnected from everything else.

### 9.7 A GAP IN THIS PROPOSAL: the requester's motion has no DOF, in §4 either

The requester names the vital interaction explicitly: *"torso (i.e. bending
forward, returning to upright position), and thus head (pulling it suddenly back
up, letting it fall with torso movement)"*. That is **SAGITTAL** — fore-aft.

The puppet is drawn front-facing, and all five DOF are rotations/translations in
the single screen plane, so what ships is: `sway` = lateral lean, `headNod` = a
lateral head **tilt** (the name is wrong for what is drawn), `armSwing` = lateral
abduction. **There is no fore-aft DOF anywhere.**

**Name all three axes, or this gets misread — it already has been.** "Not
bending forward" is easy to hear as "turning about the vertical axis", and that
is a THIRD motion the model also does not have. The screen plane of a
front-facing figure IS the frontal plane, so `sway` rotates about the **fore-aft
(anteroposterior)** axis:

| Motion | Plane | Axis | In the model? |
|---|---|---|---|
| flexion/extension — bending forward and back | sagittal | mediolateral | **no** |
| lateral flexion — side-bending toward a shoulder | frontal | fore-aft | **yes, this is `sway`** |
| axial rotation — twisting | transverse | vertical | **no** |

The shoulder bar is drawn at a fixed width and never foreshortens, which is what
settles the third row: a twist in a front view would shorten it. So the honest
one-liner is *the model side-bends about the fore-aft axis, and neither flexes
forward about the mediolateral axis nor twists about the vertical one* — and
`armSwing` is frontal-plane abduction, a jumping-jack plane rather than a
walking swing, whatever the code comment about antiphase intends.

**The head-neck gap has a KINEMATIC half and a DYNAMIC half, and they fail
independently.** §9.1 is the kinematic one: the head's orientation does not
inherit the chest's at all (measured 0.000° at a 20° torso). Fixing only that
welds the head to the chest, which is still not a body. The dynamic half is that
the neck base ACCELERATES when the chest pitches, and the head's own inertia
resists it — roughly

```
tau_neck = I_head * ddtheta_head  +  m_head * d * a_base  +  gravity
```

where the middle term is the one that does not exist anywhere in the current
model. It is what makes a head lag going in, overshoot coming back, and need an
active arrest — i.e. exactly the requester's "letting it fall with the torso,
pulling it suddenly back up". It is first-order, not a refinement: it is the
whole difference between a head attached to a body and a head drawn on one.
(Strictly it is not angular momentum *conserved* — gravity and the neck torque
are external — but the inertial-lag effect is what the phrase is reaching for.)

Head-on-neck as a **spherical (3-DOF) joint** — flexion/extension, lateral
flexion, axial rotation — is the right idealization to build against, and is
what would let one model carry all three rows of the table above instead of the
present single hinge. The real neck is a seven-vertebra column that distributes
the motion, but a single ball joint at the base is well inside this project's
own "does not need to be hyper-exact".

And §4's table as drafted would not deliver it either — it puts `torso lean θ_t`
in the **frontal** plane. A first build to that table would produce a
better-behaved lateral sway and still not bend forward. Whatever else §7 does,
**the trunk needs a sagittal flexion/extension DOF, and the head a sagittal
flexion DOF hanging off it**, or the one interaction this was commissioned to
show is still absent. Note the two are physically different: lateral lean is
nearly balanced about the spine, whereas forward flexion works the trunk against
its own full gravity moment (24.83 N·m at 20° from the table above, and it grows
as sin θ) — which is precisely why "returning to upright" reads as effortful and
is worth showing.

### 9.8 What this changes about §7

- **§9.1 is a bug, not a milestone.** Fix the puppet's kinematic chain now; do
  not let a B2 "puppet switch" carry a spine defect into the plant's readout.
- **B0 gets a cheaper and sharper gate than §7 describes.** Before any ODE:
  assert `τ_inertial / τ_gravity` per DOF against the puppet's own amplitudes.
  It already discriminates (head 10.00 vs torso 0.39) and it is what says which
  metrical level each part may occupy.
- **The head's unit assignment is a DECIDE item, ahead of B1.** §2's own cited
  evidence (low-band flux ↔ head speed, r=.73) and §9.3's ω² argument both point
  away from `reference`. Whether the plant later re-derives head motion from
  trunk motion or not, shipping a 4 Hz head in the meantime is indefensible.
- **§4's DOF table needs the sagittal plane added** (§9.7) — this is the
  requester's stated core requirement and is currently absent from the design,
  not merely from the build.
- **B1's counteraction gate should be joined by an ARREST gate**, from §9.5: at a
  drive step to zero, a body must reach rest within about one movement cycle, and
  no part may increase its excursion. The current model fails both, measurably.

### 9.10 CORRECTION (2026-08-28): the frequencies in 9.2 and 9.3 were SEEDS

**Found while executing C1, and it supersedes part of this note.** The "measured
f" column in 9.2, and every frequency in 9.3's force table, were read from each
unit's `omega0` -- its SEEDED register -- not from the series the puppet draws.
That was wrong, and the difference is large.

What a unit's displacement `sqrt(power)*cosPhi` actually does, measured by
zero-crossing rate over the whole run:

| part | seeded | drawn (a_offset15) | drawn (h_fill_break) |
|---|---|---|---|
| headNod (`reference`) | 4.000 Hz | 4.010 Hz | 4.004 Hz |
| bounceY (`bounce`) | 2.000 Hz | **3.948 Hz** | **3.664 Hz** |
| armSwing (`limbs`) | 1.000 Hz | **3.856 Hz** | **3.363 Hz** |
| sway (`sway`) | 0.500 Hz | **3.948 Hz** | **2.591 Hz** |
| hipShift (`twobar`) | 0.250 Hz | **3.917 Hz** | **2.068 Hz** |

Only `reference` -- seeded AT the tatum -- matches its seed. Every other part is
drawn oscillating near the tatum rate, up to **16x** its seeded metrical level.

**The mechanism, and it is not a mis-seed.** The mean UNWRAPPED phase advance
does track omega (h_fill_break: sway 0.526 Hz against a 0.500 Hz seed, twobar
0.258 against 0.250; the omega clamp is working). What the display sees is not
the mean drift but the JITTER around it: `arg(z)` of a linear oscillator driven
far off its resonance is dominated by the FORCED response at the drive rate, and
the drive is impulsive on the tatum grid. So `cos(arg z)` crosses zero at the
drive rate however slow the unit is.

**|z| is not affected, and that matters.** The resonance lives in the MAGNITUDE
-- which is what `unitPower`, the compliance scalar, the heatmap tint and the
whole metric lab read, and which is the part of proposal 40 that measured well.
It is only the PHASE, and therefore only the drawn DISPLACEMENT, that is
drive-locked. `arg(z)` is exactly the right quantity for pass 2's residual
scoring (an event's timing against a common reference phase), which is what it
was built for; using it as a BODY oscillator is the part that does not follow.

**What this changes.** 9.3's ratios were computed at the seeded frequencies and
are therefore understated for every DOF except the head. Re-measured at the
DRAWN rate on `h_fill_break`, at the same amplitudes and the same f_cross:

| DOF | f_cross | drawn | ratio | 9.3 said |
|---|---|---|---|---|
| head | 1.264 Hz | 3.349 Hz | **7.02** | 10.00 (seed) |
| trunk | 0.800 Hz | 2.590 Hz | **10.48** | 0.39 (seed) |

So the trunk is not "carried" at all -- 9.3's one physically sane row was an
artifact of quoting a seed. **Both rotational DOFs are flung.** The direction of
9.3's argument survives (inertia goes as omega^2; frequency assignment is what
costs); its most reassuring number does not.

**What does NOT change:** 9.1 (the kinematic chain, pure geometry), 9.4 (a pose
component is `A(t)cos(phi(t))` BY CONSTRUCTION), 9.5 (the break behaviour), 9.6
(no cross-unit coupling, confirmed at the integration loop) and 9.7 (the missing
sagittal DOF). Those are properties of the code or of measured series, and none
of them rests on a seeded frequency.

### 9.9 What this verification does NOT establish

The §3 anthropometrics are still quoted from memory and **VERIFY**-flagged; §9.3's
torques inherit that flag and are order-of-magnitude demands, not validated
biomechanics. Nothing here measures a real body, so "4 Hz is not a head nod" rests
on the ω² arithmetic and on the movement literature §2 already cites, not on new
data. Only two committed fixtures were probed (`h_fill_break`, `a0_broadband_grid`)
and both are synthetic click material — real music would move every number
somewhat, though not the structural findings (§9.1, §9.4 and §9.6 are properties of
the code, independent of any fixture). The probe is an offline tool and **no gate
in the suite asserts any figure in this section**; re-run `body_probe` rather than
quoting it.
