#ifndef SSNAPTOGRIDACTION_H
#define SSNAPTOGRIDACTION_H

#include "stogglesettingaction.h"

// Toggle/enable/disable "snap to grid" (clip times align to the time grid).
// Verbs: snap-to-grid-toggle / -enable / -disable.
class SSnapToGridAction : public SToggleSettingAction {
public:
    explicit SSnapToGridAction( Op op = Toggle ) : SToggleSettingAction( op ) {}

protected:
    QString baseName() const override { return QStringLiteral("snap-to-grid"); }
    bool getState( SProject *project ) const override;
    void setState( SProject *project, bool ) override;
};

#endif // SSNAPTOGRIDACTION_H
