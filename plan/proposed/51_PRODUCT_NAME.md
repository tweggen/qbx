# Proposal 51 — a user-visible product name that can be changed later

**Ticket:** QBX-137. **Status:** proposed. **Written:** 2026-10-09.

Source locations below are given by **function name**, never by line number:
this project has already had a document (`docs/SIGNAL_CHAIN.md`) carry nine
line-numbered rows of which every one had rotted.

The ticket's request is not "rename the product". It is: *define the product
name in a way that it can later be modified*, while repo names and filesystem
paths stay as they are. This plan is about the indirection. The first value put
through it happens to be `volume`, but the plan is written so that the second
and third values cost nothing.

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
the Ableton Live shape rather than the Logic Pro shape.

`Smaragd` becomes what it always was in substance: an internal code name. The
value of this plan is that it stops being a *user-visible* one.

## 2. What is in the code today

Measured 2026-10-09 at `origin/main` of each repo.

**qbx: 37** `"Smaragd"` literals in non-test application code. **nassau-suite:**
name sites in 23 files, but the large counts there (`CMakeLists.txt` 28,
`ci/build.sh` 10) are almost entirely the *submodule path* and *target name*,
which this plan does not touch. The genuine display sites in the suite are
`packaging/macos/distribution.xml.in`, `packaging/windows/smaragd.iss`,
`packaging/macos/collect-licences.sh`, `README.md` and `docs/RELEASING.md`.

The survey's one real finding is that those 37 literals are **two different
kinds of thing wearing the same string**, and the ticket's "paths may remain"
does not cover the dangerous kind.

### Class A — identity keys. These are ADDRESSES OF THE USER'S DATA.

| Where | Value | What a change costs |
|---|---|---|
| `ssettings.cpp` — `SSettings::SSettings()` | `QSettings(IniFormat, UserScope, "Smaragd", "smaragd")` | every setting, window layout and recent-project entry — `~/.config/Smaragd/smaragd.ini`, `%APPDATA%/Smaragd/smaragd.ini` |
| `smediaaccountmanager.cpp` — `SMediaAccountManager::SMediaAccountManager()` | the **same quadruple, independently written** | the media accounts would read a different file from the settings |
| `ssecretstore_linux.cpp` — `schema()` | `com.smaragd.SecretStore` (libsecret schema) | every stored password on Linux |
| `ssecretstore.h` — `SSecretStore::SSecretStore()`'s default argument | `com.smaragd.media` (default `serviceName`) | the same on macOS — it becomes `kSecAttrService` |
| `main/CMakeLists.txt` — the `MACOSX_BUNDLE_GUI_IDENTIFIER` property | `dev.tweggen.smaragd` (`MACOSX_BUNDLE_GUI_IDENTIFIER`) | code signing, keychain ACLs, and **microphone permission**: macOS TCC keys consent on the bundle identifier, so a DAW that changes it loses audio input after an update and the user must re-grant it |
| `nassau-suite/packaging/macos/distribution.xml.in` | `dev.tweggen.smaragd.app` / `.vst3` / `.au` (`pkg-ref` ids) | upgrade and uninstall detection by the macOS installer |
| `nassau-plugin-sdk` stamp | the literal token `NASSAU_STAMP` | `ci/stamp-check.sh` greps for it |

Two of these are already right and should be left alone: the Inno Setup
`AppId` is a GUID, so a Windows rename keeps its upgrade path; and `.qxp` /
`.qxa` carry no product name at all.

### Class B — display strings. Freely changeable; this is what the plan makes variable.

Window titles (`SMainWindow::updateWindowTitle()`), `QMessageBox` captions, the options
page's cache note, `MACOSX_BUNDLE_BUNDLE_NAME`, the Inno `AppName`,
`DefaultDirName` and `DefaultGroupName`, the macOS `distribution.xml` titles
and descriptions, the licence document's heading, both READMEs.

### Class C — code, target and path names. Out of scope, per the ticket.

### A defect found on the way, which argues the ticket's case

`SMainWindow::fileSaveAs()` and `SMainWindow::fileOpen()` show the user
`"qbx Projects (*.qxp)"`. So the product presents **three** names today —
*Smaragd* in titles, *qbx* in file dialogs, *smaragd* in the executable and the
INI. Whatever is decided about `volume`, that is a bug now.

## 3. The rule this plan exists to write down

> **An identity key is frozen at its current value, permanently, whatever the
> product is called.**

`com.smaragd.*`, `dev.tweggen.smaragd*` and the `("Smaragd", "smaragd")`
QSettings pair are not names. They are where the user's settings, window
layout and saved passwords live. Renaming them is a data migration with a
consent dialog, not a rename — and a *silent* one, because the application
would start cleanly against an empty new location and look merely forgetful.

Each one therefore gets a comment at its site saying it reads wrong on
purpose. This is the part of the plan most likely to be undone by a future
session tidying up, which is exactly why it is a rule and not a remark.

Illustration of how sharp the trap is: `SApplication::SApplication()` says
`setOrganizationName("Smaragd")`. That is wrong twice over — the organisation
is Nassau, not Smaragd, and the product is no longer Smaragd either. It must
still not be touched, because `QSettings` derives the config directory from it.
A reviewer who "fixes" that one line orphans every existing installation's
settings.

This plan's digits-of-the-version rule applies by analogy: the suite version
makes a promise about the user's saved work, and so does a rename. A rename
that loses settings is a breaking change dressed as a cosmetic one.

## 4. Where the name lives

**Reuse the version pipeline. Do not build a second mechanism.**

