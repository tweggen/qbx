# Proposal 33: Native Plugin Editor GUIs (DRAFT)

Follows proposal 08 (plugin hosting). Closes the "Native plugin editor windows"
deferral listed in `plan/todo/08_PLUGIN_HOSTING_EXECUTION.md` §Known deferrals and
`smaragd/main/pluginui/CONTRACT.md` known debt.

## Context

Smaragd hosts CLAP, VST3 and AudioUnit effect plugins (proposal 08, M0–M8 done),
but the only editor UI is a **generic parameter-slider dialog** (`SPluginParamEditor`).
Native plugin editor windows are an explicit deferral in
`plan/todo/08_PLUGIN_HOSTING_EXECUTION.md` (§Known deferrals) and in
`smaragd/main/pluginui/CONTRACT.md` known debt.

Today the state is **detection-only**: every backend reports
`twPlugin::supportsNativeEditor()` but nothing creates, attaches, sizes or hosts a
view. There is no view interface on `twPlugin`, no `IPlugFrame`/`IRunLoop`, no
`clap_host_gui`, and no CocoaUI loader.

**Goal:** let a user double-click a plugin slot and see the plugin's *own* GUI,
embedded in a host window, resizable both ways, with edits made in the native UI
actually becoming audible.

**Scope decisions (confirmed with user):**
- **VST3 vertical slice first** (proves the pattern; most commercial plugins), then
  CLAP, then AudioUnit.
- **All three platforms**: Win11 (HWND), macOS (NSView), Linux (X11 — including a
  VST3 `IRunLoop` host object; Wayland is XWayland-only, native `IWaylandFrame`
  deferred).

## Design principle

**Host-parents-plugin.** The Qt app creates a native window (`QWidget` with
`Qt::WA_NativeWindow`), takes its native handle via `winId()` (HWND / `NSView*` /
X11 `Window`), and hands *that* to the plugin as its parent. The plugin renders
into our window; we size our container to its reported size and honor its resize
requests. This is the robust cross-toolkit route (JUCE/Ardour do the same) and
avoids the focus/reparent quirks of `QWindow::fromWinId + createWindowContainer`.

**Layering / ABI:** the native handle crosses app→engine as an **opaque `void*` +
enum** — no Qt type enters the engine, no `pluginterfaces/*` type leaves
`plugins/src/`. `check_layering.py:126` already allows `pluginui → tw/plugins`, so
no `APP_ENG` edit is needed.

---

## M1 — Format-neutral editor interface (engine)

New public header **`smaragd/tw303a/plugins/include/tw/plugins/twplugineditor.h`**,
containing **no** Steinberg/CLAP/AU types.

**Decision: a separate `twPluginEditor` object returned by a query, NOT new
virtuals on `twPlugin`.** Rationale: `twPlugin` stays narrow (the null placeholder
and every non-GUI plugin carry zero editor weight); the editor has an independent
lifetime, which makes `reloadPlugin()` clean (destroy editor → ask new instance for
a fresh one, never re-init in-place on a dangling `this`); and it keeps
`std::function` resize/edit plumbing and `void*` handles off the core ABI that
crosses into `app/objects/track`.

```cpp
namespace audio {
enum class twEditorPlatform { HWND, NSView, X11 };  // void* is HWND / NSView* / (Window)uintptr_t
struct twEditorSize { int width = 0, height = 0; };

class twPluginEditor {
public:
    virtual ~twPluginEditor() = default;                 // detaches
    virtual twEditorPlatform platform() const = 0;
    virtual bool attach( void *parent ) = 0;             // false => host falls back to sliders
    virtual void detach() = 0;                            // idempotent
    virtual twEditorSize size() const = 0;               // preferred size, valid post-attach
    virtual bool setScale( double s ) { (void)s; return false; }
    virtual bool canResize() const { return false; }
    virtual twEditorSize constrainSize( twEditorSize p ) { return p; }  // VST3 checkSizeConstraint
    virtual bool setSize( twEditorSize s ) { (void)s; return false; }   // host-driven (onSize)
    using ResizeCb = std::function<void(twEditorSize)>;
    virtual void setResizeCallback( ResizeCb cb ) = 0;   // plugin-initiated resize (UI thread)
    virtual bool pollEdited()  = 0;                       // main-thread drain of "plugin changed its own params"
    virtual bool pollRestart() { return false; }          // plugin asked for restart / param-list rescan
};
}
```

Add exactly one virtual to **`twplugin.h`** (forward-declare `class twPluginEditor;`,
keep the narrow include set):

