# Building a Native Audio Plugin — Architecture Guide

> **Scope: this guide is not about smaragd.** It documents a *separate* project — the
> Mangrove compressor, built on IPlug2 + the VST3 SDK + Skia — kept here as a reusable
> template for authoring a plugin. Every path it names (`Source/DSP/compressor_chain.h`,
> `Source/Plugin/`, `external/iplug2`, `Tests/`) belongs to that project, not to this
> repository. Smaragd is a plugin **host**: it loads CLAP/VST3/AU plugins through
> `tw/plugins/twplugin.h` and uses no part of the stack described below. See
> `plan/proposed/08_PLUGIN_HOSTING.md` and `smaragd/tw303a/plugins/CONTRACT.md` for the
> hosting side.

**Purpose:** This document instructs a future developer (human or Claude) on how to
build a cross-platform audio plugin *in the same way the Mangrove project does*, but with
a **different DSP core** ("a different DSP heart"). It captures the technology stack, the
architectural pattern, and the exact integration contract — distilled from the Mangrove
compressor plugin so it can be reused as a template.

The single most important idea: **a portable, self-contained DSP core is kept strictly
separate from a thin plugin wrapper that adapts it to plugin formats and a GUI.** To make
a new plugin, you swap the DSP core and rewire a small, well-defined set of glue.

---

## 1. Technology Stack

