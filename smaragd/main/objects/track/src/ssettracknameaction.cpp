#include "app/objects/track/ssettracknameaction.h"
#include "app/model/sobjectpath.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/actions/sactionregistry.h"
#include <QDebug>
#include <QDomElement>

using namespace strackpath;

// Resolve the addressed lane: path form first (it reaches nested lanes), then
// the legacy top-level index. Same helper shape as set-track-mute.
static SObject *laneFor( SProject *project, const QList<int> &path, int index )
{
    SObject *mixer = splacements::rootContainer( project );
    if( !mixer ) return nullptr;
    if( !path.isEmpty() ) return splacements::laneAt( mixer, path );
    if( index < 0 || index >= mixer->childCount() ) return nullptr;
    return splacements::laneAt( mixer, QList<int>{ index } );
}

SApplyResult SSetTrackNameAction::apply( SProject *project )
{
    SObject *lane = laneFor( project, trackPath_, trackIndex_ );
    if( !lane ) {
        qWarning() << "set-track-name: no track at"
                   << ( trackPath_.isEmpty() ? QString::number( trackIndex_ )
                                             : pathToString( trackPath_ ) );
        return {false, nullptr};
    }

    const QString old = lane->getSName();
    // setSName() emits sNameChanged(), which is what refreshes the head's name
    // field — so an undo puts the old string back on screen without the widget
    // having to know an action ran.
    lane->setSName( name_ );

    // The inverse addresses the SAME way it was addressed, so an index-form
    // element undoes through the index form.
    SSetTrackNameAction *inverse =
        trackPath_.isEmpty() ? new SSetTrackNameAction( trackIndex_, old )
                             : new SSetTrackNameAction( trackPath_, old );
    return {true, inverse};
}

void SSetTrackNameAction::writeXml( QDomElement &elem ) const
{
    if( !trackPath_.isEmpty() ) {
        elem.setAttribute( "trackPath", pathToString( trackPath_ ) );
    } else {
        elem.setAttribute( "trackIndex", QString::number( trackIndex_ ) );
    }
    elem.setAttribute( "name", name_ );
}

bool SSetTrackNameAction::readXml( const QDomElement &elem, int /*version*/ )
{
    trackIndex_ = elem.attribute( "trackIndex", "0" ).toInt();
    trackPath_ = stringToPath( elem.attribute( "trackPath" ) );
    name_ = elem.attribute( "name" );
    return true;
}

static const bool s_reg_settrackname = (
    SActionRegistry::instance().registerType(
        QStringLiteral("set-track-name"),
        []{ return new SSetTrackNameAction; }
    ), true
);
