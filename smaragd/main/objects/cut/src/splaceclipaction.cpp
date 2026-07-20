#include "app/objects/cut/splaceclipaction.h"
#include "app/objects/cut/scut.h"
#include "app/model/sobjectpath.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/model/slink.h"
#include "app/actions/sactionregistry.h"
#include "tw/core/twfraction.h"
#include <QDomElement>

using namespace strackpath;

SPlaceClipAction::SPlaceClipAction( const QList<int> &trackPath,
                                    const QString &filePath, offset_t timePos,
                                    offset_t startOffset, length_t duration )
    : trackPath_( trackPath ), filePath_( filePath ), timePos_( timePos ),
      startOffset_( startOffset ), duration_( duration )
{
}

SApplyResult SPlaceClipAction::apply( SProject *project )
{
    if( !project || filePath_.isEmpty() ) {
        return {false, nullptr};
    }
    SObject *mixer = splacements::rootContainer( project );
    SObject *lane = splacements::laneAt( mixer, trackPath_ );
    if( !lane ) {
        return {false, nullptr};
    }
    QString mutablePath = filePath_;
    SLink *wavLink = project->linkToFile( mutablePath );
    if( !wavLink ) {
        return {false, nullptr};
    }

    SCut *cut = new SCut( project, wavLink->getSObject() );
    delete wavLink;   // temp link; the cut holds its own ref on the wave
    if( startOffset_ != 0 ) cut->setStartOffset( startOffset_ );
    if( duration_ > 0 ) cut->setDuration( duration_ );

    SLink *link = new SLink( *cut, nullptr );
    link->setStartTime( timePos_ );
    link->setParent( lane );

    QList<int> clipPath = trackPath_;
    clipPath.append( lane->indexOfChild( link ) );
    SAction *inverse = new SUnplaceClipAction( clipPath, trackPath_, filePath_,
                                               timePos_, startOffset_,
                                               duration_ );
    return {true, inverse};
}

void SPlaceClipAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "trackPath", pathToString( trackPath_ ) );
    elem.setAttribute( "filePath", filePath_ );
    elem.setAttribute( "timePos", QString::fromStdString(
                           Fraction( timePos_, 1 ).toString() ) );
    elem.setAttribute( "startOffset", QString::fromStdString(
                           Fraction( startOffset_, 1 ).toString() ) );
    elem.setAttribute( "duration", QString::fromStdString(
                           Fraction( duration_, 1 ).toString() ) );
}

bool SPlaceClipAction::readXml( const QDomElement &elem, int /*version*/ )
{
    trackPath_ = stringToPath( elem.attribute( "trackPath" ) );
    filePath_ = elem.attribute( "filePath", "" );
    timePos_ = (offset_t)parseFractionOrDouble(
        elem.attribute( "timePos", "0" ).toStdString() ).toDouble();
    startOffset_ = (offset_t)parseFractionOrDouble(
        elem.attribute( "startOffset", "0" ).toStdString() ).toDouble();
    duration_ = (length_t)parseFractionOrDouble(
        elem.attribute( "duration", "0" ).toStdString() ).toDouble();
    return true;
}

static const bool s_reg_placeclip = (
    SActionRegistry::instance().registerType(
        QStringLiteral("place-clip"),
        []{ return new SPlaceClipAction; }
    ), true
);

// ---------------------------------------------------------------------------

SUnplaceClipAction::SUnplaceClipAction( const QList<int> &clipPath,
                                        const QList<int> &trackPath,
                                        const QString &filePath,
                                        offset_t timePos, offset_t startOffset,
                                        length_t duration )
    : clipPath_( clipPath ), trackPath_( trackPath ), filePath_( filePath ),
      timePos_( timePos ), startOffset_( startOffset ), duration_( duration )
{
}

SApplyResult SUnplaceClipAction::apply( SProject *project )
{
    if( !project || clipPath_.isEmpty() ) {
        return {false, nullptr};
    }
    SObject *mixer = splacements::rootContainer( project );
    SLink *link = splacements::placementAt( mixer, clipPath_ );
    if( !link ) {
        return {false, nullptr};
    }
    delete link;    // the cut becomes unreferenced → deleteLater

    SAction *inverse = new SPlaceClipAction( trackPath_, filePath_, timePos_,
                                             startOffset_, duration_ );
    return {true, inverse};
}

void SUnplaceClipAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "clip", pathToString( clipPath_ ) );
    elem.setAttribute( "trackPath", pathToString( trackPath_ ) );
    elem.setAttribute( "filePath", filePath_ );
}

bool SUnplaceClipAction::readXml( const QDomElement & /*elem*/, int /*version*/ )
{
    return false;   // created live as place-clip's inverse
}
