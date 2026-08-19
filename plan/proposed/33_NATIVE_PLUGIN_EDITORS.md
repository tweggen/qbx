# Proposal 33 v2: Native Plugin Editor GUIs

**Status: DRAFT — v2 supersedes the v1 draft in place.** v1 was written before
anything had been measured; v2 keeps its layering decision and its milestone
spine, replaces its risk ranking with numbers from a PoC that has now run, and
adds the half v1 missed entirely (§5).

Closes the "Native plugin editor windows" deferral in
`plan/todo/08_PLUGIN_HOSTING_EXECUTION.md` §Known deferrals,
`smaragd/main/pluginui/CONTRACT.md` known debt, and the "no native editor to
raise a gesture" caveat in `pluginui/CONTRACT.md` inv. 9.

---

## 1. What this is

Let a user open a plugin's **own** interface — the thing they mean by "open the
plugin" — embedded in a host window, for **instruments and effects alike**,
across **VST3, CLAP and AudioUnit** on **Windows 11, macOS and Linux/X11**.

Today the only editor is `SPluginParamEditor`: a scrolling column of generic
sliders on a fixed 1000-tick normalization. That is a reasonable fallback and a
poor primary. The measurement that settles how poor:

> **Dexed — a real, installed, third-party VST3 — exposes 2 238 parameters.**
> `vst3_probe` prints them. The generic editor renders that as 2 238 sliders in
> a `QScrollArea`, in plugin-declaration order, labelled with the plugin's own
> terse names (`MASTER TUNE ADJ`, `OSC KEY SYNC`). It is not a usable way to
> operate an FM synth, and no amount of polishing the slider list makes it one.

### 1.1 The state before this proposal

**Detection-only, and the detection is unread.** All three backends implement
`twPlugin::supportsNativeEditor()` — `twvst3plugin.cc:173`, `twclapplugin.cc:139`,
`twauplugin.cc:99` — and **nothing in the app has ever read it**. The single
reader in the whole tree is `clap_probe.cc:65`, printing `yes`/`no`.

The VST3 backend goes further and already creates a view, at
`twvst3plugin.cc:396-403`, purely to answer the bool — then releases it. There
is no view interface on `twPlugin`, no `IPlugFrame`, no `clap_host_gui`, no
CocoaUI loader, and no native-window code anywhere in `main/` (a repo-wide grep
for `WA_NativeWindow`, `createWindowContainer`, `QWindow::fromWinId` and
`winId()` returns **nothing**). This is greenfield.

---

## 2. THE OBVIOUS DESIGN IS WRONG, AND IT IS WRONG ABOUT WHICH HALF IS HARD

The obvious reading of this feature is *"put the plugin's window inside one of
ours."* That framing survives contact with reality for about a day — the PoC in
§4 did exactly that, against three real commercial plugins, and it worked on the
first run.

**A native editor is not a window feature. It is a PARAMETER-FLOW feature.**

When a user turns a knob in the plugin's own GUI, the plugin changes its own
state and tells the host *afterwards*. That is a model mutation entering the app
from outside the action system, and every mechanism this codebase relies on to
make a parameter change audible, undoable, automatable and correct under
multi-instance mapping is downstream of the action it just bypassed. Concretely,
in the tree as it stands today:

| # | Fact | Consequence for a naive implementation |
|---|---|---|
| 1 | **`twVst3ComponentHandler::performEdit(ParamID, ParamValue)` discards BOTH arguments** (`twvst3host.cc:304-311`; both parameters are unnamed) and sets a bare `edited_` flag | The host never learns *which* parameter moved or *to what* |
| 2 | **Nothing drains `edited_` or `restart_`.** `takeEditFlag()`/`takeRestartFlag()` (`twvst3host.h:263-264`) have **zero call sites** in the entire repo | Even the bare "something changed" signal is write-only dead state |
| 3 | **`IEditController::setParamNormalized` never reaches the DSP.** A VST3 parameter reaches audio only as `ProcessData::inputParameterChanges` (`plugins/CONTRACT.md:365-375`) | The knob moves on screen and **the sound does not change** |
| 4 | **CLAP offers no host GUI extension at all.** `hostGetExtension` (`twclapplugin.cc:305-320`) answers exactly three extensions and `nullptr` for everything else, including `clap.gui` | A CLAP plugin cannot request a resize, a show, a hide, or report a closed window |
| 5 | **CLAP's `request_flush` / `params_rescan` set atomics nobody reads** (`twclapplugin.cc:337`, `:350`) | Same hole as (2), in the second format |
| 6 | **A slot may hold N plugin INSTANCES.** `DualMono` instantiates one per channel (`twpluginslotproc.cc:484-491`); the editor can only attach to `instances_[0]` | Instance 0's GUI edits desync channels 1..N−1 — *the plugin sounds different on different channels* |
| 7 | **A parameter edit is inaudible without `SPluginSlot::notifyPluginEdited()`** (`pluginui/CONTRACT.md` inv. 6): it bumps the param epoch and emits `audioInvalidated()`, which `STrack` turns into `invalidateRenderPath()` | Frozen pages above the slot keep serving the old audio |

