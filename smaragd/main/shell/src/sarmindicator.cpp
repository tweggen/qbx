#include "app/shell/sarmindicator.h"

#include "app/model/sarmcensus.h"
#include "app/model/sproject.h"

SArmIndicator::SArmIndicator( QWidget *parent )
    : SLaneBadge( QStringLiteral( "armIndicator" ), parent )
{
}

void SArmIndicator::refresh( SProject *project, SObject *activeRoot,
                             const QString &masterLabel )
{
    if( !project ) { clearState(); return; }

    // THE TOOLTIP STATES THE RULE, NOT THE INSTANT. "Its input is open" would
    // be false for a lane that came back armed from a saved project and has
    // not been armed again in this session (QBX-260 design D9), and the badge
    // has no business claiming a device state it does not read. What is always
    // true is what an arm MEANS.
    refreshFrom(
        project, activeRoot, masterLabel,
        sarm::armedLanesInProject( project ),
        { QStringLiteral( "ARM" ),
          tr( "At least one track is armed for recording. Recording captures "
              "into the armed tracks, and an armed track's monitor setting is "
              "what opens its input and takes its instrument live." ),
          tr( "Armed: %1" ),
          tr( "Click to disarm every track (one undo step)." ) } );
}

QString SArmIndicator::describe() const
{
    return describeFields( QStringLiteral( "arm" ) )
           + QStringLiteral( "|armList=%1" ).arg( specList() );
}
