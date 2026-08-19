#pragma once

// Format-neutral native plugin editor ABI (proposal 33).
//
// A twPluginEditor is the plugin's OWN user interface — the thing a user means
// by "open the plugin". It is obtained from twPlugin::createEditor() and has an
// independent lifetime, so a plugin that has no GUI, and the null placeholder,
// carry no editor weight at all.
//
// INVARIANT (plugins/CONTRACT.md inv. 4): NO FORMAT TYPE MAY APPEAR IN THIS
// HEADER. No IPlugView, no clap_plugin_gui_t, no NSView, no HWND. This header
// is public — it is included by app/pluginui, which must never see a
// TW_HAVE_CLAP / TW_HAVE_VST3 / TW_HAVE_AU-dependent declaration or the two
// sides get different views of the same type (ODR/ABI skew). A native window
// crosses the boundary as an opaque void* tagged with a twEditorApi enumerator,
// and nothing here changes shape with a build flag.
//
// THREADING: every method on every type in this header is MAIN-THREAD ONLY.
// All three formats say so — CLAP annotates the whole clap_plugin_gui extension
// [main-thread], VST3's IPlugView is UI-thread, and an AU Cocoa view is AppKit.
// A plugin signals the host from its own thread; those signals are recorded as
// atomic state inside the backend and handed over only by poll(), which the
// host calls on the main thread. NOTHING in an implementation of this interface
// may call Qt, touch the component graph, or take a graph lock: the engine is
// deliberately not a QObject and a Qt call from a plugin's thread would make Qt
// adopt that thread (see CLAUDE.md, "No Qt on audio thread" — the same hazard,
// a different thread).

#include <cstdint>
#include <memory>
#include <vector>

namespace audio {

// ---------------------------------------------------------------------------
// Native window handles
// ---------------------------------------------------------------------------

// Which platform window system a void* handle belongs to. The value is
// deliberately NOT conditional on the build platform: a Windows build still
// names MacNSView, so this enum is one stable ABI everywhere and a mismatched
// handle is a rejected value rather than a renumbered enumerator.
enum class twEditorApi : std::uint8_t
{
    None = 0,
    Win32Hwnd,  // void* IS the HWND
    MacNSView,  // void* is an NSView*
    X11Window,  // void* is (void*)(uintptr_t)Window
};

// A parent window offered to a plugin. Never owns anything.
struct twEditorHandle
{
    twEditorApi api    = twEditorApi::None;
    void       *handle = nullptr;

    bool valid() const { return api != twEditorApi::None && handle != nullptr; }
};

// A size in PHYSICAL PIXELS. Always. This is the one rule that keeps high-DPI
// embedding honest, and it is a rule this ABI imposes rather than inherits:
// VST3 states (pluginterfaces/gui/iplugview.h:98-100) that IPlugView
// coordinates are LOGICAL units on macOS but PHYSICAL pixels on Windows and
// X11. A backend therefore converts on macOS and passes through elsewhere, so
// that the host has exactly one convention to reason about. The host divides by
// the widget's devicePixelRatio to get Qt's logical geometry — see
// pluginui/CONTRACT.md.
struct twEditorSize
{
    int width  = 0;
    int height = 0;

    bool valid() const { return width > 0 && height > 0; }
};

// ---------------------------------------------------------------------------
// Capabilities
// ---------------------------------------------------------------------------

// What this particular editor can do. Read after createEditor() and before
// attach(): the host picks embedded or floating from it, and decides whether to
// let the user drag the window edges.
struct twEditorCaps
{
    // Can be parented into a host window. Every VST3 and AU editor is; a CLAP
    // plugin may support only the floating form.
    bool embeddable = false;

    // Can run as its own top-level window that the plugin owns and draws.
    // CLAP only — VST3 and AU have no such concept, and the host supplies the
    // window there.
    bool floating = false;

    // Honours setSize(): the user may drag the host window's edges.
    bool resizable = false;

    // Honours setScale(): the host may tell it the monitor's scale factor.
    bool scalable = false;

    // X11 only. The plugin registers file descriptors and timers that the
    // HOST's event loop must service, or its GUI never repaints. The engine
    // keeps only a registry (see twEditorRunLoopSink); the app owns the loop.
    bool needsRunLoop = false;
};

// ---------------------------------------------------------------------------
// Plugin -> host feedback
// ---------------------------------------------------------------------------

// A parameter edit that ORIGINATED INSIDE THE PLUGIN'S OWN GUI, bracketed as a
// gesture. This is the payload that makes a native editor a first-class citizen
// rather than a hole in the model: the host turns Begin/End into an automation
// punch-in and each End into ONE undoable set-plugin-param, exactly as the
// app's own slider already does.
enum class twEditorGesture : std::uint8_t
{
    Begin,   // the user took hold of a control
    Change,  // a new value while held
    End,     // the user let go
};

struct twEditorParamEdit
{
    std::uint32_t   paramId = 0;
    double          value   = 0.0;  // the plugin's HOST-FACING domain, i.e. the
                                    // same domain twPlugin::setParam() takes
                                    // (normalized [0,1] for VST3).
    twEditorGesture phase   = twEditorGesture::Change;
};

// Everything the plugin has asked for since the previous poll(). Returned by
// value and consumed entirely; a backend clears its pending state as it fills
// this in, so two polls never report one request twice.
struct twEditorFeedback
{
    // The plugin resized itself and the host must resize its container to
    // match. Coalesced: only the LAST size since the previous poll survives,
    // because an intermediate size is never worth a repaint.
    bool         resized = false;
    twEditorSize newSize{};