```cpp
virtual std::unique_ptr<twPluginEditor> createEditor() { return nullptr; } // UI thread only
```

`supportsNativeEditor()` stays as the cheap probe the FX strip uses to decide
*which* editor to open. `pollEdited()` is a **poll**, not a push, because plugins
fire edits from their own UI thread — reaching Qt from there is the hazard
`twvst3host.h` already documents (atomic flag set by plugin thread, drained by main
thread).

---

## M2 — VST3 backend (the vertical slice)

New unit **`smaragd/tw303a/plugins/src/twvst3editor.{h,cc}`** implementing
`twPluginEditor` over a **retained** `IPlugView`. New host objects in
**`twvst3host.{h,cc}`**: `twVst3PlugFrame`, and Linux-only `twVst3RunLoop`.

1. **Retain the view.** Today `twvst3plugin.cc:362-365` does
   `createView(kEditor)→release` purely to set `hasGui_`. Add
   `twVst3Plugin::createEditor()` that retains the `IPlugView*` and wraps it in
   `twVst3Editor` (guard against a second editor per instance).
2. **`twVst3Editor`** maps the interface onto the view:
   - `attach(parent)`: `view_->setFrame(&frame_)` then
     `view_->attached(parent, platformType())`.
   - `platformType()`: `kPlatformTypeHWND` (Win) / `kPlatformTypeNSView` (mac) /
     `kPlatformTypeX11EmbedWindowID` (Linux); mirrored by `platform()`.
   - `size()`←`getSize`; `canResize()`←`canResize`;
     `constrainSize()`←`checkSizeConstraint`; `setSize()`←`onSize`;
     `setScale()`← `IPlugViewContentScaleSupport::setContentScaleFactor`.
   - `detach()`: `setFrame(nullptr); removed();` (idempotent).
   - `pollEdited()` forwards the existing `twVst3ComponentHandler` `edited_` flag
     (set in `performEdit`, `twvst3host.h:262`) — this is the Inv-6 wire.
3. **`twVst3PlugFrame`** (borrowed-refcount, like existing host objects):
   `resizeView(view, rect)` stashes size + invokes the `ResizeCb` on the main
   thread. On **Linux**, its `queryInterface` also returns the `IRunLoop*`.
4. **Linux `twVst3RunLoop`**: implements
   `register/unregisterEventHandler(IEventHandler*, fd)` and
   `register/unregisterTimer(ITimerHandler*, ms)` as a pure registry — it does
   **not** own a loop. The Qt bridge (M3) drives it. Comment that Wayland relies on
   XWayland + Qt xcb; native `iwaylandframe.h` is future work.
5. **IIDs in `twvst3iids.cc`** (the SDK ships none — a missing one is a link-time
   undefined symbol at first reference): `DEF_CLASS_IID` for `IPlugView`,
   `IPlugFrame`, `IPlugViewContentScaleSupport`, and Linux-guarded `Linux::IRunLoop`,
   `Linux::IEventHandler`, `Linux::ITimerHandler`. GUI headers
   (`pluginterfaces/gui/iplugview.h`, `iplugviewcontentscalesupport.h`) are already
   vendored/mirrored.

**Riskiest VST3 unknown:** the app is MinGW, plugins are MSVC DLLs. `vst3_probe`
already proved the COM vtable ABI for the *audio* interfaces; `IPlugView`/
`IPlugFrame` are the same shape, so risk is moderate — but a MinGW-Qt `winId()` HWND
handed to `attached()` on an MSVC plugin is the specific thing to smoke-test first.

---

## M3 — Qt host window (app/pluginui)

New module-private widget **`smaragd/main/pluginui/src/spluginnativeeditor.{h,cpp}`**
(`.cpp`, never `.mm`) owning the native container and the `twPluginEditor` lifetime.

- Reuse the slot-keyed pattern from `splugineffectstrip.cpp:349` (`ensureParamEditor`).
  Add a sibling map `QHash<SPluginSlot*, QPointer<QDialog>> nativeEditors_`.
- Inner container `QWidget` gets `setAttribute(Qt::WA_NativeWindow)`; take
  `container->winId()` (HWND / `reinterpret_cast<NSView*>(winId())` / X11 `Window`).
- `showEvent`: `editor->setResizeCallback(...)`, `editor->attach(handle)`, size
  container to `editor->size()`. `closeEvent`/dtor: `editor->detach()` **then**
  destroy the editor (Steinberg `removed()` must precede release).
