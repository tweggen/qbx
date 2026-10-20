# The Linux gate

**What this file is for.** `ctest` is the safety net (CLAUDE.md), and on each
platform that net has holes. This is the list for Linux, so anyone gating a
change there can tell their own breakage from the standing breakage. Its
siblings are `docs/MACOS_GATE.md` and `docs/ASIO_WINDOWS_GATE.md`.

Unlike those two, this list was not built by hand: **it is what the first CI run
measured.** Before CI existed nobody had a Linux baseline at all, which is why
this file is new and short.

## Baseline

Measured **2026-10-03** on the first-ever run of `.github/workflows/ci.yml`
(`ubuntu-latest`, GitHub-hosted, Qt 6 from `qt6-base-dev`,
`QT_QPA_PLATFORM=offscreen`, `ctest -j4`), at the tip of `feat/ci`:

```
99% tests passed, 2 tests failed out of 400
Total Test time (real) = 367.51 s
```

With both exclusions below applied, the same tree is **green at 398/398** in
367 s.

Also **5 disabled** and not counted: `qxa.au_effect_audible`,
`qxa.au_missing_placeholder`, `qxa.au_slot_roundtrip` (AU is macOS-only) and
`qxa.media_options_page`, `qxa.media_secret_redaction`.

## The two failures

### 1. `secret_store_test` — no keyring on a headless runner

**Not a code defect. Excluded, because it cannot be configured around.**

The test reports `platform default backend on this build: libsecret`, and a
GitHub-hosted runner has no D-Bus session or keyring daemon for libsecret to
talk to.

The first attempt here was to set `SMARAGD_SECRET_BACKEND=memory`, on the
strength of the test's own opening section proving that `resolveBackend()`
honours it. **That did not work, and the reason is worth recording:** the test
does not only ask `resolveBackend()` what it would pick — it goes on to
exercise *the platform default backend* deliberately, resolved independently of
the override ("platform backend under test: libsecret"). Overriding the
environment cannot rescue a test whose subject is what the environment would
otherwise have chosen.

So it is excluded, in the same category as the three macOS-only AU cases: an
environment this runner cannot provide, not a defect.

Two consequences to be honest about. The libsecret backend is **not covered on
Linux by CI** — exercising it needs a session bus and a keyring, i.e. a
desktop, so it stays a manual check. But `libsecret-1-dev` is still installed
deliberately, so the backend is still **compiled**; dropping the dependency
would have turned the test green by removing the code it tests, which is worse
than an exclusion that says what it is.

### 2. `qxa.asset_clip_preview` — SEGFAULT, needs triage

**Excluded from CI, and this is a real open question, not an environment
quirk.** It segfaults in ~3.6 s. Three possibilities, in the order I would
check them:

- the offscreen platform plugin — a preview path that assumes a real window or
  screen would crash under `QT_QPA_PLATFORM=offscreen` and nowhere else;
- a genuine Linux-only bug in the asset preview path;
- a `-j4` interaction, though the other 399 cases tolerate it.

Nothing here distinguishes them: the diagnosis needs a Linux box with a
debugger, which the run that found it does not have. The exclusion lives in
`.github/workflows/ci.yml` as `CTEST_EXCLUDE`, in one place, with a pointer to
this section — **this list is meant to shrink.**

## Flakes seen once in CI and NOT excluded (QBX-200)

**Three tests failed together in run `38911317466` attempt 1** on
`feat/QBX-186-isolate-take`, and **attempt 2 of the same run, on the same
commit with no changes, was green** in 12m46s. So all three are
non-deterministic, none is excluded, and none of them is a reason to read your
own run as broken — but equally, a failure in one of them is not automatically
"the known flake" either. Check the numbers in the log before concluding.

```
 49 - qxa.collect_external_media_missing (Failed)
354 - qxa.sample_missing_survives (Failed)
440 - twlog_test (Failed)
```

### `twlog_test` — FIXED, not excluded (QBX-193)