So the work is **≈20 % window plumbing and ≈80 % making a plugin's self-edit a
first-class citizen of the model.** v1 ranked its risks as Linux `IRunLoop` >
macOS NSView > MinGW/MSVC ABI, and named the parameter path nowhere. That
ranking is now inverted by measurement: the ABI risk is retired (§4), and the
parameter path is the whole proposal.

### 2.1 The upside of taking it seriously

Route the plugin's own gestures through the existing action system and three
things fall out for free, one of which is an open debt:

- **Native GUI edits become UNDOABLE** — v1 explicitly conceded this as "a
  documented non-undoable gap". It does not have to be one.
- **DualMono fan-out is automatic**, because `SSetPluginParamAction` already
  broadcasts to every instance (`ssetpluginparamaction.cpp:79-81`).
- **`pluginui/CONTRACT.md` inv. 9's punch-in debt closes.** It records that "a
  plugin's own `ParamGestureBegin/End` reaches the host only inside `process()`,
  on a worker thread, at freeze time … so the app's slider is the gesture."
  VST3 `beginEdit`/`endEdit` and CLAP `gui`-driven edits arrive **on the main
  thread, at gesture time** — which is exactly what `SAutomationRecorder` wants
  and has never been offered. **Read §4.1 before designing on that**: the phases
  are real and useful, but measurement shows they are not reliably drag
  boundaries, so they are a punch-in cue rather than an undo boundary.

---

## 3. Goals and non-goals

**Goals.** Open a plugin's own GUI, embedded, resizable where the plugin allows
it, HiDPI-correct, for instruments and effects, on all three platforms and all
three formats; make edits made inside it audible, undoable and automatable;
degrade to the generic slider editor whenever any of that is unavailable.

**Non-goals, named so they are not silently assumed:**

- **Out-of-process GUI hosting.** A plugin GUI that crashes takes the app with
  it. Bitwig-style sandboxing is a proposal of its own; scanning is already
  isolated (`smaragd_pluginprobe`), running is not, and this changes neither.
- **Plugin delay compensation.** Out of scope since proposal 37 P9 and still
  out of scope; every mount that shows a latency says so.
- **Native Wayland.** `iwaylandframe.h` is vendored and unused; Linux is
  X11/XWayland, stated rather than discovered.
- **A custom-drawn editor of our own** per plugin. The generic slider editor
  remains the fallback and is not being replaced.
- **VST2, LV2.** No backend exists for either.

---

## 4. PoC FINDINGS — measured, on this box, 2026-08-19

The v1 draft called the MinGW-host ↔ MSVC-plugin `IPlugView` seam its
"riskiest VST3 unknown … unproven". It is now proven. `vst3_probe` gained a
`--view` step (`plugins/tools/vst3_probe.cc`) that creates a real off-screen
`HWND` (`WS_POPUP`, never shown), walks
`createView → setFrame → attached → getSize/canResize/onSize → removed → release`,
and reports whether the plugin created a child window inside ours and whether it
called `IPlugFrame::resizeView` back into our vtable.

Build: Qt 6 bundled MinGW g++, x86_64. Plugins: MSVC-built, third-party,
installed on this machine.

| Plugin | Shape | Params | `attached()` | Child HWND | Size | `canResize` | ScaleSupport | `resizeView` |
|---|---|---|---|---|---|---|---|---|
| **Dexed** (instrument) | split ctrl | 2238 | **OK** | **yes** | 866×674 | no | yes | **1 ×** |
| **NassauEQ** (effect) | single comp | 22 | **OK** | **yes** | 720×340 | no | yes | 0 |
| **Mangrove** (effect) | single comp | — | **OK** | **yes** | 640×400 | no | yes | 0 |
| `twtestvst3` Gain / Sine | in-repo fixture | 1 / — | — | — | — | — | — | — |

**What this establishes:**

1. **The GUI ABI crosses the MinGW/MSVC boundary in both directions.** Not just
   a `kResultOk` return — a **real child `HWND` of the measured size exists
   inside our window** in all three cases. `attached()` returning OK and the GUI
   actually being embedded are different claims, and the probe separates them
   deliberately.
2. **The reverse leg works.** Dexed called `IPlugFrame::resizeView` **from
   inside `attached()`**, which our frame answered and forwarded to `onSize`.
3. **`setFrame` MUST precede `attached()`, and that is now a measured rule
   rather than a stylistic preference.** Dexed's only resize request arrives
   during `attached()`; with the frame installed afterwards it would have been
   silently dropped — no error, no log, just a plugin sized wrong forever.
