#ifndef SSETPLUGINBYPASSACTION_H
#define SSETPLUGINBYPASSACTION_H

#include "app/actions/saction.h"

// Action: set (not toggle) the bypass flag of one plugin slot.
//
// proposal 08 M5. Before this, SPluginEffectStrip poked SPluginSlot::setBypass()
// straight from the checkbox — a model mutation outside the action system, which
// is exactly what app/pluginui/CONTRACT.md invariant 3 forbids, and it was
// therefore not undoable and not scriptable.
//
// ABSOLUTE, not a toggle: an action must be replayable and its inverse must be
// exact. A toggle applied twice by a repeated undo would land on the wrong value.
//
// Inverse: the same action carrying the pre-mutation flag.
class SSetPluginBypassAction : public SAction {
public:
    SSetPluginBypassAction( const QString &trackPath = QString(),
                            int slotIndex = 0, bool bypassed = false );

    QString name() const override { return QStringLiteral( "set-plugin-bypass" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString trackPath_;
    int     slotIndex_;
    bool    bypassed_;
};

#endif  // SSETPLUGINBYPASSACTION_H
