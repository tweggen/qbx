#include "app/objects/track/ssettracksoloaction.h"
#include "app/model/sobjectpath.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/actions/sactionregistry.h"
#include <QDomElement>

using namespace strackpath;

SSetTrackSoloAction::SSetTrackSoloAction( const QList<int> &trackPath, bool solo )
    : trackPath_( trackPath ), solo_( solo )
{
}

SApplyResult SSetTrackSoloAction::apply( SProject *project )
{
    SObject *mixer = splacements::rootContainer( project );
    SObject *lane = splacements::laneAt( mixer, trackPath_ );
    if( !lane ) {
        return {false, nullptr};
    }
    const bool old = lane->isSolo();
    // Everything audible is downstream of this one write: setSolo() emits
    // soloChanged(), which a folder relays up as subtreeSoloChanged() until it
    // reaches the root mixer, which re-applies the audibility rule to the whole
    // lane tree and stales the affected pages.
    lane->setSolo( solo_ );
    SAction *inverse = new SSetTrackSoloAction( trackPath_, old );
    return {true, inverse};
}

void SSetTrackSoloAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "trackPath", pathToString( trackPath_ ) );
    elem.setAttribute( "solo", solo_ ? "1" : "0" );
}

bool SSetTrackSoloAction::readXml( const QDomElement &elem, int /*version*/ )
{
    trackPath_ = stringToPath( elem.attribute( "trackPath" ) );
    // "1"/"0" is set-track-mute's spelling; "true" is accepted so a
    // hand-written element in either house style works.
    const QString v = elem.attribute( "solo", "0" );
    solo_ = ( v == "1" || v == "true" );
    return true;
}

static const bool s_reg_settracksolo = (
    SActionRegistry::instance().registerType(
        QStringLiteral("set-track-solo"),
        []{ return new SSetTrackSoloAction; }
    ), true
);