4. **The negative control holds.** Both `twtestvst3` classes return `nullptr`
   from `createView`, so the "plugin has no editor → fall back to sliders" path
   is exercised by the fixture, and (per §9) will stay exercisable after the
   fixture grows a view on one class only.
5. **HiDPI is offered by everything.** All three answered
   `IPlugViewContentScaleSupport`; none refused `setContentScaleFactor(1.0)`.

### 4.1 The knob HAS now been turned (`--show`, Dexed, 2026-08-19)

The gap above was closed the same day. `vst3_probe --show` makes the host window
visible, sizes its client area to the plugin's reported size and runs a real
message loop; a human turned a knob. Two results, and the second one **changes a
design decision**.

**The GUI paints.** Dexed's complete interface — six operator panels, the
algorithm matrix, the keyboard — renders inside a window our MinGW build owns.
Embedding is real, not merely an `HWND` that exists.

**Gestures arrive, with id and value, and are bracketed:**

```
edit : beginEdit   id=1367747436
edit : performEdit id=1367747436 value=0.030303
edit : endEdit     id=1367747436
edit : beginEdit   id=1367747436
edit : performEdit id=1367747436 value=0.060606
edit : endEdit     id=1367747436
edit : beginEdit   id=1367747436
edit : performEdit id=1367747436 value=0.080808
```

So §2 fact (1) is confirmed from the outside: VST3 really does hand the host a
`ParamID` and a normalized value, and the pre-M2 backend really was throwing
exactly that away. The value domain is normalized `[0,1]`, matching
`set-plugin-param` and `param:` lanes with no conversion.

**But look at the SHAPE, because it is not the shape the design assumed.**
Every single value is wrapped in its **own complete** `begin → perform → end`.
One drag of one knob produced three separate gestures, not one gesture with
three values in it.

> **`beginEdit`/`endEdit` are NOT drag boundaries.** A host that turned one
> gesture into one undo entry — which is exactly what §2.1 and M5 proposed —
> would produce **one undo entry per mouse-move step**: the thirty-entries-a-
> second problem the design set out to avoid, arrived at by the mechanism
> chosen to avoid it.

The fix is already in the tree and needs no new machinery:
`SSetPluginParamAction` has `mergeKey()` / `mergeWith()`
(`ssetpluginparamaction.cpp:111-134`), which exist precisely to coalesce a
slider drag into one undo entry. **M5 must coalesce by `(slot, paramId)` through
that existing merge, and treat `begin`/`end` as HINTS — a punch-in cue and a
"the user let go" cue — never as the authoritative undo boundary.** The ABI
keeps carrying the phases, because a plugin that *does* span a drag (the ratio
tells us which) can then be honoured exactly; what changes is that the host may
not depend on it.

`--show` now reports the ratio and names which of the two shapes it saw, so this
is a measurement anyone can repeat per plugin rather than a fact about Dexed.

**Also measured:** `restartComponent flags=0x4` = `kParamValuesChanged`, sent
during setup. The pre-M2 backend's `(void)flags;` made that indistinguishable
from a `kReloadComponent`, which would have meant re-reading the whole plugin
instead of just its values. The probe now decodes the flags by name.

**What is STILL not established, and must not be read as established:**

- **Host-driven resize is untested against a real plugin** — all three report
  `canResize -> no`, so `checkSizeConstraint`/`onSize` ran only on the ones that
  declined. A resizable plugin (Surge XT, Vital) is needed and is not installed
  here.
- **One plugin's gesture shape is not every plugin's.** Dexed is JUCE-hosted and
  brackets per value; a plugin that spans a drag would report a ratio well above
  1 and is equally legal. The host must handle both, which is why §4.1's rule is
  "coalesce, and treat the phases as hints" rather than "ignore the phases".
- **Focus and keyboard are untested.** The GUI paints and takes the mouse;
  nothing has typed into a plugin, and nothing has checked that our shortcuts do
  not eat a keystroke the plugin wanted (or the reverse).
- **Audio was never heard to follow a knob.** The probe has no audio device: it
  proves the host is *told*, not that the sound changes. That closes only when
  M2's queue is routed to the DSP.
- **macOS and Linux are untouched.** The probe compiles the non-Windows branch
  to a stub that says so.
- One box, one compiler, three plugins, all built with the same vendor SDK
  version. That is a data point, not a survey.

---

## 5. The interface

New public header **`tw303a/plugins/include/tw/plugins/twplugineditor.h`**
— written, and in the branch. It contains **no** Steinberg, CLAP, AppKit or Qt
type, because `plugins/CONTRACT.md` inv. 4 forbids a public header whose shape
changes with `TW_HAVE_VST3` / `TW_HAVE_CLAP` / `TW_HAVE_AU` (ODR/ABI skew
between `tw_plugins` and its consumers).

