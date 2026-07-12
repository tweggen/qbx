#include "app/actions/ssnaptogridaction.h"
#include "app/model/sproject.h"
#include "app/model/sprojectprops.h"
#include "app/actions/sactionregistry.h"

bool SSnapToGridAction::getState( SProject *project ) const
{
    return project->prop( SProjectProps::SnapToGrid, true ).toBool();
}

void SSnapToGridAction::setState( SProject *project, bool on )
{
    project->setProp( SProjectProps::SnapToGrid, on );
}

static const bool s_reg_snaptogrid = (
    SActionRegistry::instance().registerType(
        QStringLiteral("snap-to-grid-toggle"),
        []{ return new SSnapToGridAction( SToggleSettingAction::Toggle ); } ),
    SActionRegistry::instance().registerType(
        QStringLiteral("snap-to-grid-enable"),
        []{ return new SSnapToGridAction( SToggleSettingAction::Enable ); } ),
    SActionRegistry::instance().registerType(
        QStringLiteral("snap-to-grid-disable"),
        []{ return new SSnapToGridAction( SToggleSettingAction::Disable ); } ),
    true
);