The assertion was `worst single logf < 2000us` for an RT thread under five-way
contention, i.e. a wall-clock worst case, which is dominated by OS scheduling
rather than by the lock it meant to measure. On a saturated runner it broke with
the design working perfectly; the same run reported 211,295 records dropped to
contention, which is exactly the condition such a ceiling is least able to
survive. It is now gated deterministically by HOLDING the sink's mutex, so
`try_lock` cannot succeed and the drop is a fact rather than a probability. See
`tw/core/CONTRACT.md` invariant 5. **Nothing about it is load-sensitive any
more, so a failure there is a real defect.**

### The two `.qxa` cases — MECHANISM NOT ESTABLISHED

Both load `smaragd/tests/sample_missing_survives.qxp`, and both turn on one
reference being unreachable **on this machine**. That is a claim about the
environment in two ways, and either one would produce the exact failure
signature seen (wrong counts, nothing else):

1. the stored spelling is `~/__smaragd_no_such_dir_missing__/__missing_sample__.wav`
   and the `~` is expanded against **`$HOME`**;
2. if resolution fails, the loader's last-resort recovery adopts any file with
   the **same basename sitting next to the project file**
   (`splainwave.cpp`, `instantiateFromDomElement`) — by design, for projects
   carried as a unit.

A THIRD case, `qxa.load_missing_sample_placed_survives`, loads a different
fixture carrying the SAME `~` path and did not flake. That neither confirms nor
refutes the above: before QBX-200 its only assertion over the miss was
`assert-log contains="__missing_sample__.wav"`, and a line about a file that
RESOLVED carries the basename too ("Name of extern file is …"), so it would have
passed either way. It now carries the same precondition assertion as the other
two, which closed that hole.

**The HOME half is demonstrable on demand**, which is as close to a mechanism as
this got. Running the two cases with `HOME` pointed at a directory that DOES
contain `__smaragd_no_such_dir_missing__/__missing_sample__.wav` reproduces
CI's failure signature closely: `assert-extern-files FAILED: missing is 0
expected 1`, three times in `collect_external_media_missing`, plus
`collect-external-media FAILED: copied 2 expected 1`. CI reported four rejected
actions against this one's four-to-five, so the shapes agree without matching
exactly. What is NOT explained is what could have supplied such a path on a
GitHub runner for one attempt and not the next — attempt 2 was green on the same
commit, so whatever it was, it was transient. **This is a hypothesis with a
matching signature, not a diagnosis.**

**Not reproduced locally in any configuration tried**: 5/5 serially, 490/490 at
`-j4` and 490/490 at `-j24` on a 12-core box (oversubscribed well past CI's
`-j4` on 4 cores), and, after QBX-200, 30 runs of each of the three cases at
`SMARAGD_REVAL_WORKERS` 1, 4, 8 and 16 under a parallel `ctest -j8` of the rest
of the suite — **360/360**. A GitHub runner differs enough in per-core speed and
memory bandwidth that a load-shaped failure may simply be unreachable here,
which is itself worth knowing.

What QBX-200 changed, since the mechanism could not be found: **both cases now
state that precondition as their first assertion** rather than assuming it, so a
machine that supplies either path fails on a named line instead of on an opaque
count, and the loader **announces a placeholder** (`sample NOT FOUND, keeping a
silent placeholder: <stored> -> looked in <resolved>`) where it used to create
one silently. The resolved path is in the message, so a surprising `$HOME`
appears in the run itself.

**And the run now carries the diagnosis.** `ci/gates.sh` exports
`SMARAGD_LOG_CONSOLE=1`, so `ctest --output-on-failure` prints the failing
case's own log. Before that, a failed `.qxa` in CI left only the runner's
summary — "4 actions rejected" and the verb names — while every warning the
actions emitted, including `assert-extern-files FAILED: missing is N expected 1`
and the paths it looked for, was discarded. That is why these two could not be
diagnosed from the run and had to be guessed at. A green run is as quiet as
before, because `--output-on-failure` prints nothing for a test that passed.

## Running the comparison yourself

```bash
./ci/gates.sh                  # build + the four checkers + ctest, excluding nothing
CTEST_EXCLUDE='^(qxa\.asset_clip_preview|secret_store_test)$' ./ci/gates.sh   # what CI runs
```

A local run excludes nothing by default, on purpose: the exclusion is a
statement about CI's environment, not about the test.
