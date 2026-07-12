#include "app/actions/sgridaction.h"
#include "app/model/sproject.h"
#include "app/model/sprojectprops.h"
#include "app/actions/sactionregistry.h"

bool SGridAction::getState( SProject *project ) const
{
    return project->prop( SProjectProps::GridVisible, true ).toBool();
}

void SGridAction::setState( SProject *project, bool on )
{
    project->setProp( SProjectProps::GridVisible, on );
}

static const bool s_reg_grid = (
    SActionRegistry::instance().registerType(
        QStringLiteral("grid-toggle"),
        []{ return new SGridAction( SToggleSettingAction::Toggle ); } ),
    SActionRegistry::instance().registerType(
        QStringLiteral("grid-enable"),
        []{ return new SGridAction( SToggleSettingAction::Enable ); } ),
    SActionRegistry::instance().registerType(
        QStringLiteral("grid-disable"),
        []{ return new SGridAction( SToggleSettingAction::Disable ); } ),
    true
);
