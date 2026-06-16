#include "actions/sresizeclipaction.h"
#include "actions/strackpath.h"
#include "sproject.h"
#include "sactionregistry.h"
#include "sstdmixer.h"
#include "strack.h"
#include "slink.h"
#include "scut.h"
#include "twfraction.h"
#include <QDomElement>

using namespace strackpath;

SResizeClipAction::SResizeClipAction( const QList<int> &clipPath,
                                      offset_t startTime, offset_t startOffset,
                                      length_t duration, length_t loopLength,
                                      double stretch )
    : clipPath_( clipPath ), startTime_( startTime ),
      startOffset_( startOffset ), duration_( duration ),
      loopLength_( loopLength ), stretch_( stretch )
{
}

SApplyResult SResizeClipAction::apply( SProject *project )
{
    if( !project || clipPath_.isEmpty() ) {
        return {false, nullptr};
    }
    SStdMixer *mixer = dynamic_cast<SStdMixer*>( project->getRootComponent() );
    if( !mixer ) {
        return {false, nullptr};
    }
    QList<int> trackPath = clipPath_;
    int idx = trackPath.takeLast();
    STrack *track = dynamic_cast<STrack*>( resolveByPath( mixer, trackPath ) );
    if( !track ) {
        return {false, nullptr};
    }
    SLink *link = track->childAt( idx );
    if( !link ) {
        return {false, nullptr};
    }
    SCut *cut = dynamic_cast<SCut*>( &link->getSObject() );
    if( !cut ) {
        return {false, nullptr};   // only SCut clips have a window to resize
    }

    // Capture the pre-mutation window for the inverse, then apply the new one.
    offset_t oldStart  = link->getStartTime();
    offset_t oldOffset = cut->getStartOffset();
    length_t oldDur    = cut->getDuration();
    length_t oldLoop   = cut->getLoopLength();
    double   oldStretch = cut->getStretch();

    link->setStartTime( startTime_ );
    cut->setWindow( startOffset_, duration_, loopLength_, stretch_ );

    SAction *inverse = new SResizeClipAction( clipPath_, oldStart, oldOffset, oldDur,
                                              oldLoop, oldStretch );
    return {true, inverse};
}

void SResizeClipAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "clip", pathToString( clipPath_ ) );
    elem.setAttribute( "startTime", QString::fromStdString( Fraction(startTime_, 1).toString() ) );
    elem.setAttribute( "startOffset", QString::fromStdString( Fraction(startOffset_, 1).toString() ) );
    elem.setAttribute( "duration", QString::fromStdString( Fraction(duration_, 1).toString() ) );
    elem.setAttribute( "loopLength", QString::fromStdString( Fraction(loopLength_, 1).toString() ) );
    elem.setAttribute( "stretch", QString::number( stretch_ ) );
}

bool SResizeClipAction::readXml( const QDomElement &elem, int /*version*/ )
{
    clipPath_    = stringToPath( elem.attribute( "clip" ) );
    startTime_   = (offset_t) parseFractionOrDouble( elem.attribute( "startTime", "0" ).toStdString() ).toDouble();
    startOffset_ = (offset_t) parseFractionOrDouble( elem.attribute( "startOffset", "0" ).toStdString() ).toDouble();
    duration_    = (length_t) parseFractionOrDouble( elem.attribute( "duration", "0" ).toStdString() ).toDouble();
    loopLength_  = (length_t) parseFractionOrDouble( elem.attribute( "loopLength", "0" ).toStdString() ).toDouble();
    stretch_     = elem.attribute( "stretch", "1.0" ).toDouble();
    return true;
}

static const bool s_reg_resizeclip = (
    SActionRegistry::instance().registerType(
        QStringLiteral("resize-clip"),
        []{ return new SResizeClipAction; }
    ), true
);