**A separate `twPluginEditor` object, obtained by a query — not new virtuals on
`twPlugin`.** `twPlugin` stays narrow, as its own comment demands ("format-specific
behavior (native editor, note input) lives behind capability-queried extension
interfaces", `twplugin.h:25-26`); the null placeholder and every GUI-less plugin
carry zero editor weight; and the editor gets an independent lifetime, which is
what makes `reloadPlugin()` clean — destroy the editor, ask the *new* instance
for a fresh one, never re-initialise in place on a dangling `this`.

```cpp
enum class twEditorApi : std::uint8_t { None, Win32Hwnd, MacNSView, X11Window };
struct twEditorHandle { twEditorApi api; void *handle; bool valid() const; };
struct twEditorSize   { int width, height; bool valid() const; };

struct twEditorCaps { bool embeddable, floating, resizable, scalable, needsRunLoop; };

enum class twEditorGesture : std::uint8_t { Begin, Change, End };
struct twEditorParamEdit { std::uint32_t paramId; double value; twEditorGesture phase; };

struct twEditorFeedback {          // drained by poll(), cleared as it is filled
    bool resized;  twEditorSize newSize;   // coalesced: last size only
    bool restartRequested;                 // param list / IO / latency changed
    bool closeRequested;                   // plugin asked to be hidden
    std::vector<twEditorParamEdit> edits;  // ordered, gesture-bracketed
};

class twPluginEditor {
    virtual ~twPluginEditor();                 // MUST detach(); before the plugin dies
    virtual twEditorCaps caps() const = 0;
    virtual twEditorApi  api()  const = 0;
    virtual bool attach( const twEditorHandle &parent ) = 0;   // embedded
    virtual bool attachFloating( const twEditorHandle & );     // D1; transient parent
    virtual void detach() = 0;                                 // idempotent
    virtual twEditorSize size() const = 0;
    virtual twEditorSize constrain( twEditorSize ) const;
    virtual bool setSize( twEditorSize );
    virtual bool setScale( double );
    virtual twEditorFeedback poll() = 0;       // main thread, ~30 Hz, never blocks
    virtual void setRunLoopSink( twEditorRunLoopSink * );      // X11 only
    virtual void onFdReady( int );
    virtual void onTimer( std::uint64_t );
};
```

plus exactly one defaulted virtual on `twPlugin` (forward-declaring the class,
keeping the narrow include set):

```cpp
virtual std::unique_ptr<twPluginEditor> createEditor() { return nullptr; }  // UI thread
```

`supportsNativeEditor()` stays as the cheap probe the FX strip uses to decide
*which* editor to open, so no existing caller changes.

### 5.1 Three decisions inside that header worth defending

**`twEditorSize` is always PHYSICAL PIXELS.** Imposed by this ABI, not
inherited. The VST3 SDK states the asymmetry outright
(`pluginterfaces/gui/iplugview.h:98-100`):

> on macOS (`kPlatformTypeNSView`), the coordinates are expressed in **logical
> units** …, whereas on Windows (`kPlatformTypeHWND`) and Linux
> (`kPlatformTypeX11EmbedWindowID`), the coordinates are expressed in **physical
> units (pixels)**.

A host that passes both through unconverted embeds correctly at 100 % on every
platform and is wrong by the scale factor on a Retina Mac — the classic
embedding bug, and one that never appears on the developer's own monitor. One
rule, converted in the macOS backend, divided by `devicePixelRatio` once in the
Qt widget.

**Feedback is POLLED, never pushed.** Plugins fire edits from their own UI
thread, and this codebase's hardest-won rule is that a non-Qt thread must never
reach Qt — Qt adopts the thread and deadlocks the join at teardown (CLAUDE.md,
"No Qt on audio thread"; `twclapplugin.cc:273-276` states the engine-side twin).
The backend records; a main-thread `QTimer` drains.

**Gestures are in the ABI, not just values.** `Begin`/`Change`/`End` is what
lets one gesture become one undo entry and one automation punch-in. A
value-only stream could not distinguish a drag from thirty separate edits.

---

## 6. Threading and lifetime

Every method on every type in the header is **main-thread only**, and all three
formats independently require it (CLAP annotates `clap_plugin_gui`
`[main-thread]`; VST3's `IPlugView` is UI-thread; an AU Cocoa view is AppKit).

**There is no main-thread guard in this engine.** `twRtThreadGuard`
(`tw/graph/tw_freeze_context.h:74-148`) distinguishes `Rt` / `Live` / none — an
ordinary worker and the Qt main thread are both `Kind::None`. M1 adds
`Kind::Main` + `markMainThread()` / `onMainThread()` to that existing class
rather than introducing a second guard that can drift from it (its own comment:
"Hence one policy per thread and ONE check"), keeping the POD-`thread_local`
discipline that MinGW's heap corruption bug forces. Editor entry points assert
on it and bump a process-wide refusal counter, mirroring
`twPluginSlotProcessor::liveOwnedRefusals()`.

**Teardown order is the sharp edge**, and it differs per format only in the
detail. Universally: `detach()` → release the view → destroy the editor →
*then* the plugin may die.

- **VST3**: the view must be `removed()` and released **before**
  `controller_->setComponentHandler(nullptr)` at `twvst3plugin.cc:433`, i.e. a
  new step at the very top of `teardown()`. `module_.reset()` at `:448` unloads
  the DSO; a leaked view is a use-after-unload.
- **CLAP**: `gui->destroy()` must precede `plugin_->destroy()`
  (`twclapplugin.cc:598-607`) **and must be on the main thread** — while
  `~twClapPlugin` today may run from whichever thread rebuilds the slot
  (`twpluginslotproc.cc:394` clears `instances_` under `mutex_`). **This is the
  single biggest lifetime hazard in the proposal**: the app must guarantee the
  editor is gone before any rebuild can start, and M1's main-thread assert is
  what makes a violation loud instead of a rare crash on a user's machine.
- **AU**: the Cocoa view must be released before `AudioComponentInstanceDispose`
  (`twauplugin.cc:228-237`).

**Instance rebuilds invalidate an open editor.** `setFactory()` /
`setChannelCount()` destroy and recreate every `twPlugin` (`plugins/CONTRACT.md`
inv. 18). The app closes the native window on `SPluginSlot::pluginReloaded` and
re-creates it from the new instance, exactly as `SPluginParamEditor::onPluginReloaded`
already rebuilds its sliders.

---

## 7. Format mapping

One column per format; the interface is the same in each.

| `twPluginEditor` | VST3 | CLAP | AudioUnit |
|---|---|---|---|
| `createEditor()` | `controller_->createView(kEditor)`, **retained** (today `twvst3plugin.cc:400` creates and releases) | cache `extGui_ = get_extension(CLAP_EXT_GUI)` (today `twclapplugin.cc:564` tests presence and drops the pointer) | `kAudioUnitProperty_CocoaUI` → `AudioUnitCocoaViewInfo` → factory bundle → `AUCocoaUIBase` (today `twauplugin.cc:404-411` only calls `GetPropertyInfo`) |
| `attach(parent)` | `setFrame(&frame_)` **then** `attached(h, kPlatformType*)` — order measured, §4.3 | `create(p, api, floating=false)` → `set_scale` → `set_parent` → `show` | `[(NSView*)parent addSubview:auView]` |
| `detach()` | `setFrame(nullptr); removed();` | `hide(); destroy();` | `[auView removeFromSuperview]` |
| `size()` | `getSize` (physical px) | `get_size` | `[view frame]` × scale → physical |
| `resizable` / `setSize` | `canResize` / `checkSizeConstraint` + `onSize` | `can_resize` / `adjust_size` + `set_size` | `NSView` autoresize |
| `scalable` / `setScale` | `IPlugViewContentScaleSupport` | `gui->set_scale` | n/a (AppKit handles it) |
| `floating` | never | `create(..., floating=true)` — the only format with the concept | never |
| **edits → `poll()`** | `IComponentHandler::begin/perform/endEdit` — **must start carrying `(id,value)`** | `clap_host_params::rescan` + a `clap.gui` host ext | `AudioUnitAddPropertyListener` / parameter listener |
| **resize ← plugin** | `IPlugFrame::resizeView` | `clap_host_gui::request_resize` | view frame notification |
| run loop | X11 `Linux::IRunLoop` | X11 fd/timer via host ext | n/a |

**CLAP's host-side extension must be implemented before it is advertised.**
`twclapplugin.cc:307-309` states the rule the backend already lives by:
"Claiming an extension we answer with nothing useful is how a plugin ends up in
a state the host never leaves." `clap.gui` gets added to `hostGetExtension` in
the same commit that implements all four of its callbacks, never before. The
same discipline applies to VST3's `isPlugInterfaceSupported` whitelist
(`twvst3host.cc:224-246`), which must not name `IPlugFrame` until the frame
exists — some plugins branch on that answer during `initialize()`.

---

## 8. Platform mapping

| | Windows 11 | macOS | Linux / X11 |
|---|---|---|---|
| Handle | `HWND` from `QWidget::winId()` | `NSView*` from `winId()` | X11 `Window` from `winId()` |
| Container | `QWidget` + `Qt::WA_NativeWindow` | same | same |
| Size units | physical px | **logical** → converted in backend | physical px |
| Run loop | Qt pumps; nothing extra | Qt pumps; nothing extra | **`IRunLoop` bridge required** |
| Risk | **retired by §4** | untested; layer-backed views can render blank or steal first responder | untested; highest remaining risk |
| Wayland | — | — | XWayland only, by design |

**Host-parents-plugin**, via `QWidget` with `Qt::WA_NativeWindow` and `winId()`
— not `QWindow::fromWinId` + `createWindowContainer`, which has focus and
reparenting quirks. JUCE and Ardour both do it this way, and §4 confirms it
works.

**The Linux run loop is now the riskiest thing in the proposal.** A VST3 plugin
on X11 gets no event loop of its own; it registers fds and timers through
`IRunLoop` and expects callbacks. The engine keeps a **registry only**
(`twEditorRunLoopSink`, already in the header) and stays Qt-free; the app
bridges each registration to a `QSocketNotifier` or `QTimer`. A plugin that
repaints only on `onFDIsSet` freezes solid if that bridge is wrong, and nothing
in this repo has ever exercised it.

---

## 9. Milestones

Ordered so the parameter path (§2, the hard half) is proven on the format the
PoC already de-risked, before any second format or platform is attempted.

| M | What | Gate |
|---|---|---|
| **M0** | ✅ **DONE** — `vst3_probe --view`, GUI IIDs in `twvst3iids.cc`, §4's numbers | 3/3 third-party plugins embed |
| **M1** | ✅ **DONE** — `twplugineditor.h`, `twPlugin::createEditor()`, `twRtThreadGuard::Kind::Main` + `markMainThread()` from `SApplication`'s ctor | build clean, both static gates clean, 21/21 plugin + live cases green |
| **M2** | ✅ **DONE** — `performEdit` carries `(id,value)`, `begin/endEdit` bracket, a drained queue, `restartComponent` flags kept, `applyGuiEdit()` → mirror + DSP ring | `--production --show` reads each edit back through `getParam()`; §4.1 |
| **M3** | ✅ **DONE** — `twVst3Editor` + `twVst3PlugFrame` behind the M1 ABI, `createEditor()` wired | `vst3_probe --production`: 3/3 real plugins attach and tear down clean, fixture correctly yields null |
| **M4** | The Qt host window (`main/pluginui/src/spluginnativeeditor.{h,cpp}`), lifetime, fallback, **§10's ownership fix**, D2 persistence | qxa case; the strip's track-switch behaviour; `editorOpen` round-trips and opens no window headlessly |
| **M5** | Edits → `SSetPluginParamAction` **coalesced by (slot,paramId) via the existing `mergeWith()`** (§4.1 — the phases are hints, not undo boundaries) + `SAutomationRecorder` punch-in; DualMono fan-out | ONE undo entry for a whole knob drag, measured against a plugin that brackets per value; `pluginui/CONTRACT.md` inv. 9 rewritten |
| **M6** | CLAP: `extGui_`, the `clap.gui` host extension (all four callbacks), **D1 floating fallback + `set_transient`** | `twtestclap` gains a GUI entry point |
| **M7** | macOS: `twaupluginview.mm` (AU) + NSView for VST3/CLAP; the logical→physical conversion | manual, on a Retina Mac |
| **M8** | Linux/X11: `IRunLoop` + the `QSocketNotifier`/`QTimer` bridge | manual, incl. under XWayland |

M0–M3 are in the branch. **Everything from M4 — the Qt window, and therefore
anything a user can click — is unwritten.**

`vst3_probe --production` is what gates M2 and M3, and it exists because the
rest of the probe deliberately hand-rolls raw COM: that proves the PLUGIN works,
not that `twVst3Editor` does. The production mode drives
`createVst3Plugin → twPlugin::createEditor → attach/poll/detach` instead, so the
class the app will use is the class under test. Measured, all four cases:

| Module | params | caps (embed/float/resize/scale) | 2nd editor | attach | teardown |
|---|---|---|---|---|---|
| Dexed | 2238 | 1 / 0 / 0 / 1 | refused | 866×674 | clean |
| NassauEQ | 22 | 1 / 0 / 0 / 1 | refused | 720×340 | clean |
| Mangrove | 16 | 1 / 0 / 0 / 1 | refused | 640×400 | clean |
| `twtestvst3` | 1 | — | — | — | `createEditor() → null` |

The fixture row is the one that matters most: `supportsNativeEditor()` is false,
`createEditor()` returns null, and that is the branch the FX strip will take to
fall back to the generic slider editor. It is exercised on every run.

**One M1 finding worth keeping**, because the opposite was tried first and looks
right: `twPlugin` must **include** `twplugineditor.h`, not forward-declare
`twPluginEditor`. `createEditor()` returns a `std::unique_ptr` with an inline
`return nullptr;` default, and destroying that temporary instantiates
`default_delete`, which needs a complete type — the forward declaration breaks
eight translation units, none of which mention editors. The include turned out
to cost nothing at all: `twplugineditor.h`'s three system includes are a strict
subset of the ones `twplugin.h` already had, and inv. 4 keeps every format type
out of it. There was no narrowness to protect.

**M4 and M5 land together.** D3 removed the slider list as an alternative entry
point, so between a shipped M4 and a shipped M5 there would be no undoable way
to change a parameter on any plugin that has its own GUI. They may be developed
as two milestones; they may not be merged as two.

---

## 10. The app half, and a constraint v1 missed

**`STrackDetailPanel::rebuildUI()` deletes the whole FX strip on every track
switch** (`strackdetailpanel.cpp:162`, `delete pluginStrip_;`), and
`SPluginEffectStrip::ensureParamEditor` parents its editor dialogs to the strip
(`splugineffectstrip.cpp:509`, `new QDialog(this)`). So **every open editor
window is destroyed when the user selects another track.**

That is tolerable for a slider list. It is not tolerable for a native GUI: every
DAW keeps plugin windows open across selection changes, and a user who loses
their synth's editor by clicking a different lane will read it as a crash.
Native editor windows therefore need an owner that outlives the strip — the
shell — with the strip merely *requesting* one. The registry keeps
`splugineffectstrip.cpp`'s proven shape (keyed by `SPluginSlot*`, `QPointer`,
`WA_DeleteOnClose`, closed on `slot::destroyed`, re-pointed on `rebuildUI()`)
and simply moves up a layer.

Two more app-side rules, both from existing precedent:

- **Never create a native window eagerly.** The testkit builds a **parentless,
  never-shown** strip (`spluginuitestactions.cpp:39-44`), and every qxa case
  runs under `QT_QPA_PLATFORM=offscreen`. Follow the existing
  `showWindow=false` split (`splugineffectstrip.cpp:539-546`) or the suite starts
  opening plugin GUIs.
- **The model layer must not grow a widget factory.**
  `SPluginSlot::getDetailEditWidget()` deliberately returns nullptr
  (`spluginslot.cpp:263-273`) because `objects/track` is `app_objects` and
  `pluginui` is `app_ui`. The slot exposes the plugin; the window lives in
  `pluginui`. `check_layering.py:269` already permits `pluginui → tw/plugins`,
  so **no layering edit is needed.**

---

## 11. Verification

Native windows resist automation, so the strategy is a real headless seam plus
an honest manual runbook — the shape `docs/ASIO_WINDOWS_GATE.md` and
`docs/MEDIA_BROWSER_MANUAL_GATE.md` already established.

**Headless (CI-able, and the valuable part).** Extend `twtestvst3.cpp` with a
minimal `TestView : IPlugView` on **`TestGain` only** (`:414`), leaving
`TestSineController::createView` (`:605`) returning `nullptr`. That asymmetry is
deliberate: one plugin with an editor and one without, single-component and
split, covering both branches. The fixture needs `DEF_CLASS_IID(IPlugView)` in
its **own** block (`:56-68`) — a module and its host do not share IID
definitions. A view that records `attached()` and succeeds **without creating a
real window** keeps the gate runnable where there is no display. Add a testkit
verb that opens the editor into an offscreen `QWidget`, asserts non-null +
`size() > 0`, drives a parameter from the fixture and asserts that `poll()`
reports it and that `notifyPluginEdited()` fired.

**Existing gates that must stay green**, unchanged: `plugins_test`,
`plugins_scan_test`, every `plugin_*.qxa` case, and both render goldens. This
feature must not perturb one byte of audio — an editor that is never opened must
cost nothing.

**Manual, per platform** (needs a real installed plugin, and for resize
specifically one that *is* resizable — Surge XT or Vital; nothing installed here
qualifies): open → knob moves audio → **undo restores it** → resize both ways →
`reloadPlugin` while open → switch tracks while open (§10) → remove the slot
while open → close.

**What will NOT be gated, stated up front:** pixels of any plugin's GUI; focus
and keyboard routing; real-plugin resize behaviour; crash isolation (there is
none); macOS Retina scaling and Linux `IRunLoop` (manual only, and per §4 both
platforms are currently at zero coverage); and any claim that N plugins working
implies N+1 will.

---

## 12. Critical files

| File | Change |
|---|---|
| `tw303a/plugins/include/tw/plugins/twplugineditor.h` | **new — written** |
| `tw303a/plugins/include/tw/plugins/twplugin.h` | `+createEditor()`, fwd-decl |
| `tw303a/plugins/src/twvst3iids.cc` | **done** — `IPlugView`/`IPlugFrame`/`IPlugViewContentScaleSupport` (root `Steinberg` ns, **not** `Vst::`) |
| `tw303a/plugins/tools/vst3_probe.cc` | **done** — `--view` |
| `tw303a/plugins/src/twvst3host.{h,cc}` | `twVst3PlugFrame`; `performEdit` must carry `(id,value)`; `IPlugFrame` into the support whitelist |
| `tw303a/plugins/src/twvst3editor.{h,cc}` | new — VST3 backend |
| `tw303a/plugins/src/twvst3plugin.cc` | retain the view; release it at the top of `teardown()` |
| `tw303a/plugins/src/twclapplugin.cc` | `extGui_`; `clap.gui` host ext; main-thread `gui->destroy` before `plugin_->destroy` |
| `tw303a/plugins/src/twaupluginview.mm` | new — macOS only |
| `tw303a/graph/include/tw/graph/tw_freeze_context.h` | `Kind::Main` |
| `main/pluginui/src/spluginnativeeditor.{h,cpp}` | new — Qt host window |
| `main/pluginui/src/splugineffectstrip.cpp` | native branch + fallback |
| `main/shell/` | the window registry (§10) |
| `tw303a/CMakeLists.txt` | **no change for VST3 GUI** — `gui` is already mirrored at `:314`. AU needs `.mm` + AppKit; Linux needs X11 |

---

## 13. Decisions (requester, 2026-08-19)

The three open questions are settled. Each carries a consequence worth stating
next to it, and the third one moves a milestone.

### D1 — Offer a FLOATING window when embedding fails

The fallback order is **embedded → floating → generic sliders**, and the middle
rung is CLAP-only: `clap_plugin_gui::create(p, api, is_floating)` is the only
place in any of the three formats where a plugin owns its own top-level window.
VST3 and AU have no such concept, so for those two the order stays
embedded → sliders. `twEditorCaps::floating` already exists in the ABI for
exactly this.

Three asymmetries a floating window brings, none of them optional:

- **The host does not own the window**, so there is no `QDialog` to close, no
  `closeEvent`, and no Qt geometry. Closing is `hide()` + `destroy()`, and the
  user closing it arrives as `clap_host_gui::closed` → `twEditorFeedback::
  closeRequested` — which is why that field is in the ABI rather than being
  inferred from a widget.
- **`set_transient` is mandatory, not decorative.** Without it the plugin's
  window does not stay above the app, does not minimise with it, and can be
  lost behind the main window with no way back. The host passes its own
  top-level handle.
- **Sizing is the plugin's business.** `caps().resizable` still reports what the
  plugin says, but the host must not try to drive it: there is no container to
  resize.

The registry in §10 still tracks a floating editor and still destroys it on
`slot::destroyed` and on `pluginReloaded`. What changes is only that teardown
runs through the editor rather than through a widget.

### D2 — Persist the editor across sessions, SPLIT by what actually travels

Persisted, and deliberately in two different places, because the two halves have
different portability:

| What | Where | Why |
|---|---|---|
| **Whether the editor was open** | the **project** (`<SPluginSlot editorOpen='true'>`) | "this project opens with the synth's editor up" is a property of the arrangement and is meaningful on any machine |
| **Window position and size** | **`SSettings`, per user, keyed by plugin uid** | a window at x=2400 is off screen on a laptop; monitor layout is machine-local and must not travel in a `.qxp` |

This is the same split the tree already makes for `midiOutPort` (proposal 37
P7): the portable NAME goes in the project, the machine-local id goes in
`SSettings` under `midi/portId/<name>`. Same shape, same reason. It is also the
lesson proposal 38 paid for in T11 — an app-config path serialised into a `.qxp`
means nothing on the next machine.

Two failure modes that must be handled rather than discovered:

- **A restored geometry may be entirely off screen** (monitor unplugged since
  the last session). Clamp onto an available `QScreen` before showing; never
  restore a position blind.
- **Restoring `editorOpen` must not open a window in a headless run.** Every
  qxa case runs under `QT_QPA_PLATFORM=offscreen`, and the testkit builds a
  never-shown strip (`spluginuitestactions.cpp:39-44`). Restoration is
  suppressed under `--test-case`, in the same place and for the same reason
  §10's "never create a native window eagerly" rule already applies. A case that
  wants to assert the flag round-trips reads the model, not a window.

`editorOpen` is written only when true, keeping every existing project file and
both render goldens byte-unchanged — the discipline `SObject::serialize()`
already follows for automation lanes.

### D3 — The generic slider editor is NOT reachable when a native GUI exists

The Edit button opens the native editor when there is one and the slider list
otherwise. No second entry point, no right-click alternative.

**This makes M5 a hard prerequisite for shipping M4, not a follow-up**, and that
is the one scheduling consequence of these three answers. The reasoning is
short: until M5 routes plugin gestures through `SSetPluginParamAction`, a native
edit is not undoable and not automatable. While the slider list stays reachable
that is survivable, because the undo-safe path still exists next to it. Remove
it, and for any plugin with a native GUI there is **no undo-safe path at all** —
the user's only way to change a parameter is one that silently cannot be undone.
M4 and M5 therefore land together or not at all; the milestone table below is
updated to say so.

Accepted cost, named: **a parameter the plugin's own GUI does not expose becomes
unreachable.** Some plugins hide parameters from their editor while still
declaring them. Automation is unaffected — a `param:` lane is created from the
arranger and names itself through `SPluginSlot::paramRows()`, which reads the
plugin's declared list rather than its GUI.
