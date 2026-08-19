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

#include <QDialog>
#include <QHash>
#include <QPointer>
#include <QString>

#include <cstdint>
#include <map>
#include <memory>

#include "tw/plugins/twplugineditor.h"

class QTimer;
class QWidget;
class SPluginSlot;
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
    static SPluginNativeEditor *openFor( STrack *track,
                                         SPluginSlot *slot,
                                         QWidget *parentForPosition );

    // True if this slot has a native editor available at all. Cheap: it asks
    // twPlugin::supportsNativeEditor() and instantiates nothing.
    static bool isAvailableFor( SPluginSlot *slot );

    ~SPluginNativeEditor() override;

protected:
    void closeEvent( QCloseEvent *e ) override;
    void resizeEvent( QResizeEvent *e ) override;

private slots:
    void onPoll();

private:
    SPluginNativeEditor( STrack *track, SPluginSlot *slot, QWidget *parent );

    bool attachPlugin();
    void applyEdit( std::uint32_t paramId, double value, bool gestureEnd );
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

    // The last value committed per parameter, for the echo guard in applyEdit().
    std::map<std::uint32_t, double>  lastCommitted_;
};
