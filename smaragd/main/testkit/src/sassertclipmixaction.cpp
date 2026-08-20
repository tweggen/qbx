#include "app/testkit/sassertclipmixaction.h"

#include <cmath>

#include <QDebug>
#include <QDomElement>

#include "app/actions/sactionregistry.h"
#include "app/model/sclipwindow.h"
#include "app/model/slink.h"
#include "app/model/sobjectpath.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"

using namespace strackpath;

SApplyResult SAssertClipMixAction::apply( SProject *project )
{
    if( !project ) return { false, nullptr };
    // PARSE FIRST: parseInto() is what SETS pathRoot_ from a qualified
    // text path, so resolving the root before it runs reads an empty
    // root and silently addresses the MASTER.
    const QList<int> idx_ = parseInto( pathRoot_, clip_ );
    SObject *mixer = splacements::rootNamed( project, pathRoot_ );
    SLink *link = mixer ? splacements::placementAt( mixer, idx_ )
    : nullptr;
    if( !link ) {
        qWarning() << "assert-clip-mix: no clip at" << clip_;
        return { false, nullptr };
    }
    SObject *obj = &link->getSObject();
    // A stack presents the addressed take through the generic take seam —
    // the same resolution set-clip-volume / set-clip-pan use.
    if( SClipWindow *w = obj->windowTakeAt( take_ ) ) obj = &w->asObject();

    const double gotVolume = obj->getVolume();
    const double gotPan    = obj->getPan();

    const QString detail = QString( "clip %1: volumeDb=%2 pan=%3" )
        .arg( clip_ ).arg( gotVolume ).arg( gotPan );

    if( hasVolume_
        && std::fabs( gotVolume - expectVolumeDb_ ) > volumeTolerance_ ) {
        qWarning() << "assert-clip-mix FAILED: volumeDb expected"
                   << expectVolumeDb_ << "got" << gotVolume << "-" << detail;
        return { false, nullptr };
    }
    if( hasPan_ && std::fabs( gotPan - expectPan_ ) > panTolerance_ ) {
        qWarning() << "assert-clip-mix FAILED: pan expected" << expectPan_
                   << "got" << gotPan << "-" << detail;
        return { false, nullptr };
    }
    qDebug() << "assert-clip-mix: OK -" << detail;
    return { true, nullptr };   // assertions are not undoable
}

void SAssertClipMixAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "clip", clip_ );
    elem.setAttribute( "take", take_ );
    if( hasVolume_ ) {
        elem.setAttribute( "expectVolumeDb", QString::number( expectVolumeDb_ ) );
        elem.setAttribute( "volumeTolerance", QString::number( volumeTolerance_ ) );
    }
    if( hasPan_ ) {
        elem.setAttribute( "expectPan", QString::number( expectPan_ ) );
        elem.setAttribute( "panTolerance", QString::number( panTolerance_ ) );
    }
}

bool SAssertClipMixAction::readXml( const QDomElement &elem, int )
{
    clip_ = elem.attribute( "clip", "" );
    take_ = elem.attribute( "take", "-1" ).toInt();
    hasVolume_ = elem.hasAttribute( "expectVolumeDb" );
    if( hasVolume_ )
        expectVolumeDb_ = elem.attribute( "expectVolumeDb" ).toDouble();
    volumeTolerance_ = elem.attribute( "volumeTolerance", "1e-6" ).toDouble();
    hasPan_ = elem.hasAttribute( "expectPan" );
    if( hasPan_ )
        expectPan_ = elem.attribute( "expectPan" ).toDouble();
    panTolerance_ = elem.attribute( "panTolerance", "1e-6" ).toDouble();
    return true;
}

static const bool s_reg_assert_clip_mix = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "assert-clip-mix" ),
        []{ return new SAssertClipMixAction; } ), true );