- **Resize both ways:** plugin→host via `ResizeCb` (resize the dialog); host→plugin
  only if `canResize()` — intercept `resizeEvent`, run `constrainSize()` then
  `setSize()`.
- **Inv 6 (central hazard):** a `QTimer` (~30 ms) while open calls
  `editor->pollEdited()`; if true, resolve the slot and call
  `SPluginSlot::notifyPluginEdited()` (bumps epoch, invalidates upward, emits
  `paramsChanged`). `pollRestart()` → param re-read. **Documented gap:** a native
  edit bypasses `SSetPluginParamAction`, so it is **not undoable** — call this out in
  code comments and a new `pluginui/CONTRACT.md` invariant. The slider editor stays
  the undoable path.
- **Lifecycle (Inv 5/7), wired like the existing editor:** close on
  `slot::destroyed` (`splugineffectstrip.cpp:376`); on `SPluginSlot::pluginReloaded`
  (`:292`) `detach()` + destroy + recreate from the new `livePlugin()` (old
  `IPlugView` controller is now dangling); re-point on `rebuildUI()` (`:296-304`).
- **Branch point** in `ensureParamEditor`/`openParamEditor`: when
  `livePlugin()->supportsNativeEditor()` and the backend supports embedding, open the
  native window; if `createEditor()` returns null or `attach()` fails, **fall back to
  the slider editor**. Reach the instance via
  `slot->getProcessor()->plugin()` (bus-0 representative, matching
  `spluginparamereditor.cpp:59-66`).
- **Linux IRunLoop bridge lives here:** the widget builds `QSocketNotifier`s (per
  registered fd → `IEventHandler::onFDIsSet`) and `QTimer`s
  (→ `ITimerHandler::onTimer`) from the `twVst3RunLoop` registration list. The
  engine-side run-loop object stays Qt-free.

---

## M4 — CLAP (after VST3 proven)

- `twClapPlugin::createEditor()`: store `extGui_ = get_extension(CLAP_EXT_GUI)`
  (today `twclapplugin.cc:306` only tests presence).
- **Offer `clap_host_gui`:** `hostGetExtension` (`twclapplugin.cc:186-191`) returns
  nullptr for everything — add a `CLAP_EXT_GUI` branch returning a static
  `clap_host_gui_t` with `resize_hints_changed`, `request_resize` (atomic size+flag
  drained by the M3 timer), `request_show`, `request_hide`.
- `attach()`: `gui->create(plugin, api, /*floating=*/false)` with api =
  `CLAP_WINDOW_API_WIN32`/`COCOA`/`X11`, then `set_scale`/`get_size`/`can_resize`/
  `set_parent(clap_window{...handle})`/`show`. `detach()`: `hide(); destroy()`.
- Edit hazard: offer `clap_host_params` / act on `request_flush` (drop-today at
  `twclapplugin.cc:228`) to set the edited flag → same `pollEdited()` path.

## M5 — AudioUnit (macOS only, after CLAP)

- `twauplugin.cc` is plain C and cannot touch AppKit. Add a sibling Obj-C++ file
  **`smaragd/tw303a/plugins/src/twaupluginview.mm`** exposing a C-callable factory:
  read `kAudioUnitProperty_CocoaUI` (extend `readGui`, `twauplugin.cc:268-275`) →
  `AudioUnitCocoaViewInfo` → load the factory bundle → instantiate the `NSView` via
  `AUCocoaUIBase` → wrap in `twAuEditor : twPluginEditor` (`platform()==NSView`,
  `attach()` does `[(NSView*)parent addSubview:auView]`).
- Edit hazard: `AudioUnitAddPropertyListener` / parameter listener sets the atomic
  flag → `pollEdited()`.
- macOS-only compile; `platform()` always `NSView`.

---

## Build / layering / threading

- **CMake `smaragd/tw303a/CMakeLists.txt`:** add `twvst3editor.{h,cc}` to
  `tw_plugins` sources; VST3 GUI headers already mirrored (the `file(COPY … gui …)`
  step). Linux: `find_package(X11)` + link `X11` under the existing `SMARAGD_LINUX`
  block (next to `dl`). AU (M5): add `twaupluginview.mm` to the `TW_HAVE_AU` block,
  link `Cocoa`/`AppKit` (mirror `coreaudio_input.mm`). No new submodule.