There is already one source of truth flowing from the suite into every
component: `VERSION` at the suite root → `NASSAU_SUITE_VERSION` →
`smaragd/cmake/SmaragdVersion.cmake` → `smaragd/cmake/smaragd_version.h.in`.
That pipeline exists *because* two hard-coded `"1.0.0"`s drifted apart, which
is the same failure this plan is trying not to repeat in a different currency.

1. A `PRODUCT` file beside `nassau-suite/VERSION`, one line: `volume`.
2. `nassau-suite/CMakeLists.txt` reads it with `file(STRINGS)` before
   `project()`, as it already does for `VERSION`, and forwards
   `NASSAU_PRODUCT_NAME` into each component's `CMAKE_ARGS`.
3. `SmaragdVersion.cmake` defaults it to `Smaragd` for a standalone build —
   honest, the way `NASSAU_SUITE_VERSION` defaults to `dev` rather than
   claiming to be a release — and `@NASSAU_PRODUCT_NAME@` joins the generated
   header.
4. One accessor in `app_model`, the lowest layer that can carry it, so every
   layer above may read it without a new dependency edge
   (`tools/check_layering.py` enforces that set).
5. The two installers and the licence document take it the same way the
   version already arrives: `distribution.xml.in` by `configure_file`,
   `smaragd.iss` by `GetEnv` beside `NASSAU_SUITE_VERSION`.

A component version is "a developer/forensics number, not a headline" (D5).
The same split applies here: the stamp keeps saying `component=smaragd/<ver>`,
because the stamp answers *which build is this* for a developer, and
`ci/stamp-check.sh` greps for those exact bytes.

## 5. How it is kept true

A one-time sweep of 37 literals decays on the next commit. qbx holds its
invariants with checkers in `ci/gates.sh`, and this one is a good fit:

**`tools/check_product_name.py`** — fails on a bare `"Smaragd"` in user-facing
application code, with an explicit allowlist naming each Class A key and *why*
it is exempt. Nothing else in this plan prevents the 38th literal.

### The gate that matters is not the title bar

A `.qxa` case asserting the window title follows the configured name is easy
and nearly worthless: it would pass with the whole Class A rule violated.

The case worth writing asserts the opposite — that **changing the display name
leaves `SSettings::configDir()` and the secret-store service unchanged**. That
is the regression that loses data rather than merely looking wrong, and it is
the one a reviewer cannot see by reading a diff. Per this project's rule on
vacuous gates, it must be built so that the correct and the broken
implementation differ: configure a *different* product name in the test and
assert the paths did not move.

Note the existing hazard here. A test that touches the real settings location
would write the developer's own `smaragd.ini`, and `secret_store_test.cpp`
already documents the isolation needed. This case must assert *the computed
path*, not read or write the file.

## 6. Staging

Each stage stands alone and is separately reviewable.

| | What | Why this order |
|---|---|---|
| **0** | Comment every Class A key as frozen. Fix the `"qbx Projects"` filter. | The safety net, and a bug fixed, before anything moves. Valuable even if the rename is abandoned. |
| **1** | `PRODUCT` + the pipeline + the accessor + the default. No call sites changed. | The mechanism, provably inert: the build still says Smaragd everywhere. |
| **2** | `tools/check_product_name.py` with every current literal allowlisted, plus the paths-do-not-move gate. | The checker lands *before* the sweep, so the sweep is what empties the allowlist rather than the allowlist being written to match whatever the sweep happened to do. |
| **3** | The qbx sweep: 37 literals, allowlist shrinking to the Class A keys. | Mechanical, and gated by stage 2. |
| **4** | The suite sweep: both installers, the licence document, both READMEs. | Changes what a user reads on an installer pane; wants its own review. |
| **5** | Set `PRODUCT` to `volume`. | One line. That is the whole point of the four stages above. |

Stage 5 is deliberately trivial and deliberately last. If it is not a one-line
change by then, this plan has failed and stages 1–4 should be re-read rather
than worked around.

## 7. Open decisions, not settled here

- **Plug-in names.** `NassauEQ`, `NassauAnalogue`, `NassauZermatt`, `Mangrove`
  are publisher-branded, which stays coherent under a `volume` suite (Nassau
  publishes volume; the plug-ins are Nassau's). But the plug-ins are also
  visible in *third-party hosts*, where the publisher name is the more useful
  one. **Recommendation: leave them.** Renaming them is not free — a VST3 or
  CLAP plug-in is found by its uid, so a display rename is safe, but the
  installed *filenames* and the macOS `pkg-ref` ids are Class A.
- **`twPluginRegistry::appendBuiltins_nolock()`** — `passThrough.vendor = "Smaragd"` and
  `native.name = "Smaragd 303"` appear in the DAW's own plug-in list. Display
  class, but they are engine-side and read as a vendor; worth deciding
  alongside the plug-in question rather than sweeping blind.
- **Trademark and searchability.** A lowercase common noun is hard to search
  for and hard to register. Stated as a fact for the record, not an objection —
  and the author's own "this might change at any time" is the reason this plan
  is worth building regardless of which name wins.

## 8. What this plan does not do

- It does not rename repositories, directories, CMake targets, `smaragd.exe`,
  `tw303a`, `.qxp` or `.qxa`.
- It does not migrate any existing user data, and after it there is still **no
  mechanism** for doing so. If the settings location is ever genuinely to
  move, that is its own ticket with its own consent dialog.
- It does not touch `AppId`, bundle identifiers, `pkg-ref` ids, the libsecret
  schema or the keychain service.
- It does not decide the plug-in names (§7).
- **Nothing here is measured against a working implementation.** The literal
  counts and the file locations are measured; the claim that stage 5 reduces to
  one line is a design intent, and the first attempt at stage 1 is what tests
  it.
