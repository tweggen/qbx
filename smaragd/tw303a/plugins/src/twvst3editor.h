#pragma once

// VST3 native editor (proposal 33 M3), behind the format-neutral twPluginEditor.
//
// PRIVATE to tw_plugins and living in plugins/src/ for the reason
// plugins/CONTRACT.md inv. 4 gives: this header names Steinberg types, and a
// public header whose declarations changed with TW_HAVE_VST3 would give
// tw_plugins and its consumers different views of the same type. Everything the
// app sees is tw/plugins/twplugineditor.h, which names none of them.
//
// THREADING: main thread only, like every twPluginEditor. See that header.

#include "tw/plugins/twplugineditor.h"

#include "twvst3host.h"

#include "pluginterfaces/gui/iplugview.h"

#include <functional>

namespace audio {

// The host side of the resize protocol. It lives HERE rather than beside the
// other host objects in twvst3host.h because it is the only one whose lifetime
// is the EDITOR's: host_ and handler_ are per plugin instance and outlive every
// call into the plugin, whereas a frame exists exactly as long as the view it
// was handed to. Borrowed refcount, like its siblings — the plugin never owns it.
class twVst3PlugFrame final : public twVst3Borrowed<Steinberg::IPlugFrame> {
public:
    Steinberg::tresult PLUGIN_API queryInterface( const Steinberg::TUID _iid,
                                                  void **obj ) override;

    Steinberg::tresult PLUGIN_API resizeView( Steinberg::IPlugView *view,
                                              Steinberg::ViewRect *rect ) override;

    // Drained by twVst3Editor::poll(). A plugin may ask several times between
    // two polls; only the LAST size is worth anything, so they coalesce.
    bool         takePending( twEditorSize &out );

private:
    bool         pending_ = false;
    twEditorSize size_{};
};

class twVst3Editor final : public twPluginEditor {
public:
    // Applies one GUI-originated parameter edit to the mirror and the DSP.
    // A callback rather than a twVst3Plugin* because that class is TU-local to
    // twvst3plugin.cc — it has no header at all — and giving it one purely so
    // the editor could name it would be a bigger change than the feature.
    using ApplyFn = std::function<void( std::uint32_t id, double value )>;

    // Run at destruction, after the view is released. The plugin uses it to drop
    // its live-editor count, which is what lets teardown report an editor that
    // outlived it. Separate from ApplyFn because it must run exactly once and on
    // a path that has nothing to do with parameters.
    using ReleaseFn = std::function<void()>;

    // Takes ONE reference on the view; releases it in the destructor. `handler`
    // is borrowed and must outlive this object, which the plugin guarantees by
    // destroying its editor before handler_ (see twvst3plugin.cc's teardown).
    twVst3Editor( Steinberg::IPlugView *view,
                  twVst3ComponentHandler *handler,
                  ApplyFn apply,
                  ReleaseFn onDestroy = {} );
    ~twVst3Editor() override;

    twEditorCaps caps() const override { return caps_; }
    twEditorApi  api() const override;

    bool attach( const twEditorHandle &parent ) override;
    void detach() override;

    twEditorSize size() const override;
    twEditorSize constrain( twEditorSize want ) const override;
    bool         setSize( twEditorSize s ) override;
    bool         setScale( double factor ) override;

    twEditorFeedback poll() override;

private:
    Steinberg::IPlugView   *view_    = nullptr;
    twVst3ComponentHandler *handler_ = nullptr;
    ApplyFn                 apply_;
    ReleaseFn               onDestroy_;
    twVst3PlugFrame         frame_;
    twEditorCaps            caps_{};
    bool                    attached_ = false;
};

}  // namespace audio
