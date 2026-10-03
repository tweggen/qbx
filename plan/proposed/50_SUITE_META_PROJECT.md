# Proposal 50 — One superproject: the suite is the product

> **STATUS: PROPOSED.** Surveyed against `main` at `0305a735` (2026-09-22) and
> against the sibling checkouts `../nassau-plugin-sdk` (`db15697`),
> `../nassau-analogue` (`bb31eff`), `../nassau-eq` (`12f8ff0`) and
> `../nassau-zermatt` (`6b4ec98`). Every file:line below is from those trees.
>
> **The shape is decided by the requester** (2026-09-22): one superproject, and
> the SDK's usage aligned across the plugin repos. **All five questions are now
> answered (§8).** Two of the answers moved the design rather than confirming
> it: the plugins ARE wanted in other hosts (Q5), which retires the bundled-copy
> idea an earlier draft proposed and makes system-wide installation the shipping
> mechanism (D9); and the existing `plugin_*` coverage turns out to test the
> host against in-repo fixtures only (Q3), which resizes M4. The staging and the
> decisions in §2 are what this document adds.

## The defect this closes

**The shipped product has no definition.** Smaragd and the Nassau plugins are
one thing to a user and six unrelated repositories to us. There is no commit,
tag or manifest anywhere that answers "which plugin builds went out with which
DAW build", no build that produces the whole product, and no test that has ever
seen a shipped plugin loaded by a shipped DAW.

Three concrete symptoms, all present today:

| Symptom | Evidence |
|---|---|
| The SDK is consumed three different ways | `nassau-eq` pins it as a `sdk/` submodule (`../nassau-eq/.gitmodules`) *and* falls back to a sibling path; `nassau-analogue/CMakeLists.txt:47` and `nassau-zermatt/CMakeLists.txt:48` only ever look at `../nassau-plugin-sdk`, unpinned |
| The per-plugin build prologue is copy-pasted | `nassau-analogue/CMakeLists.txt:1-43`, `nassau-eq/CMakeLists.txt:1-38`, `nassau-zermatt/CMakeLists.txt:1-43` are the same MSVC `/MT` + OBJC-enable + warning-flag block. `nassau-analogue/CMakeLists.txt:8-11` says so out loud: *"Mirrors nassau-zermatt/CMakeLists.txt verbatim"* |
| Nothing is built or tested automatically, anywhere | The only workflow in any of the six repos is `../nassau-eq/.github/workflows/ci.yml.disabled`, and it is disabled because the private SDK submodule needs a cross-repo PAT (`NASSAU_CI_TOKEN`) that was never provisioned |

And nothing packages anything. `qbx` has no `CPack`, no `install()`, no
`codesign`, no notarization step; `smaragd/main/CMakeLists.txt:770` builds a
`MACOSX_BUNDLE` and that is where shipping stops. The plugins drop bundles in
`build/out/*.vst3|.component|.clap` and that is where *they* stop.

## Reading list, before any milestone

1. `nassau-plugin-sdk/cmake/NassauPlugin.cmake` (216 lines) — the whole shared
   surface. Its header comment already documents the consumption contract:
   set `NASSAU_SDK_DIR`, `include()` the module **from top-level scope** (iPlug2
   must be able to enable OBJC/OBJCXX), then `add_subdirectory(Source/Plugin)`.
   Note the platform gate at `:50-54`: `FATAL_ERROR` on anything but macOS and
   Windows.
2. `nassau-plugin-sdk/cmake/ProvisionDeps.cmake` (135 lines) — and read its
   header twice. **It writes into the iplug2 submodule tree**
   (`external/iplug2/Dependencies/IPlug/`): a VST3_SDK link plus shallow clones
   of CLAP_SDK and clap-helpers. This is by design (iPlug2 gitignores those
   paths) but it means an SDK *checkout* is mutated by provisioning — which
   constrains the superbuild (D4).
