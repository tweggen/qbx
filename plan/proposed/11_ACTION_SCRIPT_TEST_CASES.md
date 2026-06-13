# 11 — Action Scripts & Headless Test Cases

**Status:** Proposed (design only)
**Depends on:** `03_ACTION_MODEL.md`, `03a_ACTION_MODEL_PHASE_2_ROLLOUT.md`
**Relates to:** `04_WIRE_FORMAT_AND_SAMPLE_RATE.md` (XML serialization conventions)

## 1. Goal

Allow Smaragd to be launched with a startup parameter that points at a **text
file describing a sequence of `SAction`s**. On launch, the file is parsed into
concrete `SAction` objects, executed against a project, and the run's outcome is
reported. This turns the existing command/undo model into a **scripting and
automated-testing surface**:

```powershell
smaragd.exe --run-actions tests/cases/group_track_roundtrip.qxa
smaragd.exe --test-case tests/cases/group_track_roundtrip.qxa   # same, with strict exit code
```

The same machinery serves two audiences:

1. **Regression test cases** — author a script + expected assertions, run
   headless in CI (or locally), exit non-zero on failure. This generalises the
   hand-coded `runGroupTrackTest()` currently buried in `smainwindow.cpp`.
2. **Scripting / automation** — drive the app from a file for demos, batch edits,
   or reproducing user-reported bug states.

## 2. Why this is mostly already possible

The action model was designed for exactly this. The pieces that already exist:

| Piece | Location | State |
|-------|----------|-------|
| `SAction::writeXml()` / `readXml()` | `main/include/saction.h:35` | ✅ every action implements it |
| `SAction::name()` (stable verb / XML tag) | `main/include/saction.h:23` | ✅ |
| `SAction::formatVersion()` | `main/include/saction.h:26` | ✅ |
| `SActionRegistry::create(name)` | `main/src/sactionregistry.cpp:16` | ✅ name → factory |
| `SActionRegistry::createFromXml(elem)` | `main/src/sactionregistry.cpp:25` | ⚠️ **implemented but never called anywhere** — untested dead code |
| `SActionRegistry::knownNames()` | `main/src/sactionregistry.cpp:44` | ✅ (for verb listing) |
| `SActionHistory::submit(action)` | `main/include/sactionhistory.h:25` | ✅ executes + records undo |
| Default ctors on action classes | e.g. `SSetTrackVolumeAction() = default` | ✅ required for factory |

What is **missing**:

1. **A sequence/container format.** Individual actions serialize, but there is no
   document that holds an ordered list of them. `createFromXml()` has no caller,
   so per-action XML round-trip has never actually been exercised end-to-end.
2. **A serializer.** `writeXml()` writes only the action's *parameters* — it does
   **not** emit the element tag name or the `version` attribute. The reader
   (`createFromXml`) expects `elem.tagName()` == action name and reads a
   `version` attribute. So a small wrapper is needed to bridge write↔read.
3. **Command-line parsing.** `main.cpp` passes `argc/argv` to `QApplication` and
   ignores them entirely (`main/src/main.cpp:8`). No `QCommandLineParser`.
4. **Test-case semantics.** Actions mutate; a *test* also needs to **assert**
   expected state and produce a pass/fail exit code.

This proposal fills those four gaps.

## 3. File format

### 3.1 Canonical form: XML (`.qxa` — "Qbx Action" script)

XML is chosen as the canonical format because it reuses the existing
`writeXml`/`readXml` machinery verbatim and guarantees lossless round-trip with
zero new per-action code. It is a text file, satisfying the requirement. Each
child element's **tag name is the action verb**; attributes are the action's
serialized parameters (exactly what `writeXml` already produces).

```xml
<?xml version="1.0" encoding="UTF-8"?>
<SActionScript version="1" name="group_track_roundtrip">

  <!-- Optional setup: how the project under test is created. -->
  <setup project="new"/>                     <!-- new | load -->
  <!-- or: <setup project="load" file="fixtures/two_tracks.qxp"/> -->

  <!-- Ordered action steps. Tag = action name(); attrs = writeXml output. -->
  <actions>
    <add-track index="-1"/>
    <add-track index="-1"/>
    <reparent-track sources="1" dest="0"/>
    <set-track-volume trackIndex="0" newVolume="-6.0"/>
  </actions>

  <!-- Optional assertions evaluated after the actions drain. -->
  <assertions>
    <assert-prop key="gridVisible" equals="true"/>
    <assert-track-count equals="1"/>
    <assert-track-volume trackIndex="0" equals="-6.0" epsilon="0.001"/>
  </assertions>

  <!-- Optional undo/redo exercise: replay the inverse stack and re-assert. -->
  <verify-undo restoresInitialState="true"/>

</SActionScript>
```

