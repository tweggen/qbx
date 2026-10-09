# Proposal 51 — a user-visible product name that can be changed later

**Ticket:** QBX-137. **Status:** proposed. **Written:** 2026-10-09.
**Revised** 2026-10-09 after review; see §9 for what the review changed.

Source locations below are given by **function name or symbol**, never by line
number: this project has already had a document (`docs/SIGNAL_CHAIN.md`) carry
nine line-numbered rows of which every one had rotted.

The ticket's request is not "rename the product". It is: *define the product
name in a way that it can later be modified*, while repo names and filesystem
paths stay as they are. This plan is about the indirection. The first value put
through it happens to be `volume`, but the plan is written so that the second
and third cost nothing.

## 1. The names, and who owns which

Recorded from the author, 2026-10-09:

| | Name | Role |
|---|---|---|
| Publisher | **Nassau** (Nassau Records) | the company that releases the product |
| Suite | **volume** | the product — the only release artifact (D5) |
| DAW | **volume** | the application inside the suite; shares the suite's name |
| Plug-ins | NassauEQ, NassauAnalogue, NassauZermatt, Mangrove | publisher-branded; **see §7** |
| Repositories | `qbx`, `nassau-suite`, `nassau-eq`, … | unchanged, deliberately |
| Paths, targets | `smaragd/`, `smaragd.exe`, `tw303a` | unchanged, deliberately |

So the DAW `volume` ships as part of the `volume` suite, published by Nassau —
the Ableton Live shape rather than the Logic Pro shape. `Smaragd` becomes what
it always was in substance: an internal code name. The value of this plan is
that it stops being a *user-visible* one.

## 2. What is in the code today

### The count, with the command that produces it

```bash
grep -rn '"[^"]*Smaragd[^"]*"' --include=*.cpp --include=*.cc --include=*.h \
     --include=*.mm --include=*.c smaragd/main smaragd/tw303a | grep -v '/tests/'
```

**41** string literals at `origin/main` on 2026-10-09 — **20 in
`smaragd/main`** (the app) and **21 in `smaragd/tw303a`** (the engine). The
split is the single most important number in this document; see §4.

The engine's 21, by file:

| File | n | What |
|---|---|---|
| `tw303a/devices/src/coremidi_midi.cc` | 6 | MIDI client and port names |
| `tw303a/devices/src/alsa_seq_midi.cc` | 6 | MIDI client and port names |
| `tw303a/plugins/src/twpluginregistry.cc` | 3 | built-in plug-in vendor and name |
| `tw303a/plugins/tools/vst3_probe.cc` | 2 | probe-host window class |
| `tw303a/plugins/src/twclapplugin.cc` | 2 | CLAP host identity |
| `tw303a/plugins/src/twvst3plugin.cc` | 1 | VST3 host identity |
| `tw303a/plugins/src/twvst3host.cc` | 1 | VST3 host identity |

**nassau-suite:** name sites in 23 files, but the large counts
(`CMakeLists.txt` 28, `ci/build.sh` 10) are almost entirely the *submodule
path* and *target name*, which this plan does not touch. The genuine display
sites are `packaging/macos/distribution.xml.in`, `packaging/windows/smaragd.iss`,
`ci/package.sh` (15), `ci/package-windows.sh` (10),
`packaging/macos/collect-licences.sh`, `README.md` and `docs/RELEASING.md`.

### The finding that shaped the plan

Those literals are **two different kinds of thing wearing the same string**,
and the ticket's "paths may remain" does not cover the dangerous kind.

### Class A — identity keys. These are ADDRESSES OF THE USER'S DATA.

