#include "app/shell/srecordmodeseed.h"
#include "app/servicesui/soptions.h"
#include "app/shell/ssettings.h"
#include "app/model/sproject.h"
#include "app/model/sprojectprops.h"
#include <QString>

namespace srecmodeseed {

void seedInto( SProject *project )
{
    if( !project ) return;

    // THE INI KEEPS ITS OWN THREE-VALUE VOCABULARY (`new-take | overdub |
    // replace`), which predates plan 53 and is what the MIDI recorder still
    // reads. Mapped here rather than renamed: an INI the user already has must
    // keep meaning what it meant, and `takes-split` has no INI spelling yet
    // because no release has offered it.
    const QString ini =
        SSettings::instance().value( SOpt::MidiRecordMode,
                                     SOpt::def( SOpt::MidiRecordMode ) )
            .toString();

    QString mode;
    if( ini == QLatin1String( "overdub" ) )      mode = QStringLiteral( "overdub" );
    else if( ini == QLatin1String( "replace" ) ) mode = QStringLiteral( "replace" );
    else if( SProjectProps::isRecordMode( ini ) ) mode = ini;   // a plan-53 spelling
    else mode = QString::fromLatin1( SProjectProps::recordModeDefault() );
    // `new-take`, anything unknown, and an absent key all land on the default
    // through the last two branches -- which is deliberate: an INI value no
    // build understands must not pin a project to a mode no planner knows.

    // WRITE NOTHING WHEN IT EQUALS THE DEFAULT, so a user who never changed
    // the option gets a project that states no mode and re-serializes exactly
    // as a pre-plan-53 one does.
    if( mode == QLatin1String( SProjectProps::recordModeDefault() ) ) return;
    project->setProp( SProjectProps::RecordMode, mode );
}

} // namespace srecmodeseed