- **Layering:** no `APP_ENG` edit (`pluginui → tw/plugins` already allowed,
  `check_layering.py:126`). Keep the AU `.mm` in the **engine** tree (its `.mm` is
  scanned) and the Qt native-editor widget as **`.cpp`** in the app tree (the app
  walk skips `.mm`). Verify no `app/*` header enters `twvst3editor` and no
  `pluginterfaces/*` enters any `plugins/include/` header.
- **Threading invariants** (write into `twplugineditor.h` + a new
  `pluginui/CONTRACT.md` invariant): every `twPluginEditor` method is UI/main-thread
  only; plugin-thread edit/restart signals are atomic flags drained by the host
  `QTimer` (never call Qt/graph from a plugin callback); `notifyPluginEdited()` is
  the mandatory Inv-6 wire and is a documented non-undoable gap; teardown order is
  `detach()` → release view → destroy editor, and recreate on `pluginReloaded`.

## Verification

Native windows are hard to fully automate; combine a headless seam with manual passes.

1. `python tools/check_layering.py` and `tools/check_logging.py` — clean.
2. **Headless gate (most valuable):** extend the in-repo fixture
   `plugins/tests/twtestvst3.cpp` with a minimal `IEditController::createView` /
   `IPlugView`. Add a testkit verb (mirroring the `editorSetParam` /
   `showWindow=false` trick at `splugineffectstrip.cpp:391`) that calls
   `createEditor()`, `attach()` into an **offscreen** `QWidget` (never shown), and
   asserts non-null + `size()>0` + that driving a param makes `pollEdited()` fire
   `notifyPluginEdited()`. Runs on CI.
3. `ctest -R "plugins_test|plugins_scan_test"` and the plugin qxa cases stay green
   (the editor path must not perturb the audio/serialization layers).
4. **Manual, per platform** (needs a real installed VST3, e.g. Surge XT / Vital):
   double-click a slot → native GUI appears embedded → move a knob in the native UI →
   hear the change (Inv-6) → resize both ways → `reloadPlugin` while open → close →
   remove slot while open. Specifically:
   - **Win11:** MinGW app ↔ MSVC-plugin HWND embed + resize (riskiest VST3 ABI point).
   - **macOS:** NSView-into-QWidget — verify focus/first-responder and Retina scale
     (classic failure: layer-backed AppKit view renders blank or steals focus).
   - **Linux/X11:** fd-driven `IRunLoop` repaint works (a plugin that repaints only on
     `onFDIsSet` freezes if the `QSocketNotifier` bridge is wrong); verify under
     XWayland too.

## Riskiest unknowns (ranked)

1. **Linux `IRunLoop` ↔ Qt** — no existing code exercises this; a wrong
   `QSocketNotifier` bridge = frozen plugin GUI.
2. **macOS NSView-in-QWidget** — layer-backed views inside Qt's native window can
   render blank / steal first responder.
3. **MinGW ↔ MSVC `IPlugView` vtable** — de-risked for audio by `vst3_probe`, but
   `attached(HWND)` across the toolchain split is unproven.
4. **Wayland** — XWayland-only by design; native `IWaylandFrame` deferred.

## Critical files

- `smaragd/tw303a/plugins/include/tw/plugins/twplugin.h` — add `createEditor()`, fwd-declare `twPluginEditor`
- **new** `smaragd/tw303a/plugins/include/tw/plugins/twplugineditor.h` — the interface
- **new** `smaragd/tw303a/plugins/src/twvst3editor.{h,cc}` — VST3 `twPluginEditor`
- `smaragd/tw303a/plugins/src/twvst3host.{h,cc}` — `twVst3PlugFrame`, Linux `twVst3RunLoop`
- `smaragd/tw303a/plugins/src/twvst3iids.cc` — IID defs for the GUI interfaces
- `smaragd/tw303a/plugins/src/twvst3plugin.cc:362` — retain view in `createEditor()`
- **new** `smaragd/main/pluginui/src/spluginnativeeditor.{h,cpp}` — Qt host window
- `smaragd/main/pluginui/src/splugineffectstrip.cpp:349` — native-editor branch + lifecycle
- `smaragd/main/objects/track/src/spluginslot.cpp` — `notifyPluginEdited()` is the Inv-6 endpoint
- M4/M5 new: `plugins/src/twclapplugin.cc` (clap_host_gui), `plugins/src/twaupluginview.mm`
- Build: `smaragd/tw303a/CMakeLists.txt`; docs: `smaragd/main/pluginui/CONTRACT.md`, `plan/todo/08_PLUGIN_HOSTING_EXECUTION.md`
