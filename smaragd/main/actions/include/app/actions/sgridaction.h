#ifndef SGRIDACTION_H
#define SGRIDACTION_H

#include "app/actions/stogglesettingaction.h"

// Toggle/enable/disable drawing of the time grid in the arranger.
// Verbs: grid-toggle / -enable / -disable.
class SGridAction : public SToggleSettingAction {
public:
    explicit SGridAction( Op op = Toggle ) : SToggleSettingAction( op ) {}

protected:
    QString baseName() const override { return QStringLiteral("grid"); }
    bool getState( SProject *project ) const override;
    void setState( SProject *project, bool ) override;
};

#endif // SGRIDACTION_H
