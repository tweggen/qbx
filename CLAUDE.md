# Smaragd (qbx)

A Qt6 / C++17 DAW: the audio engine lives in `smaragd/tw303a/`, the app in
`smaragd/main/`. It runs on Windows (WASAPI, ASIO), Linux (ALSA) and macOS
(CoreAudio).

## Where the knowledge is

The design decisions, the gotchas and the rules this file used to spell out now
live in the thinktank notebook **qbx2** (QBX-113). The code shows *what*;
thinktank shows *why*.

- **At session start** a hook writes `.claude/thinktank-briefing.md` for the
  directory you work in: the rules in scope (notebook-wide first, then the
  repository's, then the components covering that path), ranked, with an
  overflow line for what didn't fit. It reaches the session through the
  `@.claude/thinktank-briefing.md` import in your personal `CLAUDE.local.md`,
  which the Crew installer (`--claude-hooks`) sets up. Without that import you
  only get a truncated preview. Neither the hook configuration nor
  `CLAUDE.local.md` exists inside `.claude/worktrees/*`, so start Claude Code
  in the main checkout and work in the worktree from there.
- **For more**, ask before you grep: `cru_briefing` (or `cru_briefing
  issue_id=QBX-nnn`) and `thinktank_hybrid_search`; `wild_search`,
  `wild_facts` and `wild_timeline` for the why and the who. A narrower path
  briefing shows a component's rules and decisions in full.
- **Write the why back** with `cru_log` (and `conventions=[...]` for a standing
  rule), not into this file.
- **The old long CLAUDE.md is in git history:** `git show 075afbfa:CLAUDE.md`.
  The imported facts point into that commit's line ranges.

## Code map

Start at `docs/ARCHITECTURE.md`. Every module has a `CONTRACT.md` beside its
sources; cross-module protocols are in `docs/contracts/`; every action verb (the
`.qxa` scripting API) is in `docs/ACTIONS.md`; the plans are in `plan/`
(`plan/STATE.md` is the implementation record).

## Build

```bash
./ci/build.sh   [QT_PATH]          # incremental; configures if smaragd/build/ is missing
./ci/build.sh   [QT_PATH] --clean  # clean
./ci/clean.sh                      # drop smaragd/build/, keep the checkout
./ci/install.sh                    # install the dev build into ~/Applications
./ci/uninstall.sh                  # ...and take it back out
```

Every repo in the suite answers to those four verbs; the root `build.sh` and
`rebuild.sh` they replace are gone. `_env.sh` stays at the root, because
nassau-suite sources it from here for the one Qt detection in the product.

`QT_PATH` is the Qt prefix; omit it to auto-detect. Platform details and
prerequisites: `docs/BUILD.md`. With `AUTO_DEPLOY_QT=OFF`, copy
`<QtPrefix>/plugins/platforms/qoffscreen.*` into `smaragd/build/bin/platforms/`
or headless tests hang until they time out.

## Gates before every PR

`./ci/gates.sh` runs all of it in one command, and CI runs that same script, so
the checklist below and what gets enforced cannot drift apart.
`./ci/gates.sh --static` is just the four checkers (seconds, needs only
python3). The commands below are what the script does, kept here because
knowing them matters when one of them fails.

```bash
./ci/build.sh                               # the re-configure registers new .qxa cases
python3 tools/check_layering.py
python3 tools/check_logging.py
python3 tools/check_includes.py
python3 tools/check_tempo_authority.py
ctest --test-dir smaragd/build -j4 --output-on-failure   # scale -j to the machine
```

To pin a flake, run `smaragd/tests/repeat_test.sh <bin> <case.qxa> [N] [workers]`
from `smaragd/tests/cases/`. For record and live cases, loop `ctest -R` instead.

**On macOS the suite is not green**, and `docs/MACOS_GATE.md` names every
standing failure with a ticket beside it so you can tell your breakage from the
standing breakage. Build that comparison **serially** — two `-j4` runs of
identical code differed by 18 tests on the dev box, so a `-j4` set difference
proves nothing. Windows has the same kind of note in `docs/ASIO_WINDOWS_GATE.md`,
and Linux — measured by CI rather than by hand — in `docs/LINUX_GATE.md`
(398/400 at the first run: one SEGFAULT awaiting triage, and one keyring test
a headless runner cannot host).

## Workflow

- A PR is the only route to `main`. An agent opens the PR and stops there; the
  author merges.
- One worktree per branch:
  `git worktree add .claude/worktrees/<slug> -b fix/<slug> main`
- Branch prefixes are `feat/`, `fix/` and `docs/`, plus the YouTrack key when
  there is one (https://nassau.youtrack.cloud, project QBX).
- The PR body says what was gated and what was not.
