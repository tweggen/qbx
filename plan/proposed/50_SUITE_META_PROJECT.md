# Proposal 50 — One superproject: the suite is the product

> **STATUS: EXECUTED — M0..M7 landed 2026-09-22 .. 2026-10-03.** Every
> milestone carries an "as executed" note below, including where it corrected
> the design or recorded a mistake. **Three gates remain unmet and are not
> engineering** — signing, a clean machine, and Windows; see the status section
> at the end.
>
> The survey below is the ORIGINAL one and is kept as written: it is what the
> plan was reasoned from. Its file:line references point at the trees named
> here, not at today's `main`.
>
> Surveyed against `main` at `0305a735` (2026-09-22) and
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
>
> **M0 + M1 ARE EXECUTED (2026-10-03), as four PRs awaiting the author's
> merge.** They went in as one PR per repo rather than one per milestone --
> §9 point 4 asked the question and the answer is that M0 and M1 edit the same
> lines, so splitting them per milestone would have made each repo conflict
> with itself. Merge order is SDK first; the plugin branches degrade to a
> DSP-only build without it.
>
> | repo | PR | top-level CMakeLists |
> |---|---|---|
> | nassau-plugin-sdk | [#2](https://github.com/tweggen/nassau-plugin-sdk/pull/2) | +2 modules |
> | nassau-zermatt | [#2](https://github.com/tweggen/nassau-zermatt/pull/2) | 75 -> 40 |
> | nassau-analogue | [#5](https://github.com/tweggen/nassau-analogue/pull/5) | 117 -> 85 |
> | nassau-eq | [#2](https://github.com/tweggen/nassau-eq/pull/2) | 75 -> 45, `sdk/` dropped |
>
> Gated on macOS 26.5.1/arm64 against `nassau-plugin-sdk@db15697`, each repo
> compared with a baseline build of its own default branch in the same tree:
> build-step counts identical (110/150/104), ctest 12/12, 12/12, 5/5, and
> analogue's goldens unchanged to every digit (G5 `1.863e-09`, G11
> `2.274e-13`). The locator's resolution order was gated in a probe project
> across six cases. **Not gated: Windows and Linux** (no hosts), and the
> `FetchContent` branch against the real remote (private repo, needs
> credentials).
>
> **Three corrections this execution forces on the text below:**
>
> 1. **M0's gate in §3 claimed analogue's goldens "must still verify at exactly
>    `0.000e+00`".** They do not and never did: the fixtures verify at the small
>    non-zero errors above. That phrasing came from `nassau-analogue`'s own
>    comment, which describes the *scalar-vs-SIMD* comparison, not
>    golden-vs-fixture. The invariant actually worth gating -- and gated -- is
>    that the numbers are *unchanged*. (A side result: with `NASSAU_NO_SIMD=ON`
>    the goldens are bit-identical to the SIMD path, which is what DESIGN.md
>    §12.3 claims and nothing had demonstrated.)
> 2. **M0's premise that the three copies were identical was wrong.** They had
>    drifted: analogue and zermatt passed `/Oi` for MSVC release, **eq passed
>    nothing**. The module unifies on `/Oi`, which is the one behavioural change
>    in the series and is Windows-only, hence unverified.
> 3. **The locator cannot live in the SDK.** M0/M1 as written implied all the
>    shared CMake moves into `nassau-plugin-sdk`; the *locating* step cannot,
>    because a repo cannot `include()` from a directory it has not found yet. So
>    the split shipped is: prologue in the SDK
>    (`cmake/NassauPluginProject.cmake`, the part whose reasoning changes),
>    locator copied verbatim per repo (`cmake/NassauSDK.cmake`, pure location
>    logic, expected never to change).
>
> **M2 IS EXECUTED (2026-10-03), pushed to `nassau-suite@7dc5130`.** The repo
> holds the five pins (qbx `cd4485fe`, sdk `2cfea22`, analogue `f83ec84`, eq
> `0203835`, zermatt `f6d7258`), the `ExternalProject` superbuild,
> `cmake/StageBundles.cmake` and `ci/build.sh`. **Gate met as specified:** a
> genuinely fresh `git clone` of the remote, then `./ci/build.sh`, produces
> `build/stage/` with `smaragd.app` and all ten plugin bundles in **5m05s** on
> macOS 26.5.1/arm64 — submodule init, provisioning, Skia fetch, three plugins
> and the whole DAW. All three plugins link Skia (49 symbols each, so real UI
> builds, not headless); `smaragd.app` carries 8 Qt frameworks via macdeployqt.
>
> **Three bugs the gate caught, each of which would have shipped silently:**
>
> 1. **Smaragd's `bin/` holds the qxa plugin test fixtures** —
>    `twtestclap.clap`, `twtestvst3.vst3`, `twtestvst3bundle.vst3` — right next
>    to the app, and the first staging pass shipped all three to the installer.
>    Staging patterns are now stated per component; Smaragd contributes `*.app`
>    and nothing else. §4 should note this: the DAW's build directory is not a
>    safe thing to glob.
> 2. **iPlug2 defaults `IPLUG_DEPLOY_PLUGINS` to ON**, copying every built
>    bundle into the *building user's own* `~/Library/Audio/Plug-Ins/`. Right
>    for a plugin developer, wrong for a release build, which must produce an
>    artefact and touch nothing else. The superbuild forces it OFF, proved by
>    rebuilding eq inside the suite and confirming the `~/Library` copy's mtime
>    did not move.
> 3. CMake splits a `;` inside a `COMMAND` argument, so a semicolon-separated
>    pattern list arrived as separate arguments and the staging script silently
>    saw only the first — staging `.vst3` and dropping `.component`, `.clap`
>    and `.app`. Comma-separated now.
>
> **Two decisions M2 made that the text above did not anticipate:**
>
> - **Smaragd is driven through its own CMake, not `qbx/build.sh`.** build.sh
>   owns a build tree inside the checkout and a superbuild wants its own
>   out-of-tree binary dir. The flags it passes (`CMAKE_PREFIX_PATH`,
>   `AUTO_DEPLOY_QT=ON`) are exactly `rebuild.sh`'s.
> - **`ci/build.sh` resolves Qt by sourcing `qbx/_env.sh`** rather than
>   reimplementing detection, so the product has one such implementation
>   instead of two that can disagree. Note `resolve_qt_path` needs
>   `detect_platform` called first, or it silently finds nothing.
>
> **M4 IS EXECUTED (2026-10-03), `nassau-suite@302bd80`.** Six `.qxa` cases in
> `integration/cases/` plus `ci/integration.sh`. **M4's prediction held: no
> `qbx` code change was needed** — `CLAP_PATH`/`VST3_PATH` reach the staging
> tree, and the cases insert by uid with no `path=`, so resolution goes through
> the registry and what is gated is real discovery. 6/6 pass, twice from a cold
> config dir. Assertion bands are measured, not guessed (dry 0.06665; EQ at
> defaults 0.06665 — transparent to five decimals; EQ +24 dB 0.08782; Zermatt
> default 0.22616; Zermatt hot 0.33728; Analogue one C4 0.12847).
>
> **The sabotage pass answers §9 point 6 — "what does hosting break that a
> plugin's own tests cannot see?" — with a measurement rather than an
> argument.** EQ's `Band 1 Gain` default was changed 0 → 6 dB, so the EQ is no
> longer transparent when inserted. **`nassau-eq`'s own suite: 6/6 green. This
> gate: the two cases asserting transparency fail.** Two cheaper sabotages also
> bite correctly: deleting the staged `NassauEQ.clap` fails exactly the three
> cases that use it, and pointing the search paths at an empty directory fails
> discovery.
>
> **Three runner requirements the plan did not anticipate, all found by getting
> them wrong first:**
>
> 1. **The gate was not hermetic.** Smaragd caches its plugin scan in the
>    user's config dir (`~/.config/Smaragd/plugincache.v2.json`), so the gate
>    both read a developer's cached state and wrote to it. Qt honours
>    `XDG_CONFIG_HOME` for `IniFormat`/`UserScope`, so the runner redirects it
>    into the run directory — hermetic, and always cold.
> 2. **The scan is asynchronous** (`twPluginRegistry::rescanAsync`), so from an
>    empty cache the first process to ask for a plugin races it. Observed, not
>    theorised: after a rebuild the first cases failed at discovery while an
>    identical second run passed everything. `integration/warmup.qxa` now runs
>    until it passes and its result is never counted — and from cold it
>    converges after **four** attempts, reproducibly, which says each process
>    persists part of what it discovered rather than one completing the scan.
>    **That looks like a qbx improvement worth its own ticket:** there is no
>    verb to force a synchronous scan, and `wait-ms` is the only tool the
>    existing cases have.
> 3. **Search paths must be set explicitly**, not merged with the machine's, or
>    the gate can pass against a developer's older build in
>    `~/Library/Audio/Plug-Ins` while the staged bundle is broken.
>
> **AU remains uncovered, and §8 Q3's worry about it is now sharper:** it is not
> an oversight that can be fixed in M4. `twpluginsearchpaths.cc` returns early
> for `au` because AU is enumerated from the OS component registry, and the code
> deliberately keeps it out of the folder/env-var machinery — so there is no
> `AU_PATH` to redirect. Testing AU means really installing a `.component` and
> letting macOS register it, which a build gate must not do to its own machine.
> **AU verification therefore belongs to M6**, whose gate already installs the
> package and then checks a second host sees the plugins.
>
> Two authoring errors the gate caught in its own cases, fixed and documented in
> place: an `undo count="2"` where the bypass toggles made it 3, and an
> `assert-file-identical` across two renders in one process — which qbx's own
> `CMakeLists.txt` says is a **fresh-process** property (design F4) that no
> single case can assert. §9 point 7's circularity worry about M7 is unaffected:
> the Mangrove cases still have to come after its migration.
>
> **M7 IS EXECUTED (2026-10-03)** — `nassau-mangrove2#7` and `nassau-suite#1`,
> both awaiting merge. All four plugins are now on the SDK. The suite stages 13
> plugin bundles + `smaragd.app`, and the gate is 7/7.
>
> **D8 and M7 both understated the job.** The plan said mangrove2 carried three
> private copies of SDK work; it carried those *and* a far messier repo. What
> was removed: `external/iplug2` + `external/vst3sdk` (**9.0 GB**), a 212-line
> hand-rolled `Source/Plugin/CMakeLists.txt` (→ 30), and `Source/VST3/`, an
> older *second* plugin with its own duplicate `MangrovePlugin.cpp` and
> `config.h`. **Gained:** CLAP, and a working AU — the AU target had been
> sitting behind `if(FALSE)`.
>
> **The risk the plan feared was mostly absent, for a reason worth recording:**
> mangrove2 pinned the *identical* upstream commits as the SDK (iplug2
> `7dfe7a96d`, vst3sdk `58f8da7`), so no third-party code changed in the move.
> Only two source changes were needed, both demanded by the CLAP target alone:
> three missing `PLUG_*_STR` macros, and `: Plugin(...)` → `: iplug::Plugin(...)`
> (the CLAP-helpers headers define a competing `Plugin` template — the same
> one-line fix nassau-eq and nassau-zermatt already carry).
>
> **§9 point 7's circularity is resolved by baselining, and it worked:** the
> pre-migration VST3 was rendered through the suite's Smaragd at RMS 0.06022
> before anything was touched; the migrated VST3 and the new CLAP both reproduce
> it to five decimals, with the VST3 uid unchanged.
>
> **A mistake worth keeping in the record.** Mid-migration I measured a dry
> render from the migrated VST3 and concluded `PLUG_CHANNEL_IO "2-2"` had broken
> it; I changed the IO config and guarded `ProcessBlock` on that basis. It was
> wrong — the dry render came from a run that had **failed on plugin-registry
> readiness**, so no plugin was in the chain and the output was the untouched
> input. Reverted in a second commit rather than rewritten away. **The general
> hazard this exposes belongs in §7:** a failed plugin case leaves a
> *plausible-looking* render behind, so an artifact read without its verdict can
> manufacture a regression that does not exist. M4's runner is built against
> this; ad-hoc measurement is not.
>
> Still a good idea and deliberately not done: declaring `"1-1 2-2"` like the
> other three plugins would let Mangrove load in mono hosts — but it *requires*
> the `ProcessBlock` guard, since it reads `inputs[1]`/writes `outputs[1]`
> unconditionally. Both together, or neither.
>
> **Out of M7's scope and left alone, for the author:** mangrove2 also tracks
> `MangrovePlugin/` (127 files), `MangroveIPlug/` (132) and `Source_Original/` —
> parallel copies of the plugin, three with their own `MangrovePlugin.cpp`
> and/or `config.h` — plus `build_phase5/` and `build_vst3/`, **618 tracked
> files of committed build output**. Sorting product from history there is a
> judgement call, not a build migration. `BUILD.md` and `CLAUDE.md` now state
> that `Source/Plugin` is the one that builds and ships, and four stale build
> docs carry a SUPERSEDED banner.
>
> `nassau-mangrove` is **not** archived — an administrative change to a public
> repo, left for the author; one command.
>
> **M3 IS EXECUTED (2026-10-03)** — seven PRs, all awaiting merge. Every repo in
> the product now has CI where none had any: `qbx#216`,
> `nassau-plugin-sdk#3`, `nassau-analogue#6`, `nassau-eq#3`,
> `nassau-zermatt#3`, `nassau-suite#2`, and mangrove2's rides on its migration
> PR (`nassau-mangrove2#7`). D6 held throughout: every workflow calls a
> `ci/gates.sh` and does nothing else of substance.
>
> **The credential problem turned out far smaller than M3 assumed**, and this is
> the finding that matters most. M3 expected to need a cross-repo token
> everywhere. In fact:
>
> | repo | credential needed |
> |---|---|
> | `qbx` | **none** |
> | `nassau-plugin-sdk` | **none** — its consumer, `nassau-analogue`, is public |
> | the four plugins | one **read-only deploy key**, and only for the optional macOS job |
> | `nassau-suite` | a cross-repo token — unavoidable, see below |
>
> Two design choices bought that. First, **the plugin job builds headless**:
> headless needs the SDK (private, hence a deploy key) but *not* the prebuilt
> Skia release asset, which is the only thing that would require an API token —
> deploy keys authenticate git, not REST. Second, the **DSP-only job** uses
> `NASSAU_SDK_DIR=/nonexistent`, so it needs no SDK, no submodules, no network
> and no macOS, and is green on a fork.
>
> Only the suite can't be arranged that way: it checks out four private
> submodules *and* needs the Skia asset (its gate opens real plugin editors, so
> headless is not an option there). One GitHub App token or fine-grained PAT as
> `NASSAU_CI_TOKEN`. Its workflow is therefore **the one thing in M3 that has
> never run** — everything it calls is gated locally, but the plumbing is
> unverified until a secret exists.
>
> **qbx's first CI run did what CI was added for: it measured Linux, which this
> repo had never had a baseline for.** Build succeeded first try; **398/400
> passed in 367 s**. The two failures are now `docs/LINUX_GATE.md`, the sibling
> of `MACOS_GATE.md` and `ASIO_WINDOWS_GATE.md` — and the only one of the three
> built by measurement rather than by hand:
>
> 1. `secret_store_test` — **not a code defect.** It picks the libsecret backend
>    and a runner has no D-Bus session or keyring. Its own output proves
>    `SMARAGD_SECRET_BACKEND` is honoured, so CI sets `memory` and the test still
>    *runs*. Honest cost: the libsecret backend is uncovered on Linux.
> 2. `qxa.asset_clip_preview` — **SEGFAULT, excluded, a real open question.**
>    Offscreen platform? A Linux-only bug? A `-j4` interaction? Nothing in the
>    run distinguishes them; it needs a Linux box with a debugger. **Worth a
>    ticket.**
>
> `ci/gates.sh` gained `CTEST_EXCLUDE`, empty by default so a local run excludes
> nothing — an exclusion is a statement about CI's environment, not about the
> test — and an exclusion without an entry in a platform gate doc is a bug.
>
> **With both entries applied, qbx CI is GREEN: `100% tests passed, 0 tests
> failed out of 398`, in 367 s** (run 37135875524). So from here a red
> `build-and-test` means new breakage, which is the whole point — and it took
> three iterations to get there, each one a real finding rather than a retry.
>
> **Also measured:** the plugin DSP job runs in ~41 s on Linux; macOS jobs skip
> in ~2 s when their secret is absent, green rather than red. And the SDK, which
> had **no tests at all**, now has six: the locator's resolution order, which is
> the one file in the product copied verbatim into four repos.
>
> **Two bugs CI found in my own work, both of the same kind — things that pass
> on the machine that wrote them:** the SDK probe relied on an empty
> `ci/probe/cmake/` directory, which git does not track, so it worked locally
> and failed on every fresh checkout (now re-verified against a `git archive`
> tree rather than a working copy); and `NASSAU_FORCE_HEADLESS` was first passed
> as an environment variable, which CMake ignores for a plain `option()`.
>
> **One unresolved infrastructure quirk:** GitHub has registered no workflow for
> `nassau-mangrove2` (`actions/workflows` returns 0) though the file is on the
> PR branch and Actions is enabled, where it registered immediately for the
> other five. Expect it to appear when the file reaches `main`.
>
> **M5 IS EXECUTED (2026-10-03)** — three PRs: `nassau-plugin-sdk#4`,
> `qbx#217`, `nassau-suite#3`. **The four plugin repos needed no change at
> all**, because `nassau_add_plugin()` generates the stamp and adds it to every
> format target — the same "one place" reasoning that put the toolchain prologue
> in the SDK in M0.
>
> M5 as drafted said "each component surfaces both versions in its About box".
> What shipped is **better suited to the actual question**, which is *"which
> build is this?"* asked of an artifact that arrived without a build tree:
>
> | surface | what it answers |
> |---|---|
> | an embedded `NASSAU_STAMP` line in every binary | `strings` works on a bundle emailed to you — no host, no GUI, no build tree |
> | `smaragd --version` | machine-readable, and what `ci/stamp-check.sh` reads |
> | the macOS bundle keys | what Finder and the host show |
>
> One line, one format, every artifact: `NASSAU_STAMP suite=<v> commit=<sha>
> component=<name>/<v>`, carrying both versions per D5. A build that did not come
> through the suite stamps `dev`/`unknown` rather than inventing a release.
>
> **D5's "two literals for one number" is closed**: the macOS bundle keys now
> derive from `project()`, values unchanged (1.0.0 / 1.0), verified in the built
> `Info.plist`.
>
> **The gate is `ci/stamp-check.sh`, and all three of its checks are verified
> rather than assumed:** against the pre-stamp stage all 14 artifacts report
> `NO STAMP` and it exits non-zero; with the component branches built, four
> artifacts agree on `suite=0.1.0 commit=302bd80` each with its own component
> version; and rebuilding one component with a different suite version makes it
> name both builds and exit 1. That third case is the one that matters — it
> catches a stage assembled from two builds.
>
> Dead-stripping was the failure mode that would have made this silently
> useless, so the stamp uses external linkage + `__attribute__((used))` and was
> confirmed with `strings` on real Release bundles.
>
> **Two build-system facts this surfaced, both of which cost me a wrong
> conclusion first:** `ci/build.sh` runs `git submodule update --init
> --recursive`, so it **resets a submodule checked out to a branch back to its
> pin** — correct behaviour (the suite enforcing its pins), but it means
> component changes cannot be tested *through* `ci/build.sh`; and a cached
> `ExternalProject` sub-build does not necessarily reconfigure when the SDK's
> CMake changes, so its build directory has to be wiped. **§7 should carry the
> second one as a release-correctness risk**, not just a testing nuisance.
>
> Still outstanding for M5: a one-line step added to the suite's workflow once
> `nassau-suite#2` and `#3` both land, and advancing the suite pins to the
> components' merge commits.
>
> **M6 IS EXECUTED (2026-10-03)** — `nassau-suite#6` and `nassau-mangrove2#8`.
> `ci/package.sh` builds a macOS distribution `.pkg` (171 MB, five components);
> `ci/package-check.sh` verifies the payload without installing.
>
> **The `NassauAnalogue.app` question is answered by the requester:** it ships
> as a **deselectable component, off by default** — offered, not imposed. The
> table above can be struck.
>
> **AU IS VERIFIED, which §8 Q3 and M4 both said was structurally impossible
> from a build gate.** The way through was not a new verb but the installer:
> declaring `enable_currentUserHome` lets the package install into
> `~/Library/Audio/Plug-Ins/Components` **with no root**, and macOS registers
> AUs from there. `auval` — Apple's own tool, a genuine second host — reports
> **AU VALIDATION SUCCEEDED** for NassauEQ, NassauZermatt and NassauAnalogue.
> That turns M6's AU gate from "needs a machine someone will modify" into a
> routine check, and it is the single most reusable thing M6 produced.
>
> **Three bugs, every one found by *installing* rather than building** — which
> is the argument for M6 having a gate at all:
>
> 1. **Bundle relocation.** `pkgbuild` defaults `BundleIsRelocatable` to *true*,
>    so the installer hunts for an existing copy of each bundle identifier and
>    installs **where it already lives**. The app component wrote a receipt
>    recording `InstallPrefixPath=Applications` and delivered `smaragd.app` to
>    *neither* `~/Applications` nor `/Applications`. On a customer's machine
>    that means overwriting an old install wherever it happens to sit. Fixed by
>    analyzing each component and forcing every flag false. **Symptom worth
>    memorising: a receipt exists but the payload is nowhere.**
> 2. **`--` inside an XML comment** is illegal and `productbuild` rejects the
>    distribution file. The same rule bit M4's `.qxa` cases; second time.
> 3. **Mangrove's Audio Unit, three defects deep** — see below.
>
> **Mangrove's AU is withdrawn, and the chain is worth recording.** An
> unexpanded `$(EXECUTABLE_NAME)` (mine, from M7 — I copied `Info.plist.au`
> without reading it, and CMake does not substitute Xcode variables) meant
> macOS could not load the bundle at all. Beneath that, `factoryFunction`
> named `IPlugAUEntry` where the binary exports `MangrovePluginAUFactory`:
> registers, then `Cannot open component: -1`. Beneath *that*, its **editor
> crashes the host** — `auval` segfaults reproducibly at "VERIFYING CUSTOM UI"
> where the other three pass that stage. Not a regression: its AU target had
> sat behind `if(FALSE)` and had never been built, so the first time the format
> existed was the first time the defect could be seen. **AU out of its FORMATS
> until `auval -v aufx Mng5 Nss2` exits 0.**
>
> **M6's gate is NOT met, and this is the milestone's honest status.** The plan
> requires "installs on a clean machine, `spctl -a -vv` passes, no Gatekeeper
> prompt, and a second host sees them". What was reached: the layout, a real
> (user-domain) install, the stamp arriving through the installer
> (`smaragd 0.1.0 (smaragd 1.0.0, suite 2f1ca98)`), and the AU half of the
> second-host clause. What was not: **signing, notarization, Gatekeeper, a
> clean machine, and Windows.** Signing is written as code that runs the moment
> `CODESIGN_IDENTITY`/`PKGSIGN_IDENTITY`/`NOTARY_PROFILE` exist — by the
> requester's decision to defer credentials — and **an unsigned package is not
> shippable.** `packaging/windows/smaragd.iss` mirrors every decision but has
> never been compiled.
>
> **The licence review (§M6) is still open.** `collect-licences.sh` assembles
> the texts from the pinned trees and reports gaps as `*** MISSING` rather than
> omitting them; Skia arrives as a prebuilt binary and carries no text, and Qt's
> LGPL relinking obligation is named but unaddressed. The script says in its own
> output that it is not the review.
>
> ---
>
> **WHAT IS STILL OUTSTANDING.** M0–M7 are done. What stands between this plan
> and a release is not engineering any more:
>
> | outstanding | needs |
> |---|---|
> | M6's signing gate | a Developer ID + notarization credentials |
> | M6's clean-machine gate | a second machine |
> | M6's Windows half | a Windows host |
> | the licence review | someone qualified (and D10's SDK-visibility question) |
> | `qxa.asset_clip_preview` SEGFAULT | a Linux box with a debugger (`docs/LINUX_GATE.md`) |
> | plugin-registry readiness | a qbx fix; the async scan needs 4–18 process launches to converge |
> | Mangrove's AU editor crash | investigation (`nassau-mangrove2`) |
> | `nassau-mangrove2` CI | GitHub registers no workflow there; a repo setting to check |
> | `CMAKE_OSX_DEPLOYMENT_TARGET` | a one-line fix; see the note just below |
>
> **A shipping bug found on the way, deliberately NOT fixed in a no-op
> refactor:** `CMAKE_OSX_DEPLOYMENT_TARGET "10.13"` has never taken effect in
> any plugin repo. `project()` initialises that cache entry to empty first, and
> a non-`FORCE` `set(CACHE)` cannot overwrite it. Measured on a built bundle:
> `minos 26.0` on a macOS 26.5.1 host. The shipped plugins therefore refuse to
> load on anything older than the build machine -- which matters directly to
> D9/M6. Needs its own one-line PR.

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