| Where | Value | What a change costs |
|---|---|---|
| `SSettings::SSettings()` **and** `SMediaAccountManager::SMediaAccountManager()` | `QSettings(IniFormat, UserScope, "Smaragd", "smaragd")` | every setting, window layout and recent-project entry. Written twice **deliberately** — `main/shell/CONTRACT.md` invariant 41 requires the media manager to point its own `QSettings` at the same INI — so the two must change together or not at all |
| `SApplication::SApplication()` | `setOrganizationName("Smaragd")`, `setApplicationName("smaragd")` | **not** the INI, which passes its own tuple. Qt derives `QStandardPaths::CacheLocation` from these, and that is the sidecar store root (same function's `SMARAGD_SIDECAR_DIR` block); `AppConfigLocation` is `SMediaCache`'s standalone default root. `--version` also prints `applicationName()` as its first word |
| `schema()` in `ssecretstore_linux.cpp` | `com.smaragd.SecretStore` | every stored password **on Linux**: libsecret writes it as `xdg:schema` and matches on it in `secret_password_lookup_sync` |
| `SMediaAccountManager::SMediaAccountManager()`, passing it explicitly | `com.smaragd.media` | the same secrets **on both** Linux (a `"service"` schema attribute, matched on lookup) and macOS (`kSecAttrService` in `baseQuery`). Note the live value is at this call site; the identical default argument in `SSecretStore::SSecretStore()` is **never used by the product** |
| `main/CMakeLists.txt` — `MACOSX_BUNDLE_GUI_IDENTIFIER` | `dev.tweggen.smaragd` | macOS **microphone consent**: TCC keys consent on the bundle identifier, so a DAW that changes it loses audio input until the user re-grants it |
| `distribution.xml.in` **and** `ci/package.sh` (`add_component`, `pkgbuild --identifier`) | `dev.tweggen.smaragd.app` / `.vst3` / `.au` | upgrade and uninstall detection by the macOS installer. **Two files**, independently spelled |
| `coremidi_midi.cc`, `alsa_seq_midi.cc` | `"Smaragd"`, `"Smaragd Out"`, `"Smaragd In"` | these are the virtual MIDI client and ports **other applications see and save in their own routing**. And qbx stores the portable port *name* in the project (`STrack::midiOutPort_`, mapped to a machine-local id by `SSettings::midiPortId()`), so a renamed port can also orphan a saved `.qxp`'s routing |
| the bundle directory `smaragd.app` | from the CMake target name | `ci/package.sh` installs it to `/Applications` with relocation forced off, under the pkg-ref id above. Renaming the directory would leave the old copy behind **under the same id**; the plan freezes it — see the §8 consequence |

Three things are already right and are left alone: the Inno Setup `AppId` is a
GUID, so a Windows rename keeps its upgrade path; `.qxp` resolves plug-ins by
`uid`, never by name (`SProjectLoader`, attribute `"uid"`); and the Windows
DPAPI path is keyed by nothing nameable — `dpapiEncrypt`'s `L"Smaragd secret"`
is a description, not part of the key derivation, and the ciphertext lives in
the INI, so DPAPI is keyed only transitively by the QSettings tuple.

### Class B — display strings. Freely changeable; what the plan makes variable.

`SMainWindow::updateWindowTitle()`, `QMessageBox` captions,
`SOptionsDialog`'s cache note, `MACOSX_BUNDLE_BUNDLE_NAME`, the Inno `AppName`
/ `DefaultDirName` / `DefaultGroupName`, the `distribution.xml` titles and
descriptions, the licence document's heading, `nassau-suite/README.md`,
`docs/RELEASING.md`, and the `"Smaragd starting"` / `"Smaragd exiting"` log
lines.

### Class C — code, target and path names. Out of scope, per the ticket.

Including the 18 `SMARAGD_*` environment variables, which the ticket's "paths
may remain" covers.

### A defect found on the way, which argues the ticket's case

`SMainWindow::fileSaveAs()` and `SMainWindow::fileOpen()` show the user
`"qbx Projects (*.qxp)"`, and default to `~/Documents/smaragd`. So the product
presents **three** names today — *Smaragd* in titles, *qbx* in file dialogs,
*smaragd* in the executable, the INI and the default project directory.
Whatever is decided about `volume`, that is a bug now.

## 3. The rule this plan exists to write down

> **An identity key is frozen at its current value, permanently, whatever the
> product is called.**

The Class A values are not names. They are where the user's settings, window
layout, saved passwords, MIDI routing and installed bundle live. Renaming one
is a data migration with a consent dialog, not a rename — and a *silent* one,
because the application would start cleanly against an empty new location and
look merely forgetful.

Each one gets a marker comment at its site (§5) saying it reads wrong on
purpose. This is the part most likely to be undone by a future session tidying
up, which is why it is a rule and not a remark.

**The trap, stated precisely.** `SApplication::SApplication()` calls
`setOrganizationName("Smaragd")`. That is wrong twice over — the organisation
is Nassau, and the product is no longer Smaragd either. It must still not be
touched. The reason is **not** the INI: both `QSettings` instances pass their
own `("Smaragd","smaragd")` tuple, and no default-constructed `QSettings`
exists in non-test code. The reason is that Qt derives `QStandardPaths`
locations from the application and organisation names, and two of those are
load-bearing here — the sidecar store root and `SMediaCache`'s standalone
default root. A reviewer who "fixes" that line moves a cache the user's
projects point into.

That distinction is itself the argument for this section: the *obvious* reason
to freeze the setters was wrong, and the real one is two indirections away.
Anyone sweeping these literals on intuition will get it wrong in the same
direction.

The versioning rule applies by analogy: the suite version makes a promise about
the user's saved work, and so does a rename. A rename that loses settings is a
breaking change dressed as a cosmetic one.

## 4. Where the name lives

**Reuse the version pipeline. Do not build a second mechanism.**

`VERSION` at the suite root → `NASSAU_SUITE_VERSION` →
`smaragd/cmake/SmaragdVersion.cmake` → `smaragd/cmake/smaragd_version.h.in`
already exists, *because* two hard-coded `"1.0.0"`s drifted apart — the same
failure this plan must not repeat in a different currency.

1. A `PRODUCT` file beside `nassau-suite/VERSION`, one line: `volume`.
2. `nassau-suite/CMakeLists.txt` reads it with `file(STRINGS)` before
   `project()`, as it already does for `VERSION`, and forwards
   `NASSAU_PRODUCT_NAME` into each component's `CMAKE_ARGS`.
3. `SmaragdVersion.cmake` defaults it to `Smaragd` for a standalone build —
   honest, the way `NASSAU_SUITE_VERSION` defaults to `dev` rather than
   claiming to be a release — and `@NASSAU_PRODUCT_NAME@` joins the generated
   header as `SMARAGD_PRODUCT_NAME`.

### The carrier is the generated macro, not an app-layer accessor

This is the correction the review forced, and it is structural. **21 of the 41
literals are in `smaragd/tw303a/`**, and engine code may not include an app
header — `tools/check_layering.py` rule 1 forbids it and the `tw_*` static
libraries do not link the app targets. So an accessor in `app_model` cannot
serve half the sweep.

It does not need to. `smaragd/CMakeLists.txt` has, directory-wide:

```cmake
include_directories(${CMAKE_BINARY_DIR}/generated)   # smaragd_version.h (M5)
```

so the generated header already reaches **every** target on both sides of the
layering boundary, app and engine alike, and `main.cpp` and `sapplication.cpp`
include it today. `SMARAGD_PRODUCT_NAME` is therefore the one carrier, and no
new header or layering edge is needed anywhere.

4. **App side**: `SApplication::SApplication()` calls
   `setApplicationDisplayName(SMARAGD_PRODUCT_NAME)` — Qt's own "display name
   distinct from `applicationName()`", which is exactly this plan's
   distinction, and which notably does **not** disturb `applicationName()` and
   so does not move any `QStandardPaths` location. Call sites read
   `QGuiApplication::applicationDisplayName()`.
5. **Engine side**: reads the macro directly. No Qt dependency is added.
6. The two installers and the licence document take it the way the version
   already arrives — `distribution.xml.in` by **`sed` substitution in
   `ci/package.sh`** (it is not a `configure_file`), `smaragd.iss` by `GetEnv`
   beside `NASSAU_SUITE_VERSION`.

A component version is "a developer/forensics number, not a headline" (D5).
The same split applies here: the stamp keeps saying `component=smaragd/<ver>`,
because it answers *which build is this* for a developer, and
`ci/stamp-check.sh` greps for those exact bytes.

**Unverified, and to be settled in stage 3 rather than assumed:** whether Qt
appends the display name to `QWidget` window titles on any platform we ship.
If it does, `SMainWindow::updateWindowTitle()`'s own `"Smaragd - %1"` prefix
must go or the title doubles. Measure it; do not reason about it.

## 5. How it is kept true

A one-time sweep of 41 literals decays on the next commit. qbx holds its
invariants with checkers in `ci/gates.sh`, and this one fits.

**`tools/check_product_name.py`** does two jobs, and the second is the one that
matters:

1. **Forbid new display literals.** Fail on a bare `"Smaragd"` in a string
   literal in application or engine code.
2. **Assert the frozen keys are byte-identical to a pinned list** — the
   QSettings tuple at both sites, `com.smaragd.SecretStore`, `com.smaragd.media`
   at its live call site, `dev.tweggen.smaragd`, the two setter calls, and the
   three MIDI names. If one of them *changes*, the checker fails.

Job 2 replaces the runtime gate this plan originally proposed, which **could
not be built**: with a compile-time constant a test has no way to configure a
different product name, so "change the name and assert the paths did not move"
had nothing to turn. A static byte-identity assertion needs no running app and
catches exactly the regression a diff reviewer cannot see — someone sweeping
`"Smaragd"` and catching an identity key in the net.

**Exemptions use the house marker, not a central list.** Both existing
checkers exempt per site with a trailing comment —
`tools/check_logging.py`'s `ALLOW_COMMENT = "check_logging: allow"` and
`check_tempo_authority.py` likewise. So:

```cpp
setOrganizationName( "Smaragd" );  // check_product_name: allow — frozen identity key, plan 51 §3
```

That marker *is* stage 0's comment, so the comment pass and the checker become
one thing, and there is no central 41-entry list to become a merge-conflict
magnet. `ALLOW_FILES` covers the test trees wholesale.

**Scope, stated so the implementer does not have to guess:** string literals
only, in `smaragd/main` and `smaragd/tw303a`, excluding `**/tests/**`,
`smaragd/main/testkit/**` and `smaragd/tests/**`. Comments are not matched —
`SOptionsDialog` truthfully names `smaragd.ini`, and a checker that flagged it
would invite someone to "fix" a true statement.

## 6. Staging

Each stage stands alone and is separately reviewable.

| | What | Why this order |
|---|---|---|
| **0** | Fix the `"qbx Projects"` filter and the `~/Documents/smaragd` default. Amend `main/shell/CONTRACT.md` invariants 41/42 with the freeze rule. | A real bug fixed, and the rule recorded where the code's own contract lives. Valuable even if the rename is abandoned. |
| **1** | `PRODUCT` + the suite forwarding + `SMARAGD_PRODUCT_NAME` in the generated header + the standalone default. No call sites changed. | The mechanism, provably inert: the build still says Smaragd everywhere. |
| **2** | `tools/check_product_name.py` with job 2 (frozen-key assertions) live and job 1 **warn-only**; the `check_product_name: allow` markers added at every Class A site; register it in `ci/gates.sh` and update `CLAUDE.md`, which currently says "the four checkers". | The identity-key protection lands *before* anything is swept, which is the whole point. Job 1 warns rather than fails so this stage does not depend on stage 3. |
| **3** | The sweep: 20 app sites via `applicationDisplayName()`, 21 engine sites via the macro. Measure the Qt title-append question. Flip job 1 to failing. | Gated by stage 2, and the only stage that touches the engine. |
| **4** | The suite sweep: both installers, `ci/package.sh`'s display text, the licence document, `README.md`, `docs/RELEASING.md`. | Changes what a user reads on an installer pane; wants its own review. |
| **5** | Set `PRODUCT` to `volume`. | One line in the suite. |

Stage 5 is deliberately trivial and deliberately last. What it does **not**
cover is listed in §8; the one-line claim is about the mechanism, not about
every string in the product.

## 7. Open decisions, not settled here

- **Plug-in names.** `NassauEQ`, `NassauAnalogue`, `NassauZermatt`, `Mangrove`
  are publisher-branded, which stays coherent under a `volume` suite.
  **Recommendation: leave them.** Their installed filenames and macOS
  `pkg-ref` ids are Class A, so a rename is not free.
- **`twPluginRegistry::appendBuiltins_nolock()`** — `passThrough.vendor =
  "Smaragd"` and `native.name = "Smaragd 303"`. These have a **measured
  cost**: `smaragd/tests/cases/` holds **83 `vendor="Smaragd"` assertions
  across 61 files**, 7 `"Smaragd 303"`, and one case asserting the
  `"Smaragd exiting"` log line. Changing the vendor is a 61-file test edit.
  **Recommendation: leave the vendor, and let the checker's `ALLOW_FILES`
  cover the cases** — but decide it explicitly rather than discovering it in
  stage 3.
- **Plug-in host identity** — CLAP `host_.name`/`vendor`, VST3
  `IHostApplication::getName` → `"Smaragd"`. Third-party plug-ins occasionally
  key compatibility shims on the host name, so changing it is a compatibility
  change, not a cosmetic one. Decide alongside the vendor question.
  (`twclapplugin.cc` also carries `host_.version = "1.0.0"` — a *third*
  hard-coded version the plan-50 unification missed. Its own small ticket.)
- **Trademark and searchability.** A lowercase common noun is hard to search
  for and hard to register. Stated as fact, not objection — and the author's
  own "this might change at any time" is why the indirection is worth building
  regardless of which name wins.

## 8. What this plan does not do

- It does not rename repositories, directories, CMake targets, `smaragd.exe`,
  `tw303a`, `.qxp`, `.qxa` or the `SMARAGD_*` environment variables.
- It does not migrate any existing user data, and afterwards there is still
  **no mechanism** for doing so. If a Class A location must ever genuinely
  move, that is its own ticket with its own consent dialog.
- It does not touch `AppId`, bundle identifiers, `pkg-ref` ids, the libsecret
  schema, the keychain service or the MIDI port names.
- It does not decide the plug-in, vendor or host-identity names (§7).
- **Consequences stage 5 leaves standing, named so they are not surprises:**
  - macOS shows **`volume` in the menu bar and `smaragd.app` in Finder**,
    because `CFBundleName` is display and the bundle directory is frozen.
  - `--version` keeps leading with `smaragd`, because `applicationName()` is
    an identity key.
  - Artifact filenames (`Smaragd-<ver>.pkg`, `Smaragd-<ver>-setup.exe`) and
    release prose (`ci/release.sh`'s tag messages, `docs/RELEASING.md`'s
    title) cannot flow from `PRODUCT`; stage 4 enumerates them or accepts
    drift.
  - The qbx standalone default stays `Smaragd`, so every qbx CI run and every
    `.qxa` case executes under the *other* name. Any case asserting
    user-visible text must read the accessor, never a literal.
- **Searched for and confirmed absent**, so nobody looks again: no `.desktop`,
  MIME or AppStream file; no `[Registry]` section in the `.iss`; no
  `QLockFile`, `QSharedMemory`, `QLocalServer`, D-Bus name or single-instance
  guard; no user-agent string; no crash reporter. `L"SmaragdVst3ProbeHost"`
  (a Win32 window class in `vst3_probe.cc`) is looked up by nothing.
- **Unresolved, and the plan says so rather than guessing:** whether Inno
  Setup leaves the old `Smaragd` Start-menu group behind after a rename — it
  upgrades by `AppId` without uninstalling first. Check on a Windows box in
  stage 4.

## 9. Review history

Reviewed 2026-10-09 (Fable). Verdict YELLOW; the premise held and most
load-bearing claims verified, but it found one blocker and several factual
errors. What changed:

- **Blocker.** The accessor was in `app_model`, which cannot reach the 21
  engine literals (layering rule 1). §4 now carries the name in the generated
  macro, which `include_directories(${CMAKE_BINARY_DIR}/generated)` already
  puts on every target.
- §3's reason for freezing the setters was **wrong** — `QSettings` does not
  derive the INI path from them. Replaced with the real reason
  (`QStandardPaths`), and the error itself kept as the argument for the rule.
- The service name's live site is the `SMediaAccountManager` call, not the
  header default; and it is a lookup key on Linux as well as macOS.
- The count was 37 from an unreproducible grep; it is **41**, and the command
  is now in the document.
- The pkg identifiers exist in `ci/package.sh` as well as `distribution.xml.in`;
  `distribution.xml.in` is substituted by `sed`, not `configure_file`.
- **Missed identity keys added:** the MIDI virtual client and port names, the
  `smaragd.app` bundle directory, the `QStandardPaths` cache roots.
- The §5 runtime gate **could not be built** against a compile-time constant;
  replaced with a static byte-identity assertion in the checker.
- The 41-entry allowlist became the house `check_product_name: allow` marker,
  collapsing stages 0 and 2.
- Added: the measured 83/61-file test cost of a vendor rename, the
  `ci/gates.sh` and `CLAUDE.md` edits stage 2 needs, and the §8 consequence
  list.

One review claim was **not** adopted as stated: that the macOS row should drop
"code signing, keychain ACLs" as overstated. It is overstated for *today's*
ad-hoc-signed build, so the row now names microphone consent alone — which is
the part that is true regardless of signing, and QBX-133 will make the rest
true.