Each `<actions>` child carries an optional `version="N"` attribute; when absent
the serializer assumes `1` (matching `createFromXml`'s default at
`sactionregistry.cpp:29`).

### 3.2 Optional sugar: line-based DSL (`.qxs`) — *future / non-blocking*

For hand-authoring, a thin line-oriented front-end is friendlier than XML:

```
# group_track_roundtrip
new
add-track index=-1
add-track index=-1
reparent-track sources=1 dest=0
set-track-volume trackIndex=0 newVolume=-6.0
assert track-count == 1
verify-undo
```

This is **not** required for the first cut. It compiles down to the same
in-memory action list. Defer to a follow-up; XML is the v1 deliverable so we get
guaranteed round-trip first.

## 4. New components

### 4.1 `SActionScript` (serializer / container) — `main/`

A new class that owns an ordered `QList<SAction*>` plus setup/assertion metadata,
and knows how to read/write the `.qxa` document.

```cpp
// main/include/sactionscript.h
class SActionScript {
public:
    struct Setup    { enum Kind { New, Load } kind = New; QString file; };
    struct Assertion { QString kind; QMap<QString,QString> args; }; // see §5

    // Parse a .qxa file. Returns false + fills error() on malformed input or
    // an unknown action verb (fail fast — a typo'd verb must not be skipped).
    bool readFile(const QString &path);
    bool readXml(const QDomDocument &doc);

    // Serialize the current action list back out (round-trip / record mode).
    QDomDocument toXml() const;
    bool writeFile(const QString &path) const;

    const Setup &setup() const;
    const QList<SAction*> &actions() const;     // ownership stays with script
    const QList<Assertion> &assertions() const;
    bool verifyUndo() const;

    QString error() const;
};
```

**Serialization bridge (the one real gap).** `writeXml` does not emit the tag or
version, so `SActionScript::toXml()` must:

```cpp
QDomElement el = doc.createElement(action->name());          // tag = verb
if (action->formatVersion() != 1)
    el.setAttribute("version", action->formatVersion());
action->writeXml(el);                                        // params
actionsEl.appendChild(el);
```

Reading reuses the registry directly:

```cpp
SAction *a = SActionRegistry::instance().createFromXml(child); // tag→factory→readXml
if (!a) { error_ = "unknown or malformed action: " + child.tagName(); return false; }
```

This finally exercises `createFromXml()` — **Phase 0 below adds a round-trip unit
check so the previously-dead path is proven before anything depends on it.**

### 4.2 `SActionRunner` (execution + assertions) — `main/`

Drives a parsed `SActionScript` against a project and computes a result.

```cpp
// main/include/sactionrunner.h
class SActionRunner {
public:
    struct Result {
        bool      passed = false;
        int       actionsApplied = 0;
        int       actionsRejected = 0;
        QStringList failures;       // human-readable assertion failures
    };
    // project: the SProject to mutate (created per setup()).
    Result run(const SActionScript &script, SApplication &app);
};
```

Execution policy:

- Actions are submitted via `SApplication::submitAction()` →
  `SActionHistory::submit()` (the **real** dispatch path), so coalescing, the
  undo stack, and Phase-2 engine-thread draining are all exercised, not bypassed.
- A rejected action (`SApplyResult.applied == false`) is a **test failure** by
  default (precondition not met), recorded with the action's `name()` and index.
  A script may opt into "expect-reject" per step via an attribute
  (`expectReject="true"`) for negative tests.
- After draining, assertions (§5) are evaluated against the project.
- If `<verify-undo>` is present, the runner repeatedly calls
  `actionHistory()->undo()` back to the base and asserts the project matches its
  initial serialized snapshot (compare `SProject` XML before vs. after) — this is
  the generalised, data-driven version of today's `runGroupTrackTest()`.

### 4.3 Command-line parsing — `main/src/main.cpp` + `SApplication`

Add a `QCommandLineParser` in `main()` (before window creation):

| Flag | Meaning |
|------|---------|
| `--run-actions <file>` | Parse + execute the script, keep the window open afterwards (scripting/demo use). |
| `--test-case <file>` | Headless test mode: execute, print a TAP-style/`PASS`-`FAIL` summary, **`exit(0)` on pass, `exit(1)` on failure**, do not enter the event loop interactively. |
| `--list-actions` | Print `SActionRegistry::knownNames()` (sorted) and exit — discoverability for script authors. |

Sketch:

```cpp
QCommandLineParser parser;
parser.addOption({"run-actions", "Execute an action script.", "file"});
parser.addOption({"test-case",  "Run an action script as a pass/fail test.", "file"});
parser.addOption({"list-actions", "List known action verbs and exit."});
parser.process(app);

if (parser.isSet("list-actions")) { /* print knownNames(); return 0; */ }

const bool testMode = parser.isSet("test-case");
const QString scriptPath = testMode ? parser.value("test-case")
                                    : parser.value("run-actions");
```

