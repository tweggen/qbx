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

private:
    QString       trackPath_;
    int           slotIndex_;
    std::uint32_t paramId_;
    double        value_;
};

#endif  // SSETPLUGINPARAMACTION_H
