#ifndef SPLUGINAPPEDITORS_H
#define SPLUGINAPPEDITORS_H

// spluginappeditors.h — "this instrument has an APP-SIDE editor" (proposal 56
// §4.11, QBX-279).
//
// A native instrument whose model lives in the app (NassauDigital's
// SSamplerProgram) has no plugin GUI to embed and no flat parameter list worth
// a slider editor: its editor is a Qt window of the app's own. The FX strip
// cannot name that window's module — `samplerui` sits ABOVE `pluginui` — so
// the window registers here, keyed by the plugin's uid, from a static
// initializer in its own module, and the strip consults this registry BEFORE
// the native editor and the generic slider list (SPluginEffectStrip::
// openParamEditor, editorKindFor; SPluginNativeEditor::restoreOpenEditors).
//
// The registry holds three functions per uid and nothing else. What the window
// is, where it lives and how it keeps the slot's `editorOpen` flag are the
// registering module's (samplerui/CONTRACT.md). The flag's rule is the native
// editor's (pluginui CONTRACT): set when the window opens, cleared when it is
// closed, so a project saved with it open says so.
//
// Main thread only.

#include <QString>
#include <QStringList>

#include <functional>

class QWidget;
class SPluginSlot;
class STrack;

namespace spluginappeditors {

struct Entry {
    // Open (or raise) the editor of `slot` on `track`. `parentForPosition` is a
    // widget whose window() may own the editor; `showWindow` false builds and
    // registers it WITHOUT putting anything on screen (a --test-case run).
    // False when the editor could not be opened.
    std::function<bool( STrack *track, SPluginSlot *slot, QWidget *parentForPosition,
                        bool showWindow )>
        open;
    std::function<bool( SPluginSlot *slot )> isOpen;
    std::function<void( SPluginSlot *slot )> close;
};

// Register the editor for plugins whose descriptor uid is `uid`. A second
// registration for the same uid replaces the first (and says so in the log).
void registerEditor( const QString &uid, const Entry &entry );

// The entry for `slot`'s plugin, or null when its uid registered none.
const Entry *entryFor( const SPluginSlot *slot );

// Convenience over entryFor().
bool hasEditorFor( const SPluginSlot *slot );
bool openFor( STrack *track, SPluginSlot *slot, QWidget *parentForPosition, bool showWindow );
bool isOpenFor( SPluginSlot *slot );
void closeFor( SPluginSlot *slot );

// The uids registered, sorted (for a describer).
QStringList registeredUids();

}  // namespace spluginappeditors

#endif