3. `nassau-plugin-sdk/scripts/fetch-skia.sh:23-26` — the Skia UI libs come from
   a release asset (`skia-mac-arm64.tar.gz`, tag `skia-mac-arm64-v1`) of the
   **private** `tweggen/nassau-plugin-sdk`. macOS/arm64 only; Windows uses
   iPlug2's vendored NanoVG/GL2 and needs no provisioning at all.
4. `CLAUDE.md` § "Gates before every PR" — the five commands that are currently
   a human checklist and become M3's CI job.
5. `docs/BUILD.md:132` — the `plugin_*` qxa cases, silently dropped when the
   `smaragd/third_party/` submodules are absent. These are the integration gate
   M4 turns into the thing that makes the suite one product.

## 1. The shape

**Polyrepo, plus one thin superproject.** A new repo — `nassau-suite` — whose
git submodules pin `qbx`, `nassau-plugin-sdk` and each shipped plugin to exact
commits. The pin set *is* the product: "Smaragd 1.1" becomes one commit in
`nassau-suite`, reproducible and bisectable.

It holds four things and no source code:

```
nassau-suite/
  .gitmodules            # the pins — qbx, sdk, analogue, eq, zermatt, …
  CMakeLists.txt         # superbuild (ExternalProject_Add per component)
  ci/                    # build.sh, gates.sh, package.sh — the real logic
  packaging/             # macOS .pkg, Windows Inno/WiX, Linux
  CHANGELOG.md           # the suite's, assembled from the components'
```

Rejected, with reasons:

- **Monorepo.** The visibility split is a product decision we would have to
  abandon: `qbx`, `nassau-analogue`, `nassau-mangrove`, `nassau-mangrove2` are
  public; `nassau-eq`, `nassau-zermatt`, `nassau-plugin-sdk` are private. The
  dependency stacks barely overlap (Qt6 vs iPlug2/Skia). Monorepo buys atomic
  cross-cutting commits we do not currently need and costs the ability to
  open-source one plugin at a time.
- **A package manager** (Conan, vcpkg registry). Correct at a different
  headcount. The SDK is source-only CMake glue; `fetch-skia.sh` is already the
  twenty-line version of binary dependency provisioning and it works.
- **`git subtree` / vendored copies.** Loses the pin trail, which is the single
  thing we are trying to acquire.
- **`repo` / `west` / `vcs-tool` manifests.** More machinery than six
  submodules justify. Revisit past ~15 components, or if pinning *branches*
  rather than commits ever becomes what we want.