| Layer | Choice | Why |
|-------|--------|-----|
| DSP core | Plain C++17, **zero dependencies**, header + cpp | Portable, testable in isolation, reusable across formats |
| Plugin framework | **[IPlug2](https://github.com/iPlug2/iPlug2)** | One codebase → VST 3, AU (v2/v3), etc.; handles host boilerplate, parameter system, state |
| Plugin format SDK | **[VST 3 SDK](https://github.com/steinbergmedia/vst3sdk)** (Steinberg) | The active target format; consumed by IPlug2's VST3 layer |
| GUI | **IPlug2 IGraphics** rendered with **Skia** on **Metal** (macOS) | Vector UI, GPU-accelerated; optional at build time |
| Build | **CMake ≥ 3.15** | Orchestrates DSP lib + IPlug2 + VST3 SDK + plugin bundle |
| Language std | **C++17** for DSP & wrapper; **C++20** required when Skia UI is enabled | Skia headers need C++20 |
| Platform (current) | macOS (Cocoa/Metal/CoreAudio frameworks); Objective-C++ (`.mm`) for platform glue | Windows/Linux are planned but wired the same way in IPlug2 |

Everything below the framework is vendored under `external/` (git submodules or checkouts):
`external/iplug2`, `external/vst3sdk`. Skia is built once into
`external/iplug2/Dependencies/Build/mac/lib`.

---

## 2. The Core Architectural Principle

```
        ┌──────────────────────────────────────────────┐
        │  DSP CORE  (the "heart" — swap this)          │
        │  Source/DSP/<your_core>.h/.cpp                │
        │                                               │
        │  • Pure C++17, no framework headers           │
        │  • Lock-free: params via std::atomic          │
        │  • float in / float out, block-based          │
        │  • init(sampleRate) + process(...) + setters  │
        │  • Unit-tested standalone (Tests/)            │
        └───────────────────┬───────────────────────────┘
                            │  (thin, mechanical glue)
        ┌───────────────────┴───────────────────────────┐
        │  WRAPPER  (IPlug2 — mostly reusable)          │
        │  Source/Plugin/                               │
        │  • config.h        → plugin identity/macros   │
        │  • <Plugin>.h/.cpp → IPlug2 Plugin subclass   │
        │  • <UI>.h/.cpp     → IGraphics layout         │
        └───────────────────┬───────────────────────────┘
                            │
        ┌───────────────────┴───────────────────────────┐
        │  FORMATS: VST3 / AU  (IPlug2 + SDK, build-only)│
        └────────────────────────────────────────────────┘
```

**Rule of thumb:** the DSP core must never `#include` an IPlug2 or VST3 header, and the
wrapper must never contain DSP math. The only coupling is the DSP class's public API,
called from four wrapper methods.

---

## 3. The DSP "Heart" Contract

Your new DSP core is a single class (see `Source/DSP/compressor_chain.h` as the reference
shape). To drop cleanly into the wrapper, it should provide:

```cpp
class MyDSPCore {                        // no framework includes here
public:
  MyDSPCore();
  ~MyDSPCore();

  // Called before first process() and on every sample-rate change.
  void init(float sampleRate);

  // Block-based, stereo, float I/O. In-place safe (out may alias in).
  void process(const float* inL, const float* inR,
               float* outL, float* outR, int numSamples);

  // One setter per parameter. Continuous params take float,
  // switches take bool. Setters are called from the UI/host thread.
  void setGain(float dB);
  void setSomething(float amount);
  void setEnableX(bool on);
  // ...

  // Optional: metering snapshot for the UI (lock-free).
  struct MeterData { float a, b, c; };
  MeterData getMeterData() const;

private:
  float _sampleRate = 44100.0f;

  // KEY PATTERN: every parameter is a std::atomic so the audio thread
  // reads without locks and the UI thread writes without blocking.
  std::atomic<float> _paramGain{0.0f};
  std::atomic<float> _paramEnableX{0.0f};   // bools stored as 0/1 float
  // ... internal DSP state (doubles for accumulators/filters) ...

  MyDSPCore(const MyDSPCore&) = delete;      // create once, never copy
  MyDSPCore& operator=(const MyDSPCore&) = delete;
};
```

Design invariants that made this core reusable (keep them):

- **Lock-free parameters.** Each parameter is `std::atomic<float>`; setters just store,
  `process()` just loads. No mutexes in the audio path. Booleans are stored as `0.0f/1.0f`.
- **Sample-rate driven coefficients.** All time constants / filter coeffs are recomputed
  in `init()` (and, if they depend on a param, when that param changes). No hardcoded
  44.1 kHz assumptions.
- **Zero external dependencies.** Only `<atomic>`, `<cmath>`, etc. This is what lets the
  core be unit-tested with no plugin host (see `Tests/dsp_tests.cpp`).
- **Deterministic, allocation-free `process()`.** No `new`/`malloc`, no locks, no I/O.
- **Helper DSP (e.g. IIR filters) can live as nested classes** inside the header
  (Mangrove's biquad high-pass is the nested `IIRFilterState` in `compressor_chain.h`) —
  keeps the core one self-contained translation unit.

---

## 4. The Wrapper — Plugin Identity (`config.h`)

`Source/Plugin/config.h` is pure `#define`s describing *this* plugin. For a new plugin,
change the identity fields and the parameter/UI counts. Critical fields:

```c
#define PLUG_NAME        "MyPlugin"
#define PLUG_MFR         "MyCompany"
#define PLUG_VERSION_HEX  0x00010000        // 1.0.0
#define PLUG_CLASS_NAME   MyPlugin

// 4-char OSType codes — MUST be unique per plugin (AU + some VST3 hosts key on these)
#define PLUG_UNIQUE_ID    'Myp1'
#define PLUG_MFR_ID       'MyCo'
// 32-char hex VST3 UID — MUST be unique; a collision makes a host load the wrong plugin
#define PLUG_UID_STR      "………32 hex chars………"

#define PLUG_CHANNEL_IO   "2-2"             // stereo in / stereo out
#define PLUG_LATENCY      0
#define PLUG_N_PARAMS     15                // <-- must equal kNumParams (see §5)
#define PLUG_N_PRESETS    1

#define PLUG_HAS_UI       1                 // 0 = host-generated UI only
#define PLUG_WIDTH        640
#define PLUG_HEIGHT       400
#define PLUG_FPS          60

// AUv2 entry points (needed only if you also build an AU)
#define AUV2_ENTRY …  #define AUV2_FACTORY …  #define AUV2_VIEW_CLASS …
```

> ⚠️ **Uniqueness matters.** Reusing another plugin's `PLUG_UID_STR` / 4-char codes is a
> real bug that surfaces as "the DAW loads the wrong plugin." Generate fresh values.

---

## 5. The Wrapper — Plugin Class

`Source/Plugin/MangrovePlugin.h/.cpp` is the reference. It is small and almost entirely
mechanical. Five pieces:

**(a) Parameter enum, kept in lockstep with the config count via a compile-time check:**

```cpp
enum EParams {
    kGain = 0, kSomething, kEnableX, /* … */ kNumParams
};
static_assert(kNumParams == PLUG_N_PARAMS, "Parameter count mismatch");
```

**(b) The class holds the DSP core by value plus fixed scratch buffers** (no per-block
allocation), and overrides four IPlug2 hooks:

```cpp
class MyPlugin final : public iplug::Plugin {
public:
    explicit MyPlugin(const iplug::InstanceInfo& info);
    void ProcessBlock(iplug::sample** in, iplug::sample** out, int nFrames) override;
    void OnReset() override;                 // sample-rate / (re)init
    void OnParamChange(int paramIdx) override;
    void OnIdle() override;                  // pull meters → UI (~UI rate)
private:
    MyDSPCore mCore;
    static constexpr int kMaxBlockSize = 8192;
    float mInL[kMaxBlockSize], mInR[kMaxBlockSize];
    float mOutL[kMaxBlockSize], mOutR[kMaxBlockSize];
};
```

**(c) Constructor: declare every parameter's range/units, then wire the UI:**

```cpp
GetParam(kGain)->InitDouble("Gain", 0., -24., 24., 0.01, "dB");   // default,min,max,step,unit
GetParam(kEnableX)->InitBool ("Enable X", false);
// … one Init* per param, in enum order …

#if IPLUG_EDITOR
    mMakeGraphicsFunc = [&]() {
        return MakeGraphics(*this, PLUG_WIDTH, PLUG_HEIGHT, PLUG_FPS,
                            GetScaleForScreen(PLUG_WIDTH, PLUG_HEIGHT));
    };
    mLayoutFunc = [&](IGraphics* g) { MyUI::Layout(*g, *this); };
#endif
```

**(d) `OnReset()` — (re)initialise the core at the host's sample rate, then push every
parameter down** (so the freshly-initialised core matches current param values):

```cpp
const double sr = GetSampleRate();
if (sr >= 8000. && sr <= 192000.) {          // guard against bogus/host-probe rates
    mCore.init(static_cast<float>(sr));
    for (int i = 0; i < kNumParams; ++i) OnParamChange(i);
}
```

**(e) `OnParamChange()` — a switch mapping param index → DSP setter.** Continuous params
cast the double value to float; booleans use a `>= 0.5` threshold:

```cpp
const double v = GetParam(p)->Value();
switch (p) {
    case kGain:     mCore.setGain(static_cast<float>(v));  break;
    case kEnableX:  mCore.setEnableX(v >= 0.5);            break;
    // …
}
```

**(f) `ProcessBlock()` — bridge IPlug2's `sample` (double) I/O to the core's float I/O**
through the scratch buffers, clamping to the buffer cap:

```cpp
const int n = std::min(nFrames, kMaxBlockSize);
for (int i = 0; i < n; ++i) { mInL[i]=(float)in[0][i]; mInR[i]=(float)in[1][i]; }
mCore.process(mInL, mInR, mOutL, mOutR, n);
for (int i = 0; i < n; ++i) { out[0][i]=(sample)mOutL[i]; out[1][i]=(sample)mOutR[i]; }
```

That is the *entire* audio-side integration. For a new DSP heart you rewrite only the
parameter list (enum + `Init*` + switch) and, if its I/O differs (mono, N-channel,
sidechain), the buffer bridging in `ProcessBlock`.

---

## 6. The Wrapper — GUI (IGraphics)

`Source/Plugin/MangroveUI.h/.cpp` is the reference. The UI is a single free function that
attaches controls; each control is bound to a parameter **by its enum index**, so IPlug2
handles the two-way sync automatically — you do not manually read/write the DSP core from
the UI.

```cpp
void MyUI::Layout(IGraphics& ui, MyPlugin& /*plugin*/) {
    // ⚠️ Load a font by FAMILY NAME (not a file path) exactly once.
    // Passing a bad/again-loaded font makes Skia crash when it measures text.
    static bool fontLoaded = false;
    if (!fontLoaded) { ui.LoadFont("default", "Helvetica", ETextStyle::Normal); fontLoaded = true; }

    ui.AttachPanelBackground(IColor(255, 40, 40, 40));
    const IText lbl(14, IColor(255,0,255,0), "default");

    ui.AttachControl(new IVKnobControl (IRECT(20,90,90,170), kGain));       // knob ↔ kGain
    ui.AttachControl(new IVToggleControl(IRECT(230,315,266,380), kEnableX, "X"));
    ui.AttachControl(new ITextControl  (IRECT(20,175,90,190), "Gain", lbl));
}
```

Common control types: `IVKnobControl` (continuous), `IVToggleControl` (bool),
`ITextControl` (static labels). Style via `IVStyle` / `IText` / `IColor`. Layout is
manual pixel `IRECT`s inside `PLUG_WIDTH × PLUG_HEIGHT`.

> The one-time `LoadFont` guard above is a hard-won fix for a Skia null-typeface crash
> (see the project memory note *Skia Font Initialization Crash Fix*). Keep it.

`OnIdle()` is where you'd pull `mCore.getMeterData()` and push values into meter controls
for real-time displays.

---

## 7. Build System (CMake)

Layout mirrors the architecture: the DSP core is its own library, the wrapper is a
plugin **bundle** (`MODULE`) that links the core + IPlug2 + the format SDK.

**Root `CMakeLists.txt`:**
- `project(... LANGUAGES CXX C OBJCXX)` — note **OBJCXX**, required for the macOS `.mm`
  platform glue.
- `CMAKE_CXX_STANDARD 17`; `CMAKE_OSX_DEPLOYMENT_TARGET 10.13`.
- `add_subdirectory(Source/DSP)` → builds the core as a static lib (`compressor_chain`).
- Conditionally `add_subdirectory` the VST3 SDK and `Source/Plugin` **only if** the
  vendored deps exist (so the DSP lib and tests still build without them).
- `enable_testing(); add_subdirectory(Tests)`.

**`Source/Plugin/CMakeLists.txt`** is the meaty one. Key ideas to replicate:
- Point variables at `external/iplug2` subtrees (IPlug, IGraphics, Dependencies, WDL,
  NanoVG/NanoSVG/STB include dirs).
- **Skia is optional and auto-detected.** `find_library` for the 8 Skia libs
  (`skia skottie skshaper sksg skparagraph skunicode_icu skunicode_core svg`). If found →
  `HAS_SKIA`, build with the UI; if not → build a DSP-only plugin. This keeps the plugin
  buildable before Skia is compiled.
- The plugin target is a bundle:
  ```cmake
  add_library(MyPlugin_VST3 MODULE ${PLUGIN_SOURCES})
  set_target_properties(MyPlugin_VST3 PROPERTIES
      BUNDLE TRUE BUNDLE_EXTENSION "vst3" OUTPUT_NAME "MyPlugin"
      MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.vst3")
  ```
- `PLUGIN_SOURCES` = your `Plugin.cpp` + the IPlug2 common `.cpp/.mm` + IPlug2's
  `VST3/IPlugVST3*.cpp` + three VST3 SDK glue files
  (`vstsinglecomponenteffect.cpp`, `pluginfactory.cpp`, `macmain.cpp`). When `HAS_SKIA`,
  append `MyUI.cpp` + the IGraphics core/controls/platform `.cpp/.mm` files.
- **Compile defs pick the mode:**
  - Skia UI: `VST3_API=1 IPLUG_EDITOR=1 IGRAPHICS_SKIA=1 IGRAPHICS_METAL=1` **and `CXX_STANDARD 20`**.
  - DSP-only: `VST3_API=1 IPLUG_EDITOR=0 NO_IGRAPHICS=1`.
- **macOS frameworks to link:** Cocoa, Metal, MetalKit, QuartzCore, CoreAudio,
  AudioToolbox, CoreMIDI, Accelerate, OpenGL, AppKit.
- Suppress warnings from vendored code (`-w` on the IPlug2 lib) but keep `-Wall -Wextra`
  on your own target.

> An AU (`.component`) target is scaffolded in the same file but currently guarded by
> `if(FALSE)` — enable it when you want AU. It reuses the same `Plugin.cpp` with
> `AU_API=1` and AU/Cocoa frameworks.

---

## 8. Recommended Directory Layout (replicate this)

```
<plugin-root>/
├── Source/
│   ├── DSP/            <your core>.h/.cpp + CMakeLists.txt   ← swap the heart here
│   └── Plugin/         config.h, <Plugin>.h/.cpp, <UI>.h/.cpp,
│                       Info.plist.vst3 / .au, CMakeLists.txt
├── Tests/             <core>_tests.cpp + CMakeLists.txt      ← standalone DSP tests
├── external/          iplug2/, vst3sdk/  (vendored)
├── CMakeLists.txt     root build
└── PLUGIN_TEMPLATE_GUIDE.md
```

(The Mangrove repo also keeps a legacy hand-rolled `Source/VST3/` wrapper from an earlier
phase — the `Source/Plugin/` IPlug2 path is the one to reuse.)

---

## 9. Step-by-Step: Adapting to a New DSP Heart

1. **Write/port the DSP core** in `Source/DSP/` following the §3 contract (atomic params,
   `init`/`process`/setters, no framework includes). Unit-test it in `Tests/` with no host.
2. **Enumerate parameters** in the plugin header (`EParams … kNumParams`) and set
   `PLUG_N_PARAMS` in `config.h` to match — the `static_assert` will catch drift.
3. **Set plugin identity** in `config.h`: name, mfr, **fresh** `PLUG_UNIQUE_ID` /
   `PLUG_MFR_ID` / `PLUG_UID_STR`, channel I/O, size.
4. **Fill the three glue points** in the plugin `.cpp`: `Init*` calls (ranges/units),
   the `OnParamChange` switch (index → setter), and `ProcessBlock` buffer bridging if I/O
   shape changed.
5. **Lay out the UI** in `<UI>.cpp` — one control per parameter bound by enum index; keep
   the one-time `LoadFont` guard.
6. **Update CMake** target/library names and source lists; keep the `HAS_SKIA` gate and
   framework list as-is.
7. **Build DSP-only first** (fastest path to a loadable plugin), then build Skia once and
   rebuild with the UI.

---

## 10. Gotchas & Lessons Learned (carry these forward)

- **Skia needs C++20** and a one-time `LoadFont(name, "Helvetica", …)` by family name —
  a file path or repeated load causes a null-typeface crash on text measurement.
- **`sample` is `double`, the core is `float`** — convert both ways through fixed scratch
  buffers; never allocate in `ProcessBlock`. Cap `nFrames` to `kMaxBlockSize`.
- **Guard `GetSampleRate()`** in `OnReset` (hosts probe with odd values); re-push all
  params after `init()` so the core state matches the host.
- **`static_assert(kNumParams == PLUG_N_PARAMS)`** is your seatbelt — it turns a whole
  class of silent parameter-count bugs into a compile error.
- **Unique IDs** (`PLUG_UID_STR`, 4-char codes) must be regenerated per plugin.
- **Keep the DSP core dependency-free** so it stays unit-testable and portable to future
  formats (AU v3, LADSPA) without change.
- **Make graphics optional at build time** (`HAS_SKIA`) so the audio engine is always
  buildable even before the (heavy) Skia toolchain is set up.

---

*Reference implementation: the Mangrove compressor repository (not this one — see the
scope note at the top). Start from `Source/DSP/compressor_chain.h` (the heart), then
`Source/Plugin/MangrovePlugin.cpp` (the glue), then `Source/Plugin/MangroveUI.cpp`
(the UI), then `Source/Plugin/CMakeLists.txt` (the build).*
