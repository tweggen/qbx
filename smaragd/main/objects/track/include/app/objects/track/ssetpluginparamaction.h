#ifndef SSETPLUGINPARAMACTION_H
#define SSETPLUGINPARAMACTION_H

#include "app/actions/saction.h"

#include <cstdint>

// Action: set one plugin parameter of one slot to an ABSOLUTE value.
//
// proposal 08 M5. SPluginParamEditor used to call twPlugin::setParam() straight
// from the slider — outside the action system (app/pluginui/CONTRACT.md
// invariant 3), not undoable, and INAUDIBLE, because two caches sit in front of
// the plugin (the slot processor's all-bus page cache and each tap's frozen
// pages) and nothing staled them.
//
// Coalescing: mergeKey() is (trackPath, slotIndex, paramId), modelled on
// SSetTrackVolumeAction — dragging a slider must produce ONE undo entry, not one
// per pixel. mergeWith() absorbs the newer target value and keeps our own
// (older) baseline, which is what the already-captured inverse was measured
// against.
class SSetPluginParamAction : public SAction {
public:
    SSetPluginParamAction( const QString &trackPath = QString(),
                           int slotIndex = 0, std::uint32_t paramId = 0,
                           double value = 0.0 );

    QString name() const override { return QStringLiteral( "set-plugin-param" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

    QString mergeKey() const override;
    bool mergeWith( const SAction *later ) override;

    // Tell the action what the parameter held BEFORE the edit, instead of
    // letting apply() read it from the plugin.
    //
    // There is exactly one caller and it is not a style choice: an edit made in
    // the plugin's OWN GUI has ALREADY reached the plugin by the time the app
    // hears about it — twPluginEditor::poll() writes the mirror and the DSP on
    // its way through, which is what makes a knob audible whatever the host
    // does — so apply()'s `getParam()` baseline is the NEW value and the
    // inverse it builds restores the value it was asked to set. Undo becomes a
    // no-op, silently. Shipped in proposal 33 M5; caught by
    // qxa.plugin_native_editor; the pre-edit value now travels with the edit
    // (twplugineditor.h, twEditorParamEdit::previousValue).
    //
    // NOT serialized: it is a property of the moment the action was made, not
    // of the verb, and a script's set-plugin-param has no such moment. A
    // deserialized action therefore behaves exactly as it always did.
    void setPreviousValue( double v ) { previous_ = v; hasPrevious_ = true; }

private:
    QString       trackPath_;
    int           slotIndex_;
    std::uint32_t paramId_;
    double        value_;
    double        previous_    = 0.0;
    bool          hasPrevious_ = false;
};

#endif  // SSETPLUGINPARAMACTION_H
