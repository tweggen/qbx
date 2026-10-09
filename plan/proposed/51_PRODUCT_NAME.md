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

That grep is **case-sensitive**, and the lowercase name is a separate
population of **35** more literals:

```bash
grep -rn '"[^"]*smaragd[^"]*"' --include=*.cpp --include=*.cc --include=*.h \
     --include=*.mm --include=*.c smaragd/main smaragd/tw303a \
  | grep -v '/tests/' | grep -v 'Smaragd'
```

Almost all 35 are **Class C** — `smaragd.ini`, log and cache filenames, paths —
and must NOT be swept. But one is Class A: `setApplicationName("smaragd")`.
This is why the checker has to be case-aware rather than matching the word
(§5): a checker that flagged the lowercase population would invite someone to
rename files, which is exactly what the ticket excludes.

**These numbers are the SURVEY, not the checker's scope.** The grep above
includes `main/testkit/**` and comment lines; §5's checker excludes both, so it
will report a smaller number. Neither is wrong. Expect roughly 10–12 app sites
and 0–7 engine sites to be actual sweep targets — the by-class table in §6
stage 3 has the breakdown.

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
| `main/CMakeLists.txt` — `MACOSX_BUNDLE_GUI_IDENTIFIER` | `dev.tweggen.smaragd` | macOS **microphone consent**: TCC keys consent on the bundle identifier, so a DAW that changes it loses audio input until the user re-grants it. *This is documented macOS behaviour, not verified from this repository* — the only Class A claim here that is not. Code signing and keychain ACLs are deliberately NOT cited: the build is ad-hoc signed today, so that part would be overstated until QBX-133 |
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
may double the name.

Review offered a narrower mechanism — that the append lives in
`QPlatformWindow::formatWindowTitle()`, is used by the Windows and XCB backends
but not Cocoa, and fires only when the title does not already contain the
display name, so `"volume - project"` would never double and only a bare
`"project"` would gain a suffix. **That was offered from memory and is not
verified here, so it is recorded as the hypothesis to test, not as fact.**
Stage 3 measures both title shapes on all three platforms and picks one
deliberately; keeping the explicit prefix is the safe default. Note that once a
display name is set, every `QMessageBox` caption becomes a candidate for the
same append.

## 5. How it is kept true

A one-time sweep decays on the next commit. qbx holds its invariants with
checkers in `ci/gates.sh`, and this one fits.

**`tools/check_product_name.py`** does two jobs, and the second is the one that
matters.

### Job 1 — forbid new display literals

Fail on a capital-`Smaragd` string literal in application or engine code.

**Scope, stated so the implementer does not guess:** string literals only, in
`smaragd/main` and `smaragd/tw303a`; excluding `**/tests/**`,
`smaragd/main/testkit/**` and `smaragd/tests/**`; **comments not matched**
(`SOptionsDialog` truthfully names `smaragd.ini`, and flagging it would invite
someone to "fix" a true statement); and **case-sensitive on the capital form
only** — the 35 lowercase literals are filenames and paths the ticket
explicitly leaves alone.

### Job 2 — pin the frozen keys, by file and pattern

This is the real protection, and it is implementable **today, before any
sweep** — every value below exists in the tree now. A pin needs a file, a
pattern and an **expected count**, because without the count a refactor that
drops an occurrence passes silently:

| File | Pattern | Count |
|---|---|---|
| `main/shell/src/ssettings.cpp` | `IniFormat, QSettings::UserScope,\s*"Smaragd", "smaragd"` | 1 |
| `main/shell/src/smediaaccountmanager.cpp` | the same quadruple | 1 |
| `main/shell/src/smediaaccountmanager.cpp` | `QStringLiteral( "com.smaragd.media" )` | 1 |
| `main/shell/include/app/shell/ssecretstore.h` | `serviceName = QStringLiteral( "com.smaragd.media" )` | 1 |
| `main/shell/src/sapplication.cpp` | `setOrganizationName( "Smaragd" )` | 1 |
| `main/shell/src/sapplication.cpp` | `setApplicationName( "smaragd" )` | 1 |
| `main/shell/src/ssecretstore_linux.cpp` | `"com.smaragd.SecretStore"` | 1 |
| `main/CMakeLists.txt` | `MACOSX_BUNDLE_GUI_IDENTIFIER "dev.tweggen.smaragd"` | 1 |
| `tw303a/devices/src/alsa_seq_midi.cc` | the MIDI name constants (see below) | 2 |
| `tw303a/devices/src/coremidi_midi.cc` | the MIDI name constants | 2 |

