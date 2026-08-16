#include "app/testkit/sassertautomationvalueaction.h"

#include "app/actions/sactionregistry.h"
#include "app/model/sautomationlane.h"
#include "app/model/sobject.h"
#include "app/model/sobjectpath.h"
#include "app/objects/track/sautomationactions.h"
#include "tw/core/twfraction.h"

#include <QDebug>
#include <QDomElement>
#include <cmath>

using namespace strackpath;

SApplyResult SAssertAutomationValueAction::apply( SProject *project )
{
    sautomation::OwnerRef o =
        sautomation::resolveOwner( project, ownerPath_, target_, slotIndex_, take_ );
    if( !o.valid() ) {
        qWarning() << "assert-automation-value: cannot resolve owner"
                   << pathToString( ownerPath_ ) << "slot" << slotIndex_
                   << "for target" << target_;
        return { false, nullptr };
    }

    SAutomationLane *lane = o.owner->automationLane( target_ );
    if( !lane ) {
        qWarning() << "assert-automation-value: no lane" << target_ << "on owner"
                   << pathToString( ownerPath_ );
        return { false, nullptr };
    }

    if( !mode_.isEmpty() ) {
        const QString actual = sAutomationModeToString( lane->mode() );
        if( actual.compare( mode_, Qt::CaseInsensitive ) != 0 ) {
            qWarning() << "assert-automation-value:" << target_ << "mode is"
                       << actual << "expected" << mode_;
            return { false, nullptr };
        }
    }
    if( pointCount_ >= 0 && lane->pointCount() != pointCount_ ) {
        qWarning() << "assert-automation-value:" << target_ << "holds"
                   << lane->pointCount() << "points, expected" << pointCount_;
        return { false, nullptr };
    }

    const double got = lane->valueAt( time_ );
    if( std::fabs( got - value_ ) > tolerance_ ) {
        qWarning() << "assert-automation-value:" << target_ << "at frame"
                   << (qlonglong) time_ << "reads" << got << "expected" << value_
                   << "+/-" << tolerance_;
        return { false, nullptr };
    }
    return { true, nullptr };
}

void SAssertAutomationValueAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "owner", pathToString( ownerPath_ ) );
    elem.setAttribute( "target", target_ );
    elem.setAttribute( "time", QString::number( (qlonglong) time_ ) );
    elem.setAttribute( "value", QString::number( value_, 'g', 17 ) );
    elem.setAttribute( "tolerance", QString::number( tolerance_, 'g', 17 ) );
    if( slotIndex_ >= 0 )  elem.setAttribute( "slotIndex", slotIndex_ );
    if( take_ >= 0 )       elem.setAttribute( "take", take_ );
    if( !mode_.isEmpty() ) elem.setAttribute( "mode", mode_ );
    if( pointCount_ >= 0 ) elem.setAttribute( "pointCount", pointCount_ );
}

bool SAssertAutomationValueAction::readXml( const QDomElement &elem, int )
{
    ownerPath_  = stringToPath( elem.attribute( "owner" ) );
    target_     = elem.attribute( "target" );
    time_       = (offset_t) parseFractionOrDouble(
                      elem.attribute( "time", "0" ).toStdString() ).toDouble();
    value_      = elem.attribute( "value", "0" ).toDouble();
    tolerance_  = elem.attribute( "tolerance", "1e-6" ).toDouble();
    slotIndex_  = elem.attribute( "slotIndex", "-1" ).toInt();
    take_       = elem.attribute( "take", "-1" ).toInt();
    mode_       = elem.attribute( "mode" );
    pointCount_ = elem.attribute( "pointCount", "-1" ).toInt();
    return true;
}

static const bool s_reg_assertautomationvalue = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "assert-automation-value" ),
        []{ return new SAssertAutomationValueAction; } ), true );