**Forge portability** (the requester's constraint) falls out of this: submodules
are a git feature, not a GitHub feature, so the superproject moves to Codeberg
unchanged. The only forge-coupled artifact is CI, and D6 keeps that thin.

## 2. Decisions

**D1 — The suite repo is `nassau-suite`, private.** Brand-level, not
product-level, so it survives the DAW being renamed. Private because it pins
private components and will hold signing configuration.

**D2 — One SDK resolution order, in the SDK's own module, not in each plugin.**
`NASSAU_SDK_DIR` if explicitly set (what the superbuild passes) → a sibling
`../nassau-plugin-sdk` checkout → `FetchContent` of a pinned SDK tag. Exactly
one mechanism, three fallbacks in a fixed order, written once.

**D3 — `nassau-eq`'s `sdk/` submodule is deleted.** It is the second pin of the
SDK, and a second pin is a second answer to "which SDK shipped". After M1 the
release pin lives in `nassau-suite` and the standalone-developer path is D2's
sibling/FetchContent fallback. This is a deliberate, small regression for
anyone cloning `nassau-eq` alone: one extra step (`--recurse-submodules` is
replaced by having the SDK sibling, or by letting FetchContent do it).

**D4 — The superbuild is `ExternalProject_Add`, not `add_subdirectory`.** Two
independent reasons. (a) `qbx` needs a Qt prefix and its own configure dance
(`build.sh` → `_env.sh` → `qt_stamp_differs`, the QBX-103 guard at
`build.sh:38-52`); the plugins need iPlug2 to enable OBJC/OBJCXX from
*top-level* scope. These cannot share one configure scope. (b)
`ProvisionDeps.cmake` mutates the SDK checkout in place — so provisioning runs
**once, before** the fan-out, never concurrently from N plugin configures.
`ExternalProject` gives us that ordering as a dependency edge; a single
`add_subdirectory` tree does not.

**D5 — The suite version is the only *user-facing* version.** Since the plugins
are never sold separately (Q1, answered), no user ever needs to compare a plugin
version against anything. So: the suite carries the product version, it is what
every About box shows and what a bug report quotes, and it is the only number in
the release notes. Components keep their own semver internally — still
lockstep-free, because a plugin bump no longer implies a release — but it becomes
a *developer* number, stamped into artifacts for forensics rather than displayed
as a headline. The SDK is the one exception that keeps a real semver contract,
because it has consumers (D2's pinned tag).

Note `smaragd/main/CMakeLists.txt:818` hard-codes
`MACOSX_BUNDLE_BUNDLE_VERSION "1.0.0"` beside `smaragd/CMakeLists.txt:2`'s
`project(smaragd VERSION 1.0.0)` — two literals for one number, and M5
collapses both into the suite-supplied value.

**D6 — CI logic lives in scripts; workflow files only call them.**
`ci/build.sh`, `ci/gates.sh`, `ci/package.sh` per repo. A move to
Woodpecker/Forgejo Actions then rewrites YAML, not engineering.

**D7 — One YouTrack project (QBX) with a `Subsystem` field**, values `DAW`,
`SDK`, `Analogue`, `EQ`, `Zermatt`, …. One backlog, one board, one
"what is in 1.1" query. The decisive practical argument: ticket keys stay
`QBX-nnn` across all repos, so the single thinktank notebook (qbx2) and the
`cru_briefing` workflow keep working instead of fragmenting into six notebooks.
Q1's answer settles this rather than merely supporting it: with no separately
sold plugin there is no separate release cadence and no separate external
audience, so there is nothing for a second project to track. Revisit only if
that changes.

**D8 — Mangrove is product (Q4) and joins the suite via M7; `nassau-mangrove`
is history.** The two repos are not two versions of one thing to be chosen
between — one is simply dead. `nassau-mangrove` is a JUCE project with no CMake
at all (only `Mangrove.jucer`), 25 commits, **last touched 2020-12-04**.
`nassau-mangrove2` is the live one: 62 commits, last touched 2026-08-15,
`project(MangroveCompressor VERSION 4.0.0)`.

So: archive `nassau-mangrove` (keep the repo, mark it so, stop reading it), and
refactor `nassau-mangrove2` onto the SDK in M7. The refactor the requester
called for is precisely the SDK's reason to exist — mangrove2 today vendors its
**own** `external/iplug2` (`nassau-mangrove2/CMakeLists.txt:24`), points at its
own `VST3_SDK_ROOT` and carries a hand-rolled `Source/VST3` target
(`:17-18`), which is three private copies of what `NassauPlugin.cmake` already
does once. It never mentions `NASSAU_SDK_DIR`.

Until M7 lands it cannot be pinned into a superbuild that provisions one shared
SDK, so the suite ships without it and M7 adds it.

**D9 — One copy, in the standard system directories. The plugins install
system-wide, and that is the only shipping location.**

An earlier draft of this document proposed the opposite — plugin bundles carried
*inside* `Smaragd.app`, with system-wide installation as a deselectable extra —
on the reasoning that plugins never sold separately (Q1) are really the DAW's
built-in effects. **Q5 retires that idea**: the plugins are wanted in other
hosts. A bundle-internal copy cannot serve another host, so the system-wide
install has to exist regardless; and once it exists, a second copy inside the
app buys nothing and costs the shadowing hazard (two copies on disk, no
indication which one a session loaded).

So: `Smaragd.app` → `/Applications`; each bundle →
`/Library/Audio/Plug-Ins/{VST3,Components,CLAP}` and the Windows equivalents.
One copy each, the locations every host already scans.

This costs nothing in the DAW, which is the pleasant part:
`tw303a/plugins/src/twpluginsearchpaths.cc:207-208` already scans both the
system and per-user `/Library/Audio/Plug-Ins/<folder>` trees, and `:216` already
honours the `CLAP_PATH` / `VST3_PATH` environment lists. Smaragd will find
system-installed Nassau plugins the day the installer places them, with no code
change — and M4 gets its staging-directory hook from the same env vars for free
(M4, §6).

The drift D5 worried about is handled by the installer instead: app and plugins
are written by one package, so they update together.

**D10 — The SDK repository goes public if, and only if, a licence review of the
third-party terms clears it.** Q2's answer removes the owner's own objection, so
what remains is purely a third-party-IP question, and the repository's contents
make that question narrower than it looks: `external/iplug2` and
`external/vst3sdk` are **git submodules** — commit pointers into Steinberg's and
iPlug2's own public repositories. A public `nassau-plugin-sdk` would therefore
redistribute no third-party source at all; it would publish ~350 lines of our own
CMake plus two upstream URLs and commit ids.

Two genuinely separate obligations should be checked before flipping the switch,
and neither is decided here:

1. **The Skia release asset** (`fetch-skia.sh`, tag `skia-mac-arm64-v1`) *is*
   redistribution — prebuilt Skia static libraries and headers hosted by us.
   Skia is BSD-3-Clause, so this is ordinarily fine *with the licence text
   carried alongside*, which the asset should be checked for.
2. **Shipping VST3 binaries** carries Steinberg's own licensing terms
   (GPLv3-or-their-agreement). That obligation attaches to the *plugins we
   ship*, not to this repository's visibility — it exists today, unchanged, and
   belongs in M6's packaging review rather than here.

Until that review happens, M3 must not depend on the answer: it uses a
read-only deploy key or a GitHub App token, which works either way (M3).

## 3. Milestones

Each is a separate PR into its own repo, in this order. M0-M2 are worth doing
even if the suite were abandoned.

### M0 — Lift the shared prologue into the SDK

**Where:** `nassau-plugin-sdk/cmake/NassauPluginProject.cmake` (new).

Move out of all three plugin top-levels: the MSVC `/MT` runtime selection and
its LNK2038 reasoning, `add_compile_definitions(_CRT_SECURE_NO_WARNINGS)`, the
`NASSAU_WARNING_FLAGS` / `NASSAU_RELEASE_OPT_FLAGS` pair, the
`enable_language(OBJC/OBJCXX)` + `CMAKE_OSX_DEPLOYMENT_TARGET` block, and the
"skip plugin targets off macOS/Windows" guard.

Keep in each plugin: its own `project()`, its own DSP options
(`NASSAU_DSP_FLOAT`, `NASSAU_NO_SIMD` — `nassau-analogue/CMakeLists.txt:65-90`
is genuinely analogue-specific), its own `add_subdirectory` list.

Constraint to respect: `CMAKE_MSVC_RUNTIME_LIBRARY` must be set **before the
first `add_subdirectory`** — the comment at `nassau-eq/CMakeLists.txt:26-34`
explains why (it seeds each target's property at creation time, and
`Source/DSP` is added before the SDK include). So the new module is included at
the very top, before `project()`'s subdirectories, which is a different
inclusion point from `NassauPlugin.cmake`'s. That is the one subtlety in M0.

**Gate:** each of the three plugins configures and `ctest`s clean, on macOS and
on a non-Apple host (the DSP-core-only path), before and after, with identical
test output. `nassau-analogue`'s golden batteries must still verify at exactly
`0.000e+00`.

**Expected result:** each plugin top-level drops from 117/75/75 lines to ~20.

### M1 — One SDK resolution order

**Where:** `nassau-plugin-sdk/cmake/NassauSDK.cmake` (new, or folded into M0's
module); all three plugin top-levels; `nassau-eq/.gitmodules`.

Implement D2's order. Delete `nassau-eq`'s `sdk/` submodule (D3). Give the SDK
its first real semver tag and state a compatibility policy in its README — it
is the one component with a consumer contract, so it is the one that needs a
promise.

**Gate:** all three plugins configure from (a) a bare clone with the SDK as a
sibling, (b) a bare clone with *no* sibling and FetchContent allowed, (c) an
explicit `-DNASSAU_SDK_DIR=`, and (d) `-DNASSAU_SDK_DIR=/nonexistent`, which
must still build DSP + tests, as `ci.yml.disabled:18` already relies on.

### M2 — The suite repo and the superbuild

**Where:** `nassau-suite` (new repo).

Submodules for `qbx`, `nassau-plugin-sdk`, `nassau-analogue`, `nassau-eq`,
`nassau-zermatt`. A top-level `CMakeLists.txt` with:

- `ExternalProject_Add(provision ...)` running
  `cmake -P cmake/ProvisionDeps.cmake` and, on macOS, `scripts/fetch-skia.sh`
  — **once**, as the dependency of every plugin target (D4).
- `ExternalProject_Add(smaragd ...)` driving `qbx` with the Qt prefix.
- one `ExternalProject_Add` per plugin, each `DEPENDS provision`, each passed
  `-DNASSAU_SDK_DIR=<the pinned SDK submodule>`.
- an install/staging step collecting `build/out/*.vst3|.component|.clap` and
  `Smaragd.app` into one `stage/` tree, which is what M5 packages.

`ci/build.sh` wraps it so a human types one command.

**Gate:** `ci/build.sh` from a fresh recursive clone on macOS produces
`stage/` containing the DAW plus all three plugins in all their formats; then
the same on Windows (VST3 + CLAP, no AU).

### M3 — Per-repo CI

**Where:** `.github/workflows/ci.yml` + `ci/gates.sh` in each of the six repos.

For `qbx`, `ci/gates.sh` is the CLAUDE.md checklist verbatim: `build.sh`, the
four `tools/check_*.py`, `ctest`. It stops being a thing a human remembers.

For the plugins, revive `ci.yml.disabled` — which is now much simpler, because
after M1 the DSP-and-tests job needs no SDK access at all, and the full plugin
job resolves the SDK from the suite or FetchContent rather than from a private
submodule.

**Fix the token problem properly rather than around it, and do not block on the
visibility question.** The private `nassau-plugin-sdk` is what disabled CI in the
first place, and Q2's answer says the owner has no objection to publishing it —
only the third-party terms might. But that review is a licence task on its own
clock (D10), and CI should not wait for it: M3 provisions a **read-only deploy
key** (or a GitHub App token) scoped to `nassau-plugin-sdk`, never a long-lived
PAT. That works whether the repo stays private or goes public, and if D10 later
clears publication, the key simply becomes unnecessary — a deletion, not a
migration.

**Gate:** a red CI on a deliberately broken PR in each repo; green on `main`.

### M4 — The integration gate

**Where:** `nassau-suite/ci/integration.sh`, plus new `.qxa` cases. **No `qbx`
code change** — see below.

Run the **built** Nassau bundles from M2's `stage/` through the **built** DAW.
This is the coupling that actually matters, and today neither repo can test it
alone: a plugin change that breaks hosting should fail here, not at a customer.

Q3's survey (§8) sized this precisely, and the answer is a good one: **the
harness is done, the coverage is absent.** The sixteen `plugin_*` cases plus
`automation_plugin_param` are a thorough test of the *host*, but every one of
them loads an in-repo fixture — `twtestclap.clap` and `twtestvst3.vst3`, built
from `tw303a/plugins/tests/twtestclap.c` and `twtestvst3.cpp`. No case has ever
loaded a Nassau bundle. So M4 writes cases, not infrastructure:

- Every verb it needs exists: `insert-plugin` already takes
  `format=`/`uid=`/`path=` and resolves `path=` relative to the `.qxa`'s own
  directory; `set-plugin-param`, `set-plugin-bypass`, `reorder-plugin`,
  `assert-plugin-strip`, `assert-instrument-slot`,
  `assert-plugin-editor-kind`, `plugin-native-editor`, `render`,
  `assert-audio-energy`, `assert-channels-differ`, `assert-file-identical`.
- The staging hook exists too: `twpluginsearchpaths.cc:216` honours the
  `CLAP_PATH`/`VST3_PATH` environment lists, so `integration.sh` points them at
  `stage/` and nothing in `qbx` needs touching. This is what removes the one
  non-build code change an earlier draft assumed (D9, §6).
- **The gap worth naming: AU has no `.qxa` coverage at all.** The cases are 19
  CLAP and 1 VST3; AU appears only in the `au_test.cc` unit test. macOS ships
  AU, so M4 should not inherit that hole.

Per plugin, the minimum worth writing: it scans and instantiates in each format
it ships; a parameter set through `set-plugin-param` is *audible* in a render;
bypass is a true bypass; state survives save/reload; and the editor opens and
closes without violating the lifetime contract `main/pluginui/CONTRACT.md`
describes.

**Gate:** the suite build fails when a plugin is deliberately broken in a way
only the host observes — a parameter count mismatch, or Zermatt's latency
report (the `6b4ec98` fix) regressed.

### M5 — Version stamping

**Where:** `nassau-suite` passes `-DNASSAU_SUITE_VERSION` and
`-DNASSAU_SUITE_COMMIT` into every component; `qbx` collapses the duplicated
`1.0.0` literals into the suite-supplied value (D5).

Under D5 this is *displayed* more simply than originally drafted: every About
box leads with the suite version, and the component's own version is a
secondary, diagnostic line — because no user compares plugin versions when no
plugin is sold alone.

**Gate:** every artifact in `stage/` reports the same suite version and commit;
each still carries its own component version where a crash report can find it.

### M6 — Packaging and signing

**Where:** `nassau-suite/packaging/`, `ci/package.sh`.

Detail in §4. Signing identities and notarization credentials live here, once
— which is the other strong argument for the superproject: without it those
secrets get copied into five repos.

Q1's answer shrinks this milestone: **one installer per platform, one changelog,
one signing identity, no per-plugin release notes and no per-plugin `.pkg`s.**
Q5's answer then fixes its shape: the installer must place the bundles in the
system directories, because that is what makes them reachable from another host
(D9). So the package is doing real work, not merely convenience — which is why
its gate below ends at a second DAW rather than at Smaragd.

Also in M6: the **third-party licence review** for the installer — the Skia,
iPlug2, CLAP and VST3 terms that attach to what we *ship* (distinct from D10's
question about what we *publish*), including carrying the licence texts in the
package.

**Gate:** the macOS `.pkg` installs on a clean machine, `spctl -a -vv` passes on
the app and every bundle, the DAW starts and finds all three plugins with no
Gatekeeper prompt, **and a second host — any other DAW — also sees them**. That
last clause is the one that proves Q5's requirement was actually met rather than
assumed. Then the Windows installer's equivalent.

### M7 — Mangrove onto the SDK, and into the suite

**Where:** `nassau-mangrove2`; then one more submodule in `nassau-suite`.

Q4 makes Mangrove product, and the refactor it needs is exactly the one the SDK
exists to perform. Today `nassau-mangrove2/CMakeLists.txt` carries three private
copies of SDK work: its own vendored `external/iplug2` (`:24`), its own
`VST3_SDK_ROOT` (`:17`), and a hand-rolled `Source/VST3` target (`:18`).

Replace all three with `NassauPluginProject.cmake` (M0) + `NassauPlugin.cmake` +
M1's resolution order, exactly as the other three plugins consume them. Then add
it to the pin set, and to M4's integration cases.

Sequenced last on purpose: it is the one migration with real behavioural risk —
`MangroveCompressor` is at VERSION 4.0.0 with a shipping history, and swapping
its plugin-format layer is not a build-only change the way M0 is for the others.
M4 must be able to catch a regression before M7 relies on it.

Separately and cheaply: archive `nassau-mangrove` (JUCE, last commit
2020-12-04), so no future session mistakes it for the live repo (D8).

## 4. Packaging, per platform

Under D9 there is exactly one copy of each bundle, in the location every host
already scans.

- **macOS:** one signed + notarized distribution `.pkg` with component packages
  — `Smaragd.app` → `/Applications`, each bundle →
  `/Library/Audio/Plug-Ins/{VST3,Components,CLAP}`. A `.pkg` and not a
  drag-install `.dmg`, because Q5 requires the plugins to reach other hosts and
  only a `.pkg` can write those directories.
- **Windows:** one Inno Setup or WiX installer; the app, plus VST3 →
  `C:\Program Files\Common Files\VST3` and CLAP → `…\Common Files\CLAP`. The
  plugins already link `/MT` deliberately (`nassau-eq/CMakeLists.txt:32-33`) so
  no VC++ redistributable is needed.
- **Linux:** DAW only — AppImage or `.deb`. The SDK `FATAL_ERROR`s off
  macOS/Windows (`NassauPlugin.cmake:50-54`), so there are no Linux plugin
  builds to ship until someone ports the UI backend.

## 5. Versioning summary

One user-facing number: the suite's. Component semver survives as a developer
and SDK-contract number, stamped into every artifact for forensics but not led
with (D5, M5).

## 6. What this does *not* do

- It does not merge any repositories, and it does not itself change any repo's
  visibility — D10 states the condition for publishing the SDK and explicitly
  leaves the licence review undone.
- **It touches no `qbx` code at all.** M0-M2 are build-system only; M4 reaches
  the staging tree through the `CLAP_PATH`/`VST3_PATH` support already in
  `twpluginsearchpaths.cc:216`, and D9's system directories are already scanned
  at `:207-208`. Only M5's version stamping edits a `CMakeLists.txt`.
- It does not unify the test harnesses. `qbx` uses ctest + qxa; the plugins use
  a hand-rolled harness behind `enable_testing()`. Both are invoked by
  `ci/gates.sh` and that is enough.

## 7. Risks

1. **M0 is a wide, boring, cross-repo refactor of the exact code that is hard
   to test off-platform.** The MSVC `/MT` reasoning cannot be verified on the
   dev box at all. Mitigation: M3 (CI on Windows) ideally lands before M0's
   Windows half, or M0 goes in behind a per-plugin "use the new module" switch
   so it can be reverted one plugin at a time.
2. **`ProvisionDeps.cmake` mutating the SDK checkout** (D4) will bite whoever
   forgets it. Any parallel or cached CI must treat the provisioned SDK tree as
   a build artifact, not a clean checkout.
3. **The Skia asset is a single point of failure** — one release asset, one
   tag, one repo. If it disappears the fallback is an ~8 GB source build
   (`fetch-skia.sh:15-17`). Worth mirroring, independently of D10.
4. **D3 makes `nassau-eq` slightly harder to clone standalone.** Accepted;
   noted here so it is not rediscovered as a bug.
5. **M7 is the one migration with real behavioural risk.**
   `MangroveCompressor` is at VERSION 4.0.0 with a shipping history, and
   swapping its plugin-format layer (its own iPlug2, its own VST3 target) for
   the SDK's is not the build-only change M0 is elsewhere. Mitigation: it is
   sequenced last, behind an M4 that can catch the regression.
6. **A developer's locally built plugin and the installed one now occupy the
   same directories.** With one shipping copy (D9) this is the ordinary audio
   situation rather than a hazard we invented, but `integration.sh` must set
   `CLAP_PATH`/`VST3_PATH` explicitly rather than trusting whatever is
   installed on the runner — otherwise CI silently tests the wrong bundles.

## 8. Questions

### Answered (2026-09-22)

**Q1 — Are plugins sold individually, or only inside Smaragd? → Only inside.**
The suite is the only release artifact. Consequences: D5 (one user-facing
version), D7 (one YouTrack project, now settled rather than merely preferred),
one installer per platform and one changelog (M6). Note that Q1 alone did
*not* settle where the plugins install — Q5 did, in the opposite direction from
the draft Q1 first suggested (see Q5 below).

**Q2 — Must `nassau-plugin-sdk` stay private? → Not for the owner's sake.** The
remaining question is third-party IP (CLAP, VST3 and the rest), which the owner
cannot answer on those parties' behalf. Captured as D10, with the narrowing
observation that `external/iplug2` and `external/vst3sdk` are *submodules*, so a
public SDK repo would redistribute no third-party source — only the hosted Skia
asset would be redistribution, and shipping VST3 binaries is a separate,
pre-existing obligation that belongs to M6. M3 is written not to depend on the
outcome.

**Q3 — How much do the existing `plugin_*` qxa cases already cover? → The
harness is complete; the coverage is zero.** Surveyed rather than asked, since
it was answerable from the tree. There are 16 `plugin_*` cases plus
`automation_plugin_param` among 352 `.qxa` cases, and between them they exercise
chain wiring and stereo bus routing (`plugin_stereo_chain`), bypass and
parameters, insert/remove/reorder with undo and state restoration, the missing-
plugin placeholder, rescan livelock, slot round-trip through the project file,
the FX strip, and both editor kinds including teardown safety.

But **every one of them loads an in-repo fixture** —
`uid="tw.test.clap.{stereoskew,gain,gui,restart}"` from
`tw303a/plugins/tests/twtestclap.c`, and one VST3 from `twtestvst3.cpp`. They
test the host, deliberately and well, against a plugin `qbx` controls. No Nassau
bundle has ever been loaded by a test.

Consequences, both good: M4 writes `.qxa` cases rather than infrastructure, and
needs no `qbx` code change (the `CLAP_PATH`/`VST3_PATH` hook already exists at
`twpluginsearchpaths.cc:216`). One real hole surfaced on the way: the cases are
19 CLAP and 1 VST3, with **AU covered only by a C++ unit test**
(`plugins/tests/au_test.cc`) and never by a case — and AU is a macOS shipping
format. M4 should not inherit that.

**Q4 — `nassau-mangrove` / `-mangrove2`: product or history? → Product, and it
needs refactoring.** Both are true of different repos, which is the useful part:
`nassau-mangrove` is JUCE, 25 commits, last touched **2020-12-04** — history, to
be archived. `nassau-mangrove2` is the product: 62 commits, last touched
2026-08-15, `VERSION 4.0.0`. The refactoring it needs is precisely the SDK's
job, since it currently carries its own vendored iPlug2, its own `VST3_SDK_ROOT`
and a hand-rolled `Source/VST3` target. Captured as D8 and the new **M7**,
sequenced last because it is the only migration with shipping-behaviour risk.

**Q5 — Are the plugins wanted in other hosts? → Yes.** This retires the
bundled-copy design an earlier draft proposed: a copy inside `Smaragd.app`
cannot serve another host, so system-wide installation must exist anyway, and a
second copy would then buy nothing while costing a shadowing hazard. **D9 is
rewritten accordingly** — one copy, in the standard system directories — and
with it §4 returns to a `.pkg` on macOS, §6 records that no `qbx` code is
touched at all, and M6's gate now requires a second host to see the plugins.

## 9. Adversarial review — requested, not yet done

Points to attack before M2 is written:

1. Is `ExternalProject_Add` really necessary, or would `add_subdirectory` work
   if the plugins were configured without `qbx` in the same tree? D4 claims two
   reasons; the provisioning-ordering one looks load-bearing and the Qt one may
   not be.
2. Does the suite superbuild actually need to build `qbx`, or should it consume
   a `qbx` release artifact? The latter is faster and weakens M4.
3. Is one YouTrack project (D7) right, or is the thinktank-continuity argument
   doing more work than it should?
4. M0 and M1 both touch all three plugin top-levels. Should they be one PR per
   repo instead of one PR per milestone?
5. D10 claims a submodule pointer is not redistribution. That is how git works,
   but it is a licence claim dressed as a technical one — it should be checked
   by someone qualified before anything is published, not accepted because it
   sounds right.
6. M4's per-plugin case list ("parameter is audible, bypass is a true bypass,
   state survives reload") was written from the host's side. Attack it from the
   DSP side: each plugin repo already has its own golden-battery tests, and M4
   must not become a worse duplicate of them. What does hosting break that a
   plugin's own tests cannot see?
7. M7 is sequenced last so M4 can catch its regressions — but M4's cases for
   Mangrove can only be written *after* M7 makes it buildable in the suite. Is
   that circular, and if so which half goes first?