**The header default is pinned too, and asserted EQUAL to the call site.** The
product passes `com.smaragd.media` explicitly, so the default argument is dead
today — but if someone later drops the explicit argument, the default takes
over. Pinning only the live site would leave that silent.

**The 12 MIDI literals are refactored to one `constexpr` per file first.**
`alsa_seq_midi.cc` spells `ensureSeq("Smaragd")` three ways and
`name.empty() ? std::string("Smaragd") : name` twice; pinning 12 call sites is
brittle, pinning two definitions is not. The refactor is behaviour-neutral and
belongs in stage 2 beside the markers.

Job 2 replaces the runtime gate this plan originally proposed, which **could
not be built**: with a compile-time constant a test has no way to configure a
different product name, so "change the name and assert the paths did not move"
had nothing to turn. A static pin needs no running app and catches exactly the
regression a diff reviewer cannot see — someone sweeping `"Smaragd"` and
catching an identity key in the net.

### Exemptions use the house marker, per file type

Both existing checkers exempt per site with a trailing comment —
`tools/check_logging.py`'s `ALLOW_COMMENT = "check_logging: allow"` and
`check_tempo_authority.py` likewise. The checker matches **only that prefix**,
so the text after it is free and carries the reason:

```cpp
setOrganizationName( "Smaragd" );  // check_product_name: allow — frozen identity key, plan 51 §3
passThrough.vendor = "Smaragd";    // check_product_name: allow — deliberate vendor string, plan 51 §7
```

Two distinct reasons, because §7's vendor and host-identity strings are *kept
on purpose* rather than frozen as data addresses — and without a marker they
would fail job 1 the moment stage 3 flips it to failing.

The marker is a C/C++ mechanism. Elsewhere:

| Site | Marker | What actually protects it |
|---|---|---|
| C/C++ | trailing `// check_product_name: allow — …` | job 1 skips the line; job 2 pins the value |
| `main/CMakeLists.txt` | trailing `# …` (legal between `set_target_properties` arguments) | **job 2's pin** — job 1 does not scan CMake, and the value holds no capital `Smaragd` anyway, so the marker here is documentation |
| `distribution.xml.in` | `<!-- … -->` on the preceding line (XML cannot carry one inside an attribute) | **not the qbx checker** — see below |
| `ci/package.sh`, `package-windows.sh` | trailing `#` | same |
| `smaragd.iss` | `; …` starting the line (Inno comments must) | same |

### The suite-side keys need a suite-side pin

`tools/check_product_name.py` lives in qbx and cannot see nassau-suite at all,
and nassau-suite has no static checker. The cheapest strong place is
**`ci/package-check.sh`**, which already runs `pkgutil --expand` and reads the
expanded `Distribution` file: assert the five `dev.tweggen.smaragd.*`
identifiers there and in the component receipts, and assert the app installs as
`smaragd.app`. That tests the **built artifact** rather than the source, which
is better, and needs no new workflow step. Pin the `.iss` `AppId` GUID by grep
in the same script.

**Honest limit:** nassau-suite's only CI job is token-gated and no-ops without
`NASSAU_CI_TOKEN`, so this pin does not run on a fork or on any PR without the
secret. Whoever lands it says so in the PR body, per the house rule that a PR
states what was *not* gated.

## 6. Staging

Each stage stands alone and is separately reviewable.

