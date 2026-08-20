#include "app/objects/track/seteditgroupaction.h"
#include "app/model/sobjectpath.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/actions/sactionregistry.h"
#include <QDomElement>

using namespace strackpath;

SSetEditGroupAction::SSetEditGroupAction( const QList<int> &trackPath,
                                          int group )
    : trackPath_( trackPath ), group_( group )
{
}

SApplyResult SSetEditGroupAction::apply( SProject *project )
{
    if( !project || group_ < 0 ) {
        return {false, nullptr};
    }
    SObject *mixer = splacements::rootNamed( project, pathRoot_ );
    SObject *lane = splacements::laneAt( mixer, trackPath_ );
    if( !lane ) {
        return {false, nullptr};
    }
    const int old = lane->getEditGroup();
    lane->setEditGroup( group_ );
    SAction *inverse = new SSetEditGroupAction( trackPath_, old );
    return {true, inverse};
}

void SSetEditGroupAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "trackPath", qualifiedToString( pathRoot_, trackPath_ ) );
    elem.setAttribute( "group", group_ );
}

bool SSetEditGroupAction::readXml( const QDomElement &elem, int /*version*/ )
{
    trackPath_ = parseInto( pathRoot_, elem.attribute( "trackPath" ) );
    group_ = elem.attribute( "group", "0" ).toInt();
    return true;
}

static const bool s_reg_seteditgroup = (
    SActionRegistry::instance().registerType(
        QStringLiteral("set-edit-group"),
        []{ return new SSetEditGroupAction; }
    ), true
);
