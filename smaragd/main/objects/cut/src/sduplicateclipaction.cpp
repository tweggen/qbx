#include "app/objects/cut/sduplicateclipaction.h"
#include "app/model/sobjectpath.h"
#include "app/model/splacements.h"
#include "app/objects/cut/sremoveclipaction.h"
#include "app/model/sproject.h"
#include "app/actions/sactionregistry.h"
#include "app/model/slink.h"
#include "app/model/sclipwindow.h"
#include "tw/core/twfraction.h"
#include <QDomElement>

using namespace strackpath;

SLink *makeDuplicateClip( SProject *project, SObject &srcObj,
                          SObject *destLane, offset_t startTime )
{
    if( !project || !destLane ) return nullptr;
    SClipWindow *copy;
    if( SClipWindow *src = SClipWindow::of( &srcObj ) ) {
        // Copy the WHOLE window faithfully — cloneWindowOver shares the same
        // content and reproduces every window value in one place (the slip
        // lives in the stretched output domain, so copying it without the
        // stretch would land the copy elsewhere in the source, unstretched).
        copy = src->cloneWindowOver( project );
    } else {
        copy = SClipWindow::wrapContent( project, srcObj );  // wrap raw content
    }
    if( !copy ) return nullptr;
    SLink *link = new SLink( copy->asObject(), NULL );
    link->setStartTime( startTime );
    link->setParent( destLane );
    return link;
}

SDuplicateClipAction::SDuplicateClipAction( const QList<int> &sourceClipPath,
                                            const QList<int> &destTrackPath,
                                            offset_t startTime )
    : sourceClipPath_( sourceClipPath ), destTrackPath_( destTrackPath ),
      startTime_( startTime )
{
}

SApplyResult SDuplicateClipAction::apply( SProject *project )
{
    if( !project || sourceClipPath_.isEmpty() ) {
        return {false, nullptr};
    }
    SObject *mixer = splacements::rootNamed( project, pathRoot_ );
    if( !mixer ) {
        return {false, nullptr};
    }

    // Resolve the source clip.
    QList<int> srcTrackPath = sourceClipPath_;
    int srcIdx = srcTrackPath.takeLast();
    SObject *srcTrack = splacements::laneAt( mixer, srcTrackPath );
    if( !srcTrack ) {
        return {false, nullptr};
    }
    SLink *srcLink = srcTrack->childAt( srcIdx );
    if( !srcLink || (srcLink->getSObject().isPathContainer() ) ) {
        return {false, nullptr};   // missing, or a nested track lane (not a clip)
    }

    SObject *destTrack = splacements::laneAt( mixer, destTrackPath_ );
    if( !destTrack ) {
        return {false, nullptr};
    }

    SLink *copy = makeDuplicateClip( project, srcLink->getSObject(), destTrack, startTime_ );
    if( !copy ) {
        return {false, nullptr};
    }

    QList<int> newClipPath = destTrackPath_;
    newClipPath.append( destTrack->indexOfChild( copy ) );
    if( createdPathOut_ ) *createdPathOut_ = newClipPath;
    SAction *inverse = new SRemoveClipAction( newClipPath, sourceClipPath_,
                                              destTrackPath_, startTime_ );
    return {true, inverse};
}

void SDuplicateClipAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "source", qualifiedToString( pathRoot_, sourceClipPath_ ) );
    elem.setAttribute( "destTrack", qualifiedToString( pathRoot_, destTrackPath_ ) );
    elem.setAttribute( "startTime", QString::fromStdString( Fraction(startTime_, 1).toString() ) );
}

bool SDuplicateClipAction::readXml( const QDomElement &elem, int /*version*/ )
{
    sourceClipPath_ = parseInto( pathRoot_, elem.attribute( "source" ) );
    destTrackPath_  = parseInto( pathRoot_, elem.attribute( "destTrack" ) );
    startTime_      = (offset_t) parseFractionOrDouble( elem.attribute( "startTime", "0" ).toStdString() ).toDouble();
    return true;
}

static const bool s_reg_duplicateclip = (
    SActionRegistry::instance().registerType(
        QStringLiteral("duplicate-clip"),
        []{ return new SDuplicateClipAction; }
    ), true
);