| | What | Why this order |
|---|---|---|
| **0** | Fix the `"qbx Projects"` filter and the `~/Documents/smaragd` default. Amend `main/shell/CONTRACT.md` invariants 41/42 with the freeze rule. | A real bug fixed, and the rule recorded where the code's own contract lives. Valuable even if the rename is abandoned. |
| **1** | `PRODUCT` + the suite forwarding + `SMARAGD_PRODUCT_NAME` in the generated header + the standalone default. No call sites changed. | The mechanism, provably inert: the build still says Smaragd everywhere. |
| **2** | Refactor the 12 MIDI literals to one `constexpr` per file. `tools/check_product_name.py` with job 2 live and job 1 **warn-only**; `check_product_name: allow` markers at every Class A **and** §7 site; the suite-side pins in `ci/package-check.sh`; register it in `ci/gates.sh` and update `CLAUDE.md`, which says "the four checkers". | The identity-key protection lands *before* anything is swept, which is the whole point. Job 1 warns rather than fails, so this stage does not depend on stage 3 — verified: every value job 2 pins exists in the tree today. |
| **3** | The sweep (see the table below), plus `MACOSX_BUNDLE_BUNDLE_NAME` → `${NASSAU_PRODUCT_NAME}` — the CMake variable, not the C macro. Measure the Qt title-append question. Flip job 1 to failing. | Gated by stage 2. **Precondition:** §7's vendor and host-identity decisions must be made first, or job 1 cannot be flipped. |
| **4** | The suite sweep: both installers, `ci/package.sh`'s display text, the licence document, `README.md`, `docs/RELEASING.md`. | Changes what a user reads on an installer pane; wants its own review. |
| **5** | Set `PRODUCT` to `volume`. | One line in the suite. |

### What stage 3 actually sweeps

The 41 survey hits are **not** 41 sweep sites. By class:

| | Sites | What |
|---|---|---|
| App, Class A | 3 | frozen in stage 2; marker only |
| App, display in a sensitive file | 2 | the libsecret item label `"Smaragd: %1"` and DPAPI's `L"Smaragd secret"` — both display-only (Seahorse shows the label), but they sit in the secret-store backends, so they get their own review |
| App, comments | 2 | job 1 does not match comments |
| App, test code | 3 | `action_roundtrip_test.cpp`; outside the checker's scope |
| **App, genuine sweep** | **10** | `smainwindow.cpp` ×6, `main.cpp` ×2, `sstdmixerview.cpp`, `soptionsdialog.cpp` |
| Engine, Class A | 12 | the MIDI names — **frozen**, marker only |
| Engine, Class C | 2 | `vst3_probe.cc`'s window class, looked up by nothing |
| **Engine, open per §7** | **7** | registry ×3, CLAP ×2, VST3 ×2 |

So stage 3 sweeps **10 app sites, and between 0 and 7 engine sites depending on
§7**. If §7's recommendations stand — leave the vendor, leave host identity —
**stage 3 does not touch the engine at all**, and stage 2 becomes the only
stage that does, via the MIDI constants and the markers.

This table exists because the previous draft's staging said "21 engine sites
via the macro" while §2 and §8 froze 12 of those same sites. An implementer
following the table literally would have renamed the MIDI ports.

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
  stage 3, because §6 stage 3 cannot flip job 1 to failing until it is decided.
  (The case asserting the `"Smaragd exiting"` log line is **not** part of this
  decision: that log line is Class B and stage 3 sweeps it, so that one case
  changes in the stage-3 PR.)
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
  - ~~Artifact filenames cannot flow from `PRODUCT`.~~ **Wrong, corrected
    while implementing stage 4.** Both installers build their own output name,
    so `Smaragd-<ver>.pkg` and `Smaragd-<ver>-setup.exe` do follow the product
    name. Only *markdown* prose cannot — `docs/RELEASING.md` and `README.md`
    are static files, so stage 4 made their product-facing sentences
    name-NEUTRAL ("the DAW", "the suite") rather than substituting a name that
    would go stale at the next change. `ci/release.sh`'s tag messages are
    shell and could follow it; left alone, because a git tag message describes
    a release that already happened.
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

### Second pass, 2026-10-09

Verdict YELLOW again, "one edit short of GREEN", and the blocker was an
**internal contradiction introduced by the first revision**: stage 3's row said
"21 engine sites via the macro" while §2 and §8 froze 12 of those same sites as
the MIDI names. An implementer following the staging table literally would have
renamed the MIDI ports — the exact failure the plan exists to prevent, written
into the plan's own instructions. §6 now carries a by-class table, and the
count of real sweep sites is 10 app plus 0–7 engine depending on §7.

Also from this pass:

