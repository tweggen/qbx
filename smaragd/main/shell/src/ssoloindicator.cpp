#include "app/shell/ssoloindicator.h"

#include "app/model/sproject.h"
#include "app/model/ssolorules.h"

SSoloIndicator::SSoloIndicator( QWidget *parent )
    : SLaneBadge( QStringLiteral( "soloIndicator" ), parent )
{
}

void SSoloIndicator::refresh( SProject *project, SObject *activeRoot,
                              const QString &masterLabel )
{
    scope_ = QStringLiteral( "global" );
    if( !project ) { clearState(); return; }
    scope_ = project->soloScope() == SProject::SoloScope::Global
                 ? QStringLiteral( "global" )
                 : QStringLiteral( "arrangement" );

    // THE TOOLTIP SAYS WHAT THE SCOPE MAKES TRUE, and the two sentences are
    // different claims. In `global` a solo anywhere is why the rest of the
    // project is quiet; in `arrangement` it is not, and a badge that implied
    // otherwise would be the lie item 6 of QBX-257 forbids.
    const QString what =
        scope_ == QLatin1String( "global" )
            ? tr( "Solo is in force: only the soloed lane(s) play, in every "
                  "arrangement." )
            : tr( "Solo is in force inside its own arrangement: it changes "
                  "what that arrangement's placements play, not the rest of "
                  "the project." );

    refreshFrom( project, activeRoot, masterLabel,
                 ssolo::soloedLanesInProject( project ),
                 { QStringLiteral( "SOLO" ), what, tr( "Soloed: %1" ),
                   tr( "Click to release every solo (one undo step)." ) } );
}

QString SSoloIndicator::describe() const
{
    return describeFields( QStringLiteral( "solo" ) )
           + QStringLiteral( "|soloScope=%1|soloList=%2" )
                 .arg( scope_, specList() );
}
