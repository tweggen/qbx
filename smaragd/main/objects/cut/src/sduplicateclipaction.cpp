#include "app/objects/cut/sduplicateclipaction.h"
#include "app/model/sobjectpath.h"
#include "app/model/splacements.h"
#include "app/objects/cut/sremoveclipaction.h"
#include "app/model/sproject.h"
#include "app/actions/sactionregistry.h"
#include "app/model/slink.h"
#include "app/model/sclipwindow.h"
#include "app/objects/cut/stakestack.h"
#include "app/objects/cut/stakehelpers.h"
#include "tw/core/twfraction.h"
#include <QDomElement>

using namespace strackpath;

SLink *makeDuplicateClip( SProject *project, SObject &srcObj,
                          SObject *destLane, offset_t startTime )
{
    if( !project || !destLane ) return nullptr;
    // A TAKE COLUMN IS DEEP-COPIED, never shared (proposal 42 M1). `SClipWindow
    // ::of` is null for an `STakeStack`, so this used to fall to `wrapContent`
    // and mint an `SCut`/`SMidiCut` over the SAME stack -- producing the wrapped
    // shape AND a column with two placements, which share one `activeTake_`.
    // Comping either one then comped both, which reads exactly like comping
    // being broken.
    if( STakeStack *column = dynamic_cast<STakeStack *>( &srcObj ) ) {
        STakeStack *dup = stakes::cloneColumn( project, *column );
        if( !dup ) return nullptr;
        SLink *link = new SLink( *dup, NULL );
        link->setStartTime( startTime );
        link->setParent( destLane );
        return link;
    }
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

SLink *makeSharedLink( SProject *project, SObject &srcObj,
                       SObject *destLane, offset_t startTime )
{
    if( !project || !destLane ) return nullptr;
    // A TAKE COLUMN IS DEEP-COPIED EVEN HERE (proposal 42 M1). See the header:
    // two placements of one `STakeStack` share its `activeTake_`, so comping
    // either would comp both. The rule is the column's, not the gesture's.
    if( STakeStack *column = dynamic_cast<STakeStack *>( &srcObj ) ) {
        STakeStack *dup = stakes::cloneColumn( project, *column );
        if( !dup ) return nullptr;
        SLink *link = new SLink( *dup, NULL );
        link->setStartTime( startTime );
        link->setParent( destLane );
        return link;
    }
    // ONE MORE PLACEMENT OF THE SAME OBJECT - no window copy, no content copy.
    // That is the whole difference from makeDuplicateClip, and it is why an
    // edit through either placement shows in both.
    SLink *link = new SLink( srcObj, NULL );
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
    if( !srcLink || (srcLink->getSObject().isLane() ) ) {
        return {false, nullptr};   // missing, or a nested track lane (not a clip)
    }

    SObject *destTrack = splacements::placementLaneAt( mixer, destTrackPath_, "duplicate-clip" );
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
                                              destTrackPath_, startTime_,
                                              /*share=*/false );
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

// --- link-clip (QBX-217) ----------------------------------------------------
//
// Everything here mirrors SDuplicateClipAction::apply() line for line, and the
// two SHOULD be read side by side: the source resolution and its lane refusal,
// the destination through `placementLaneAt` (proposal 45 D6), the created-path
// out-param, the inverse. Only the maker differs.

SLinkClipAction::SLinkClipAction( const QList<int> &sourceClipPath,
                                  const QList<int> &destTrackPath,
                                  offset_t startTime )
    : sourceClipPath_( sourceClipPath ), destTrackPath_( destTrackPath ),
      startTime_( startTime )
{
}

SApplyResult SLinkClipAction::apply( SProject *project )
{
    if( !project || sourceClipPath_.isEmpty() ) {
        return {false, nullptr};
    }
    SObject *mixer = splacements::rootNamed( project, pathRoot_ );
    if( !mixer ) {
        return {false, nullptr};
    }

    QList<int> srcTrackPath = sourceClipPath_;
    int srcIdx = srcTrackPath.takeLast();
    SObject *srcTrack = splacements::laneAt( mixer, srcTrackPath );
    if( !srcTrack ) {
        return {false, nullptr};
    }
    SLink *srcLink = srcTrack->childAt( srcIdx );
    // A LANE IS NOT A CLIP, and refusing it here is load-bearing rather than
    // defensive (QBX-217). The menu command this verb replaced had no such
    // check, and handing it a lane link appended a second placement of a whole
    // LANE: measured on 59ca0dfa, aiming the arranger menu at `$master` child
    // 0 - the Conductor lane, not a clip - left `$master,1` resolving as a
    // second placement of Conductor, with nothing on the undo stack to take it
    // back. `SDuplicateClipAction` has always refused this in the same line.
    if( !srcLink || ( srcLink->getSObject().isLane() ) ) {
        qWarning() << "link-clip: no clip at" << sourceClipPath_
                   << "(a lane is not a clip)";
        return {false, nullptr};
    }

    SObject *destTrack = splacements::placementLaneAt( mixer, destTrackPath_,
                                                       "link-clip" );
    if( !destTrack ) {
        return {false, nullptr};
    }

    SLink *link = makeSharedLink( project, srcLink->getSObject(), destTrack,
                                  startTime_ );
    if( !link ) {
        return {false, nullptr};
    }

    QList<int> newClipPath = destTrackPath_;
    newClipPath.append( destTrack->indexOfChild( link ) );
    if( createdPathOut_ ) *createdPathOut_ = newClipPath;
    SAction *inverse = new SRemoveClipAction( newClipPath, sourceClipPath_,
                                              destTrackPath_, startTime_,
                                              /*share=*/true );
    return {true, inverse};
}

void SLinkClipAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "source", qualifiedToString( pathRoot_, sourceClipPath_ ) );
    elem.setAttribute( "destTrack", qualifiedToString( pathRoot_, destTrackPath_ ) );
    elem.setAttribute( "startTime", QString::fromStdString( Fraction(startTime_, 1).toString() ) );
}

bool SLinkClipAction::readXml( const QDomElement &elem, int /*version*/ )
{
    sourceClipPath_ = parseInto( pathRoot_, elem.attribute( "source" ) );
    destTrackPath_  = parseInto( pathRoot_, elem.attribute( "destTrack" ) );
    startTime_      = (offset_t) parseFractionOrDouble( elem.attribute( "startTime", "0" ).toStdString() ).toDouble();
    return true;
}

static const bool s_reg_linkclip = (
    SActionRegistry::instance().registerType(
        QStringLiteral("link-clip"),
        []{ return new SLinkClipAction; }
    ), true
);

static const bool s_reg_duplicateclip = (
    SActionRegistry::instance().registerType(
        QStringLiteral("duplicate-clip"),
        []{ return new SDuplicateClipAction; }
    ), true
);
