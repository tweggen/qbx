#ifndef SCYCLEACTION_H
#define SCYCLEACTION_H

#include "stogglesettingaction.h"

// Toggle/enable/disable cycle (loop) playback. STUB: flips the state (so the
// toolbar button reflects it) but looped playback is not implemented yet.
// Verbs: cycle-toggle / -enable / -disable.
class SCycleAction : public SToggleSettingAction {
public:
    explicit SCycleAction( Op op = Toggle ) : SToggleSettingAction( op ) {}

protected:
    QString baseName() const override { return QStringLiteral("cycle"); }
    bool getState( SProject *project ) const override;
    void setState( SProject *project, bool ) override;
};

#endif // SCYCLEACTION_H
