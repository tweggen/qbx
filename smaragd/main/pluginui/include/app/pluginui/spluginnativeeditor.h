#pragma once

// The window a plugin's OWN interface lives in (proposal 33 M4).
//
// A non-modal QDialog holding one native container QWidget. The plugin renders
// into that container's native handle; we size ourselves to what it asks for and
// honour its resize requests. Host-parents-plugin, which is what JUCE and Ardour
// do and what proposal 33 §4 measured working on Win11.
//
// NOT PARENTED TO THE FX STRIP, and that is the whole reason this class exists
// rather than another branch in SPluginEffectStrip::ensureParamEditor():
// STrackDetailPanel::rebuildUI() DELETES the strip on every track switch
// (strackdetailpanel.cpp:162), so a window parented to it vanishes when the user
// clicks another lane. Tolerable for a slider list; for a synth's editor it
// reads as a crash. Ownership therefore lives in the module-level registry
// below, keyed by slot, and the windows are top-level.
//
// THAT LAST PARAGRAPH WAS ALSO THE BUG (found 2026-08-23): the constructor did
// `QDialog( parent )` with `parentForPosition` passed straight through as the
// real Qt ownership parent, and the FX strip's Edit handler passed the STRIP
// ITSELF. So a window opened from the strip WAS parented to the strip after
// all, and rebuildUI()'s `delete pluginStrip_` took it down as a Qt child —
// exactly the failure mode this comment already described as unacceptable.
// `restoreOpenEditors()` passed `SMainWindow` and its windows survived, which
// was the asymmetry that gave the fix away.
//
// The constructor now climbs to `parentForPosition->window()` — the durable
// TOP-LEVEL ancestor — before handing it to QDialog. A dock, a strip, a panel:
// whatever transient widget a caller has on hand, its window() is the
// long-lived QMainWindow, which is never deleted while the app runs. Passing
// SMainWindow directly (restoreOpenEditors) or nullptr (the testkit's headless
// open) both still work: window() on a window is itself, and window() on
// nullptr is nullptr. `parentForPosition`'s only remaining job is that walk —
// it is not read for anything else (attachPlugin()'s D1 floating-rung
// transient parent reads parentWidget()->window(), which after this change
// already equals what parentForPosition->window() computed at construction).

#include <QDialog>
#include <QHash>
#include <QPointer>
#include <QRect>
#include <QString>

#include <cstdint>
#include <map>
#include <memory>

#include "tw/plugins/twplugineditor.h"

class QTimer;
class QWidget;
class SPluginSlot;
class SProject;
class STrack;

class SPluginNativeEditor : public QDialog {
    Q_OBJECT

public:
    // Returns nullptr when this slot has no native editor, or when one could not
    // be attached — the caller then falls back to the generic slider editor.
    //
    // It takes the TRACK and the SLOT, not a track path and a slot index, and
    // that is the fix for a whole class of bug rather than a style choice. An
    // edit addresses the model as (trackPath, slotIndex); both are POSITIONS,
    // and both move under a window that outlives the strip. Removing a slot
    // above this one renumbers it; moving the track in the arranger renumbers
    // the path. A cached pair goes silently stale and the next knob turn is
    // committed to a DIFFERENT PLUGIN.
    //
    // The generic editor is re-pointed on every rebuildUI() for exactly this
    // reason (splugineffectstrip.cpp, CONTRACT inv. 7). A native window has no
    // rebuild to hang that on, so it derives both addresses FRESH at every
    // commit instead of being told when they moved. Nothing to keep in step,
    // and nothing to forget to call.
    //
    // showWindow = false opens and attaches WITHOUT putting anything on screen,
    // which is what a headless gate needs: a qxa run uses the REAL platform
    // plugin (the suite does not set QT_QPA_PLATFORM=offscreen), so a shown
    // dialog would appear on the developer's desktop mid-suite. The container's
    // native handle exists either way -- winId() creates it whether the window
    // is visible or not -- so the plugin is genuinely attached. Same precedent,
    // and the same spelling, as
    // SPluginEffectStrip::ensureParamEditor( slotIndex, showWindow ).
    static SPluginNativeEditor *openFor( STrack *track,
                                         SPluginSlot *slot,
                                         QWidget *parentForPosition,
                                         bool showWindow = true );