**Headless concern.** Smaragd is a `QApplication` (widgets). For CI without a
display, document running under a virtual display / `-platform offscreen`
(`smaragd.exe --test-case ... -platform offscreen`). `--test-case` should
construct the project and run the script **without requiring the main window to
be shown**; if a window is needed for some action's side effects, create it but
do not `showMaximized()`. Audio backend init must be skippable in test mode (use
the Null backend) so cases run without a sound device — wire this through the
existing backend selection.

## 5. Assertion vocabulary

Assertions are intentionally a small, declarative set evaluated by
`SActionRunner` against the live `SProject` after the actions drain. They are
**not** `SAction`s (they don't mutate); they're a separate element family so the
action registry stays pure-mutation.

| Assertion element | Checks |
|-------------------|--------|
| `<assert-prop key= equals=>` | `SProject::prop(key)` equals value (reuses the JSON props dict). |
| `<assert-track-count equals=>` | number of tracks in the root mixer. |
| `<assert-track-volume trackIndex= equals= epsilon=>` | track volume within epsilon. |
| `<assert-clip-count trackIndex= equals=>` | clips on a track. |
| `<assert-project-matches file=>` | full `SProject` XML equals a golden fixture (snapshot test). |

`assert-project-matches` is the highest-leverage one: most cases reduce to "apply
these actions, then the project XML should equal this golden file," which also
gives a trivial **record/update** workflow (`--run-actions` + `writeFile` to
regenerate the golden). Start with `assert-track-count`,
`assert-project-matches`, and `verify-undo`; add the finer-grained asserts as
real cases need them.

## 6. Implementation phases

### Phase 0 — Prove the round-trip (foundation, ~½ day)
- Add `SActionScript::toXml()` / `readXml()` (the §4.1 bridge).
- Unit-style self-check (can live behind `--test-case` itself or a tiny test
  harness): for every name in `SActionRegistry::knownNames()`, construct via
  factory → `toXml` one action → `createFromXml` → `toXml` again → assert the two
  XML fragments are identical. **This is the first time `createFromXml` is
  exercised; expect to find actions whose `readXml` doesn't perfectly invert
  `writeXml` and fix them.** (Audit all ~27 registered verbs across
  `main/src/actions/`.)

### Phase 1 — Script load + execute (core, ~1 day)
- `SActionScript::readFile()` with setup handling (`new` / `load`).
- `SActionRunner::run()` submitting through `SActionHistory`.
- `--run-actions` flag in `main.cpp` (window stays open; no assertions yet).
- Port one existing manual test (`runGroupTrackTest`) to a `.qxa` fixture to
  validate the path produces identical behaviour.

### Phase 2 — Test-case mode + assertions (~1 day)
- Assertion vocabulary (§5), starting with track-count / project-matches /
  verify-undo.
- `--test-case` flag: headless-ish execution, Null audio backend, `PASS`/`FAIL`
  summary, process exit code.
- `--list-actions`.
- A `tests/cases/` directory of `.qxa` fixtures + goldens; a short doc on
  authoring them.

### Phase 3 — Ergonomics (optional / later)
- Line-based `.qxs` DSL front-end (§3.2).
- `expectReject` negative tests; per-step comments preserved on record.
- CI wiring (run all `tests/cases/*.qxa` under `-platform offscreen`).

## 7. Risks & open questions

1. **Untested serialization.** The biggest unknown: `createFromXml` and several
   `readXml` implementations have never run. Phase 0's exhaustive round-trip
   audit de-risks everything downstream and must come first.
2. **Actions needing UI/main-window context.** Some actions may depend on state
   the window owns (selection, current-time cursor). Audit which verbs are
   "pure project mutations" vs. UI-coupled; the first test cases should use the
   pure ones. UI-coupled actions may need their parameters made fully explicit in
   the script (no implicit "current selection").
3. **Audio device dependence.** `--test-case` must force the Null backend so CI
   has no sound-card requirement.
4. **Path resolution.** `setup file=` and `assert-project-matches file=` should
   resolve relative to the `.qxa` file's directory, not CWD, so fixtures are
   relocatable.
5. **Non-determinism.** Timestamps in recorded filenames (e.g. recording actions)
   and absolute paths must be normalised or excluded from golden comparisons.
6. **Phase-2 async drain.** Once `03a` lands engine-thread draining, the runner
   must wait for the queue to fully drain before evaluating assertions (a
   synchronisation/flush point), rather than assuming synchronous apply.

## 8. Out of scope

- A full scripting language (loops, variables, conditionals) — actions only.
- Mouse/keyboard event replay (this drives the *model*, not the widgets).
- Performance/timing assertions; this is functional correctness only.
