#include "actions/scycleaction.h"
#include "sproject.h"
#include "sprojectprops.h"
#include "sactionregistry.h"

// Tracks the cycle on/off flag. The actual loop playback lives in twSpeaker's
// render callback; SMainWindow pushes this flag (and the range bounds) to the
// speaker via syncCyclePlayback() whenever it changes.
bool SCycleAction::getState( SProject *project ) const
{
    return project->prop( SProjectProps::Cycle, false ).toBool();
}

void SCycleAction::setState( SProject *project, bool on )
{
    project->setProp( SProjectProps::Cycle, on );
}

static const bool s_reg_cycle = (
    SActionRegistry::instance().registerType(
        QStringLiteral("cycle-toggle"),
        []{ return new SCycleAction( SToggleSettingAction::Toggle ); } ),
    SActionRegistry::instance().registerType(
        QStringLiteral("cycle-enable"),
        []{ return new SCycleAction( SToggleSettingAction::Enable ); } ),
    SActionRegistry::instance().registerType(
        QStringLiteral("cycle-disable"),
        []{ return new SCycleAction( SToggleSettingAction::Disable ); } ),
    true
);
