#pragma once

// CLAP native editor (proposal 33 M6), behind the format-neutral twPluginEditor.
//
// PRIVATE to tw_plugins and living in plugins/src/ for the reason
// plugins/CONTRACT.md inv. 4 gives: this header names clap types, and a public
// header whose declarations changed with TW_HAVE_CLAP would give tw_plugins and
// its consumers different views of the same type. Everything the app sees is
// tw/plugins/twplugineditor.h, which names none of them.
//
// WHAT IS DIFFERENT FROM VST3, and it is the whole of M6's substance:
//
//   * A CLAP plugin has ONE instance. Its GUI and its DSP are the same object,
//     so a knob turned in the GUI is ALREADY audible before the host hears
//     about it. There is no controller to write back to and no ring to push —
//     twVst3Plugin::applyGuiEdit's second job does not exist here. All the host
//     has to do is keep its MIRROR in step so getParam() does not disagree with
//     what the plugin is playing.
//
//   * The edits do not arrive through a callback. They come out of
//     clap_process::out_events as CLAP_EVENT_PARAM_VALUE plus
//     CLAP_EVENT_PARAM_GESTURE_BEGIN/END — which means they arrive on whatever
//     thread called process(), i.e. a revalidation worker. VST3's performEdit
//     is a UI-thread call; this one is not, and that difference is what the
//     lock discipline in twclapplugin.cc exists for.
//
//   * When nothing is rendering there is no process() to carry them, so the
//     plugin calls clap_host_params->request_flush() and the HOST must call
//     params->flush() to collect them. Without that, a knob turned with the
//     transport stopped — the commonest case there is — never reaches the
//     model at all. twClapPlugin::guiDrain() is where that happens.
//
// THREADING: main thread only, like every twPluginEditor. See that header.

#include "tw/plugins/twplugineditor.h"

#include <clap/clap.h>

#include <functional>
#include <string>

namespace audio {

class twClapEditor final : public twPluginEditor {
public:
    // Collect everything the plugin has asked for since the last call. A
    // callback rather than a twClapPlugin* because that class is TU-local to
    // twclapplugin.cc — it has no header at all — and giving it one purely so
    // the editor could name it would be a bigger change than the feature. Same
    // reasoning as twVst3Editor::ApplyFn.
    // `guiWasDestroyed` comes back true when the plugin told the host, through
    // clap_host_gui->closed(was_destroyed=true), that its GUI is already gone.
    // The host must then acknowledge with destroy() and must NOT call hide()
    // first — which is a decision detach() can only make if it is told.
    using DrainFn = std::function<twEditorFeedback( bool &guiWasDestroyed )>;

    // Run at destruction, after the GUI is destroyed. The plugin uses it to drop
    // its live-editor count, which is what lets teardown report an editor that
    // outlived it.
    using ReleaseFn = std::function<void()>;

    twClapEditor( const clap_plugin_t *plugin, const clap_plugin_gui_t *gui,
                  std::string title, DrainFn drain, ReleaseFn onDestroy = {} );
    ~twClapEditor() override;

    twEditorCaps caps() const override { return caps_; }
    twEditorApi  api() const override;

    bool attach( const twEditorHandle &parent ) override;
    bool attachFloating( const twEditorHandle &transientParent ) override;
    void detach() override;

    twEditorSize size() const override;
    twEditorSize constrain( twEditorSize want ) const override;
    bool         setSize( twEditorSize s ) override;
    bool         setScale( double factor ) override;

    twEditorFeedback poll() override;

private:
    // The CLAP window-api string this platform embeds with. One place, because
    // is_api_supported(), create() and set_parent() must all agree and a
    // mismatch between them is a silent no-window rather than an error.
    static const char *apiString();

    // Steps 7..12 of the gui.h flowchart, shared by both attach paths.
    bool created_ = false;

    const clap_plugin_t     *plugin_ = nullptr;
    const clap_plugin_gui_t *gui_    = nullptr;
    std::string              title_;
    DrainFn                  drain_;
    ReleaseFn                onDestroy_;

    twEditorCaps caps_{};
    bool         floating_ = false;
    bool         shown_    = false;
    // Set from the drain when the plugin destroyed its own GUI. detach() then
    // skips hide() and calls destroy() purely as the acknowledgement gui.h
    // requires.
    bool         destroyedByPlugin_ = false;
};

}  // namespace audio
