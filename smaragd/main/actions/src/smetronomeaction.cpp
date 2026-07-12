#include "app/actions/smetronomeaction.h"
#include "app/model/sproject.h"
#include "app/model/sprojectprops.h"
#include "app/actions/sactionregistry.h"

// STUB: the metronome has no audible effect yet; this only tracks on/off state.
bool SMetronomeAction::getState( SProject *project ) const
{
    return project->prop( SProjectProps::Metronome, false ).toBool();
}

void SMetronomeAction::setState( SProject *project, bool on )
{
    project->setProp( SProjectProps::Metronome, on );
}

static const bool s_reg_metronome = (
    SActionRegistry::instance().registerType(
        QStringLiteral("metronome-toggle"),
        []{ return new SMetronomeAction( SToggleSettingAction::Toggle ); } ),
    SActionRegistry::instance().registerType(
        QStringLiteral("metronome-enable"),
        []{ return new SMetronomeAction( SToggleSettingAction::Enable ); } ),
    SActionRegistry::instance().registerType(
        QStringLiteral("metronome-disable"),
        []{ return new SMetronomeAction( SToggleSettingAction::Disable ); } ),
    true
);
