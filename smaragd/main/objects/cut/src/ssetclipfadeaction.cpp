#include "app/objects/cut/ssetclipfadeaction.h"

#include <QDebug>
#include <QDomElement>

#include "app/actions/sactionregistry.h"
#include "app/model/sclipwindow.h"
#include "app/model/slink.h"
#include "app/model/sobjectpath.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/objects/cut/scut.h"

using namespace strackpath;

SSetClipFadeAction::SSetClipFadeAction( const QString &clip, qint64 fadeIn,
                                        qint64 fadeOut, twFadeShape shape,
                                        int take )
    : clip_( clip ), fadeIn_( fadeIn ), fadeOut_( fadeOut ), shape_( shape ),
      take_( take )
{
}

SApplyResult SSetClipFadeAction::apply( SProject *project )
{
    if( !project ) return { false, nullptr };
    // PARSE FIRST (parseInto sets pathRoot_).
    const QList<int> idx = parseInto( pathRoot_, clip_ );
    SObject *root = splacements::rootNamed( project, pathRoot_ );
    SLink *link = root ? splacements::placementAt( root, idx ) : nullptr;
    if( !link ) {
        qWarning() << "set-clip-fade: no clip at" << clip_;
        return { false, nullptr };
    }
    SObject *obj = &link->getSObject();
    // A NAMED take, else the window whose parameters this placement carries —
    // the two questions proposal 42 M2 separated.
    if( SClipWindow *w = ( take_ >= 0 ? obj->windowTakeAt( take_ )
                                      : SClipWindow::parametersOf( *obj ) ) )
        obj = &w->asObject();

    SCut *cut = dynamic_cast<SCut *>( obj );
    if( !cut ) {
        // Refused, not ignored: a fade is a gain shape and an event clip's
        // equivalent is velocity — a different thing in a different domain.
        qWarning() << "set-clip-fade: not an audio clip at" << clip_;
        return { false, nullptr };
    }

    const twClipFade before = cut->getFade();
    twClipFade next;
    next.inLen  = fadeIn_  < 0 ? 0 : fadeIn_;
    next.outLen = fadeOut_ < 0 ? 0 : fadeOut_;
    next.shape  = shape_;
    cut->setFade( next );

    SSetClipFadeAction *inverse = new SSetClipFadeAction(
        clip_, before.inLen, before.outLen, before.shape, take_ );
    return { true, inverse };
}

void SSetClipFadeAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "clip", clip_ );
    elem.setAttribute( "fadeIn", QString::number( fadeIn_ ) );
    elem.setAttribute( "fadeOut", QString::number( fadeOut_ ) );
    elem.setAttribute( "shape", shape_ == twFadeShape::EqualPower
                                    ? "equalPower" : "linear" );
    elem.setAttribute( "take", take_ );
}

bool SSetClipFadeAction::readXml( const QDomElement &elem, int )
{
    clip_    = elem.attribute( "clip", "" );
    fadeIn_  = elem.attribute( "fadeIn", "0" ).toLongLong();
    fadeOut_ = elem.attribute( "fadeOut", "0" ).toLongLong();
    shape_   = ( elem.attribute( "shape", "linear" ) == "equalPower" )
                   ? twFadeShape::EqualPower : twFadeShape::Linear;
    take_    = elem.attribute( "take", "-1" ).toInt();
    return true;
}

static const bool s_reg_set_clip_fade = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "set-clip-fade" ),
        []{ return new SSetClipFadeAction; } ), true );