    // The parameter list, the I/O layout or the latency changed and everything
    // the host cached about this plugin must be re-read. Rare and expensive.
    bool restartRequested = false;

    // The plugin asked to be hidden (CLAP request_hide, or a floating window's
    // own close button). The host closes its window; it does NOT destroy the
    // plugin.
    bool closeRequested = false;

    // Ordered, gesture-bracketed. May be empty. A Begin without an End is
    // legal and normal — the user is still holding the control.
    std::vector<twEditorParamEdit> edits;

    bool empty() const
    {
        return !resized && !restartRequested && !closeRequested && edits.empty();
    }
};

// ---------------------------------------------------------------------------
// X11 run-loop bridge
// ---------------------------------------------------------------------------

// X11 only, and the reason the Linux path is the riskiest part of proposal 33.
// A VST3 plugin on X11 does not get its own event loop; it hands the host file
// descriptors and timers through IRunLoop and expects to be called back. The
// ENGINE implements only the registry — it must stay Qt-free — and the APP
// bridges each registration to a QSocketNotifier or a QTimer and calls
// twPluginEditor::onFdReady() / onTimer() back. A plugin that repaints only on
// onFDIsSet freezes solid if this bridge is wrong, which is why it is an
// explicit interface rather than an implementation detail.
class twEditorRunLoopSink
{
public:
    virtual ~twEditorRunLoopSink() = default;

    virtual void addFd( int fd )                              = 0;
    virtual void removeFd( int fd )                           = 0;
    virtual void addTimer( std::uint64_t id, std::uint64_t ms ) = 0;
    virtual void removeTimer( std::uint64_t id )              = 0;
};

// ---------------------------------------------------------------------------
// The editor
// ---------------------------------------------------------------------------

class twPluginEditor
{
public:
    // MUST detach() if still attached. Main thread only, and it must run
    // BEFORE the twPlugin that produced it is destroyed — all three formats
    // require the view to die before the instance does.
    virtual ~twPluginEditor() = default;

    virtual twEditorCaps caps() const = 0;

    // Which handle type attach() wants on THIS platform. A host that cannot
    // produce it must fall back to the generic parameter editor.
    virtual twEditorApi api() const = 0;

    // Parent the plugin's UI into the host's window. False is not a hard
    // failure: the host falls back, in order, to attachFloating() and then to
    // the generic parameter editor. "This plugin's GUI would not embed" must
    // not cost the user the plugin.
    virtual bool attach( const twEditorHandle &parent ) = 0;

    // The D1 middle rung: let the plugin own a top-level window of its own.
    // Only ever called when caps().floating, which today means CLAP alone —
    // VST3 and AU have no floating concept and keep the default.
    //
    // The handle is a TRANSIENT PARENT, not a container: the plugin's window is
    // its own, and this is what keeps it above the app, minimising with it and
    // impossible to lose behind it (CLAP set_transient). Passing an invalid
    // handle is allowed and merely gives up that relationship.
    //
    // There is no host widget here, so a floating editor reports the user
    // closing it through twEditorFeedback::closeRequested — there is no
    // closeEvent to observe — and the host must not attempt setSize() on it.
    virtual bool attachFloating( const twEditorHandle & ) { return false; }

    // Idempotent, and safe to call on a never-attached editor.
    virtual void detach() = 0;

    // The plugin's preferred size. Only meaningful once attached.
    virtual twEditorSize size() const = 0;

    // Ask the plugin what it would round a proposed size to. The default
    // accepts anything, which is correct for a non-resizable editor because
    // the host will not offer it a different size in the first place.
    virtual twEditorSize constrain( twEditorSize want ) const { return want; }

    // Host-driven resize. Only called when caps().resizable.
    virtual bool setSize( twEditorSize ) { return false; }

    // Monitor scale factor (1.0, 1.5, 2.0 ...). Only called when
    // caps().scalable. Must be re-sent when the window changes monitor.
    virtual bool setScale( double ) { return false; }

    // Hand over everything the plugin has asked for since the last call, and
    // clear it. Main thread, called on a timer while the window is open.
    // MUST NOT block, allocate unboundedly, or re-enter the plugin's GUI.
    virtual twEditorFeedback poll() = 0;

    // X11 only; a no-op everywhere else. Set before attach().
    virtual void setRunLoopSink( twEditorRunLoopSink * ) {}
    virtual void onFdReady( int ) {}
    virtual void onTimer( std::uint64_t ) {}
};

}  // namespace audio
