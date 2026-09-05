#include "app/objects/track/ssetlanehiddenaction.h"
#include "app/model/sobjectpath.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/actions/sactionregistry.h"
#include <QDebug>
#include <QDomElement>

using namespace strackpath;

SApplyResult SSetLaneHiddenAction::apply( SProject *project )
{
    SObject *mixer = splacements::rootNamed( project, pathRoot_ );
    if( !mixer ) {
        qWarning() << "set-lane-hidden: no root named" << pathRoot_;
        return { false, nullptr };
    }
    // laneAt() resolves the SPATH_MASTER sentinel through
    // SObject::systemLaneAt(), which is the only way to address a lane that is
    // not a childLinks() member (D2/D9).
    SObject *lane = splacements::laneAt( mixer, trackPath_ );
    if( !lane ) {
        qWarning() << "set-lane-hidden: no lane at"
                   << qualifiedToString( pathRoot_, trackPath_ );
        return { false, nullptr };
    }

    // SCOPED TO SYSTEM LANES, AND REFUSED RATHER THAN SILENTLY INERT.
    // appendRowsFor() walks childLinks() and does NOT consult laneHidden(), so
    // hiding an ordinary track would change the model and nothing on screen --
    // a verb that reports success and does nothing, which is the worst of the
    // three options. Honouring it for user lanes is a real feature with a real
    // blast radius (selection, drag targets, the reorder drop rules, what a
    // hidden FOLDER does to its children) and M4 does not ask for it; when it
    // is wanted, this guard is the one line to widen.
    if( lane->systemRole() == SSystemRole::None ) {
        qWarning() << "set-lane-hidden: refused on"
                   << qualifiedToString( pathRoot_, trackPath_ )
                   << "- only a SYSTEM lane can be hidden today; the arranger "
                      "does not honour the flag for an ordinary track";
        return { false, nullptr };
    }

    // The inverse restores the EXPLICITNESS, not just the value: a lane that
    // carried no attribute before must carry none afterwards, or an undo would
    // leave the project file differing from the one it started as.
    const bool wasExplicit = lane->laneHiddenIsExplicit();
    const bool wasHidden   = lane->laneHidden();

    if( clear_ ) lane->clearLaneHidden();
    else         lane->setLaneHidden( hidden_ );

    SSetLaneHiddenAction *inverse =
        new SSetLaneHiddenAction( trackPath_, wasHidden, !wasExplicit );
    inverse->pathRoot_ = pathRoot_;
    return { true, inverse };
}

void SSetLaneHiddenAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "trackPath", qualifiedToString( pathRoot_, trackPath_ ) );
    elem.setAttribute( "hidden", hidden_ ? "1" : "0" );
    // Written only for the inverse form, so an ordinary hand-written element
    // round-trips without it (the action_roundtrip_test rule).
    if( clear_ ) elem.setAttribute( "clear", "1" );
}

bool SSetLaneHiddenAction::readXml( const QDomElement &elem, int /*version*/ )
{
    trackPath_ = parseInto( pathRoot_, elem.attribute( "trackPath" ) );
    hidden_    = elem.attribute( "hidden", "1" ) != "0";
    clear_     = elem.attribute( "clear", "0" ) != "0";
    return true;
}

static const bool s_reg_setlanehidden = (
    SActionRegistry::instance().registerType(
        QStringLiteral("set-lane-hidden"),
        []{ return new SSetLaneHiddenAction; }
    ), true
);