    // For the testkit, and for anything that has to take a window down without
    // owning it. The registry is module-private; these are the only way in.
    static bool isOpenFor( SPluginSlot *slot );
    static void closeFor( SPluginSlot *slot );

    // D2: re-open every editor a loaded project says was open. Called once,
    // after a load has finished and the UI exists.
    //
    // IT DOES NOTHING UNDER --test-case, and that is load-bearing rather than
    // tidy. A qxa run uses the REAL platform plugin — the suite does not set
    // QT_QPA_PLATFORM=offscreen, whatever proposal 33 D2 says — so restoring an
    // editor would put real plugin windows on the developer's desktop in the
    // middle of a suite. A case that wants to assert the flag round-trips reads
    // the MODEL (SPluginSlot::getEditorOpen), never a window.
    static void restoreOpenEditors( SProject *project, QWidget *parentForPosition );

    // Put a remembered rectangle somewhere the user can actually reach.
    //
    // PUBLIC AND STATIC so it can be gated (`plugin_editor_geometry_test`): the
    // failure it prevents — a window restored at x=2400 after the second
    // monitor was unplugged, off screen, unreachable, and on Windows not even
    // reported as a problem — cannot be reproduced by hand without physically
    // unplugging a monitor. It is a pure function of the rect and the current
    // screens, which is what makes that possible.
    //
    // The rule is the weakest one that fixes it: keep the SIZE (shrinking only
    // when it exceeds the screen), and move the position only as far as it
    // takes to get the window inside an available screen.
    static QRect clampOntoAScreen( const QRect &want );

    // True if this slot has a native editor available at all. Cheap: it asks
    // twPlugin::supportsNativeEditor() and instantiates nothing.
    static bool isAvailableFor( SPluginSlot *slot );

    ~SPluginNativeEditor() override;

    // D1's middle rung: the plugin owns a top-level window of its own and this
    // dialog is never shown. It stays alive purely as the poll pump and the
    // registry entry — there is nothing to put inside it, so showing it would
    // put an empty rectangle on screen beside the plugin's real window.
    // CLAP only; VST3 and AU have no floating concept.
    bool isFloating() const { return floating_; }

protected:
    void closeEvent( QCloseEvent *e ) override;
    void resizeEvent( QResizeEvent *e ) override;

private slots:
    void onPoll();

private:
    SPluginNativeEditor( STrack *track, SPluginSlot *slot, QWidget *parent );

    bool attachPlugin();

    // `<format>:<uid>` — the SSettings key the geometry hangs on. One plugin,
    // one remembered window size, wherever it is inserted.
    QString pluginKey() const;
    void    restoreGeometryFromSettings();
    void    saveGeometryToSettings() const;
    void applyEdit( std::uint32_t paramId, double value, double previousValue,
                    bool gestureEnd );

    // Acts on a restart request, or declines to — see the .cpp for the livelock
    // that makes "declines to" the important half.
    void handleRestart();
    void resizeToPlugin( audio::twEditorSize physical );

    // The model address, DERIVED, never cached. Both return an empty/-1 "cannot
    // address this any more", which is a real state: the track can be deleted
    // or the slot removed while the window is up.
    QString currentTrackPath() const;
    int     currentSlotIndex() const;

    QPointer<STrack>                 track_;
    QPointer<SPluginSlot>            slot_;
    QWidget                         *container_ = nullptr;
    QTimer                          *pollTimer_ = nullptr;
    std::unique_ptr<audio::twPluginEditor> editor_;
    bool                             applyingResize_ = false;
    bool                             floating_       = false;
    // Whether this window was ever actually mapped. A headless open
    // (showWindow = false) and a floating editor both have no geometry of ours
    // worth storing, and writing one would overwrite the user's real position
    // with a default-constructed rectangle.
    bool                             shown_          = false;

    // The last value committed per parameter, for the echo guard in applyEdit().
    std::map<std::uint32_t, double>  lastCommitted_;

    // THE RESTART GUARD (see onPoll()). The plugin's observable configuration
    // as of the last restart we acted on: a restart that changes neither is a
    // restart with nothing in it, and acting on one is a livelock. -1 means
    // "not sampled yet", which is not a value either can take.
    long long                        lastRestartLatency_    = -1;
    long long                        lastRestartParamCount_ = -1;
};