- Job 2 needed a **file + pattern + expected count** table to be
  implementable; "byte-identical to a pinned list" named no sites. Without the
  count, a refactor that drops an occurrence passes silently.
- The `ssecretstore.h` **default argument is now pinned too, and asserted
  equal to the call site** — it is dead today, but it takes over if anyone
  drops the explicit argument.
- The 12 MIDI literals are **refactored to one `constexpr` per file** before
  being pinned; `alsa_seq_midi.cc` spells the name three different ways.
- The `: allow` marker is a C/C++ mechanism and **does not work** at the CMake,
  XML, shell and Inno sites. §5 now gives the comment syntax per file type and,
  more importantly, says what actually protects each one — for the suite that
  is a new pin in `ci/package-check.sh`, which already expands the built `.pkg`
  and reads its `Distribution`. With the honest limit that the suite's CI job
  no-ops without `NASSAU_CI_TOKEN`.
- `MACOSX_BUNDLE_BUNDLE_NAME` is a Class B site **outside job 1's scope**
  (CMake, not C++), and one of the first strings a macOS user sees. Stage 3
  sets it explicitly.
- §7's kept-on-purpose strings need the marker with a **different reason** from
  the frozen keys, or job 1 fails on them the moment stage 3 flips it.
- The Qt title-append mechanism was offered in more detail
  (`QPlatformWindow::formatWindowTitle()`, Windows and XCB but not Cocoa, only
  when the title does not already contain the name). Offered from memory and
  **not verified**, so §4 records it as the hypothesis to test rather than as
  fact — the stance that has been right repeatedly in this work.

**Found while verifying this pass, by neither the plan nor the review:** the
survey grep is case-sensitive, so it missed **35 lowercase-only literals**,
one of which — `setApplicationName("smaragd")` — is Class A. The rest are
filenames and paths that must not be swept, which turns case-awareness from a
detail into a checker requirement (§5 job 1).

### Corrections made while implementing, 2026-10-09

Kept here rather than silently edited in, because a plan whose errors are
invisible teaches nothing the second time.

- **§6 stage 0 said to rename `~/Documents/smaragd`.** It must not be: that is
  a filesystem path, which the ticket excludes in as many words, and a user
  with projects there would find new ones saved elsewhere. Stage 0 fixed the
  dialog *filter* only and commented the directory as deliberate.
- **§5's MIDI pin said two constants per backend.** It is **three** — a client
  name and two port names. Counted by reading the files.
- **§8 said artifact filenames cannot follow `PRODUCT`.** They can; see above.
- **§4's Qt title-append question is now half-measured.** Setting
  `setApplicationDisplayName()` changes neither `QWidget::windowTitle()`, nor
  `QMessageBox`'s default caption, nor — the one that matters —
  `applicationName()`, so no `QStandardPaths` location moves. What remains
  unmeasured is what a native window manager paints, which is QPA-private and
  not observable from a headless Linux box. Stage 3 therefore keeps the
  product name in the title EXPLICITLY rather than relying on the platform:
  the cost is a possibly doubled name in the native bar on Windows and X11,
  which is cosmetic, against the alternative of no product name at all in the
  title on macOS.

## 10. What to watch during implementation

Carried from review, and worth keeping where the implementer will read it:

- **Stage 2's warn-only job 1 prints ~17 warnings on every
  `./ci/gates.sh --static` run** until stage 3 lands. Keep that below the
  threshold where people stop reading the checker output, or stage 3 follows
  stage 2 closely.
- **Every `.qxa` case runs under `Smaragd`**, because the qbx standalone
  default stays that. A case wanting to assert a caption has no way to read the
  configured name — there is no `describe` field for it. Either add one in
  stage 1 or forbid literal captions in cases outright. Decide in stage 1, not
  in stage 3.
- **The Finder / menu-bar split** (§8) is the first thing a macOS user will
  report as a bug after stage 5. Stage 4 should decide whether to set
  `CFBundleDisplayName` as well, or to accept it until the bundle directory is
  renamed in its own ticket alongside the `pkg-ref` migration.
- **File the `host_.version = "1.0.0"` ticket before stage 3**, so the engine
  sweep PR does not pick up a third hard-coded version by accident and widen
  its own scope.
