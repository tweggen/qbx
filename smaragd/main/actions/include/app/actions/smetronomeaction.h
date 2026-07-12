#ifndef SMETRONOMEACTION_H
#define SMETRONOMEACTION_H

#include "app/actions/stogglesettingaction.h"

// Toggle/enable/disable the metronome. STUB: flips the state (so the toolbar
// button reflects it) but no click track is generated yet.
// Verbs: metronome-toggle / -enable / -disable.
class SMetronomeAction : public SToggleSettingAction {
public:
    explicit SMetronomeAction( Op op = Toggle ) : SToggleSettingAction( op ) {}

protected:
    QString baseName() const override { return QStringLiteral("metronome"); }
    bool getState( SProject *project ) const override;
    void setState( SProject *project, bool ) override;
};

#endif // SMETRONOMEACTION_H
