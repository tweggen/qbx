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

## Running the comparison yourself

```bash
./ci/gates.sh                  # build + the four checkers + ctest, excluding nothing
CTEST_EXCLUDE='^(qxa\.asset_clip_preview|secret_store_test)$' ./ci/gates.sh   # what CI runs
```

A local run excludes nothing by default, on purpose: the exclusion is a
statement about CI's environment, not about the test.
