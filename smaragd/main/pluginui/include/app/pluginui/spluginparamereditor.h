#ifndef SPLUGINPARAMEREDITOR_H
#define SPLUGINPARAMEREDITOR_H

#include <QScrollArea>
#include <QString>
#include <cstdint>
#include <memory>
#include <vector>
#include "tw/plugins/twplugin.h"

class QSlider;
class QLabel;
class SPluginSlot;

// Generic parameter editor for one plugin SLOT.
// Shows a scrollable list of all parameters as sliders with names and values.
//
// proposal 08 M5. Two things changed from the version that was built and never
// constructed anywhere:
//
//  1. It is bound to the MODEL (SPluginSlot + the track path and slot index that
//     address it), not to a bare audio::twPlugin. A slider therefore submits a
//     set-plugin-param ACTION instead of poking twPlugin::setParam() directly —
//     app/pluginui/CONTRACT.md invariant 3 — which is what makes a parameter
//     edit undoable, scriptable, and (via SPluginSlot::notifyPluginEdited()
//     inside the action) audible at all.
//  2. It re-reads itself when the model says the parameters moved underneath it:
//     SPluginSlot::paramsChanged() (an undo, a restored state chunk) refreshes
//     the sliders, and pluginReloaded() rebuilds them, because a reloaded plugin
//     may expose a different parameter list (CONTRACT invariant 5).
class SPluginParamEditor : public QScrollArea {
    Q_OBJECT
public:
    // trackPath/slotIndex address the slot for the action; see
    // app/model/sobjectpath.h for the path convention.
    SPluginParamEditor( SPluginSlot *slot, const QString &trackPath,
                        int slotIndex, QWidget *parent = nullptr );

    // How many parameter rows are on screen. Zero for a Missing/Unsupported
    // slot: its placeholder plugin has no parameters.
    int paramRowCount() const { return (int) params_.size(); }

    // The slot's position in the chain, which is HALF of the action's address
    // and can change under an open window: reorder-plugin moves the slot without
    // touching it, and an editor holding the old index would then aim its next
    // parameter edit at a different plugin. SPluginEffectStrip::rebuildUI() keeps
    // every open editor's index in step.
    void setSlotIndex( int slotIndex ) { slotIndex_ = slotIndex; }

    // Headless seam for the qxa verb plugin-editor-set-param: move the slider
    // for `paramId` to `value` along exactly the path a user drag takes (the
    // slider's own valueChanged -> onParamSliderChanged -> submitted action), so
    // a test proves the WIRING and not just the model. Returns false if this
    // editor has no such parameter. Never synthesizes OS input.
    bool setParamFromUi( std::uint32_t paramId, double value );

    // Headless seam for a qxa case to read back what the value label DISPLAYS
    // (after the plugin's own value-to-text formatting), so a test proves units /
    // enum names reach the screen and not just the model. Empty for an out-of-range
    // row. `row` is 0..paramRowCount()-1, matching paramInfo() order.
    QString valueLabelText( int row ) const;

protected slots:
    void onParamSliderChanged( int sliderIndex );
    // The model moved: re-read the sliders / rebuild them.
    void onParamsChanged();
    void onPluginReloaded();

private:
    struct ParamWidget {
        audio::twPluginParamInfo info;
        QSlider *slider;
        QLabel  *valueLabel;
    };

    void buildUI();
    audio::twPlugin *livePlugin() const;
    // Instance (not static): prefers the live plugin's own value-to-text so a
    // value shows in the plugin's units / enum names, falling back to a numeric
    // rendering when the plugin offers no formatter.
    QString formatValue( const audio::twPluginParamInfo &info, double v ) const;

    SPluginSlot *slot_;
    QString      trackPath_;
    int          slotIndex_;
    // True while onParamsChanged() writes the sliders, so the resulting
    // valueChanged never turns an external change back into a new action.
    bool         applyingExternal_ = false;
    std::vector<ParamWidget> params_;
};

#endif
