#include "actions/sdeleteclipaction.h"
#include "actions/strackpath.h"
#include "sproject.h"
#include "sactionregistry.h"
#include "sstdmixer.h"
#include "strack.h"
#include "slink.h"
#include "scut.h"
#include "sobject.h"
#include <QDomElement>

using namespace strackpath;

// ---------------------------------------------------------------------------
// SDeleteClipAction
// ---------------------------------------------------------------------------

SDeleteClipAction::SDeleteClipAction( const QList<int> &clipPath )
    : clipPath_( clipPath )
{
}

SApplyResult SDeleteClipAction::apply( SProject *project )
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
    if( !link || dynamic_cast<STrack*>( &link->getSObject() ) ) {
        return {false, nullptr};   // missing, or a nested track lane (not a clip)
    }

    // Snapshot the clip so the inverse can rebuild it faithfully. The recreate
    // action pins the (shared) content via addRef so it survives even when this
    // delete drops the content's last placement.
    SObject &obj = link->getSObject();
    offset_t startTime = link->getStartTime();
    SRecreateClipAction *inverse;
    if( SCut *cut = dynamic_cast<SCut*>( &obj ) ) {
        inverse = new SRecreateClipAction( trackPath, idx, &cut->getContent(), startTime,
                                           cut->getStartOffset(), cut->getDuration(),
                                           cut->getLoopLength(), cut->getGrainParams() );
    } else {
        inverse = new SRecreateClipAction( trackPath, idx, &obj, startTime );
    }

    delete link;   // the wrapped SObject drops a ref -> deleteLater if now unreferenced
    return {true, inverse};
}

void SDeleteClipAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "clip", pathToString( clipPath_ ) );
}

bool SDeleteClipAction::readXml( const QDomElement &elem, int /*version*/ )
{
    clipPath_ = stringToPath( elem.attribute( "clip" ) );
    return true;
}

static const bool s_reg_deleteclip = (
    SActionRegistry::instance().registerType(
        QStringLiteral("delete-clip"),
        []{ return new SDeleteClipAction; }
    ), true
);

// ---------------------------------------------------------------------------
// SRecreateClipAction (live-only inverse; pins the shared content)
// ---------------------------------------------------------------------------

SRecreateClipAction::SRecreateClipAction( const QList<int> &destTrackPath, int insertIndex,
                                          SObject *content, offset_t startTime,
                                          offset_t startOffset, length_t duration,
                                          length_t loopLength, const twGrainParams &grain )
    : destTrackPath_( destTrackPath ), insertIndex_( insertIndex ), content_( content ),
      isCut_( true ), startTime_( startTime ), startOffset_( startOffset ),
      duration_( duration ), loopLength_( loopLength ), grain_( grain )
{
    if( content_ ) content_->addRef();   // keep the content alive while undoable
}

SRecreateClipAction::SRecreateClipAction( const QList<int> &destTrackPath, int insertIndex,
                                          SObject *content, offset_t startTime )
    : destTrackPath_( destTrackPath ), insertIndex_( insertIndex ), content_( content ),
      isCut_( false ), startTime_( startTime )
{
    if( content_ ) content_->addRef();
}

SRecreateClipAction::~SRecreateClipAction()
{
    if( content_ ) content_->removeRef();
}

SApplyResult SRecreateClipAction::apply( SProject *project )
{
    if( !project || !content_ ) {
        return {false, nullptr};
    }
    SStdMixer *mixer = dynamic_cast<SStdMixer*>( project->getRootComponent() );
    if( !mixer ) {
        return {false, nullptr};
    }
    STrack *track = dynamic_cast<STrack*>( resolveByPath( mixer, destTrackPath_ ) );
    if( !track ) {
        return {false, nullptr};
    }

    SLink *link;
    if( isCut_ ) {
        // Rebuild the SCut over the same (shared) content with the saved window.
        // Mirror makeDuplicateClip: grain params first without rescale, then the
        // window (offset/duration/loop/stretch) via setWindow.
        SCut *cut = new SCut( project, *content_ );
        cut->setGrainParamsRaw( grain_ );
        cut->setWindow( startOffset_, duration_, loopLength_, grain_.stretch );
        link = new SLink( *cut, NULL );
    } else {
        link = new SLink( *content_, NULL );
    }
    link->setStartTime( startTime_ );
    link->setParent( track );   // childEvent appends to the end of the child order

    // Restore the original child slot: setParent appended the link, so move it
    // back to insertIndex_. This keeps the track's child order identical to
    // before the delete, so the absolute index paths on the undo stack (this
    // action's inverse, and any sibling deletes in the same macro) stay valid.
    int landed = track->indexOfChild( link );
    if( insertIndex_ >= 0 && insertIndex_ < track->childCount() && insertIndex_ != landed )
        track->moveChildToIndex( landed, insertIndex_ );

    QList<int> newClipPath = destTrackPath_;
    newClipPath.append( track->indexOfChild( link ) );
    return {true, new SDeleteClipAction( newClipPath )};
}

void SRecreateClipAction::writeXml( QDomElement & /*elem*/ ) const
{
    // Live-only: holds a raw content pointer that cannot be serialized.
}

bool SRecreateClipAction::readXml( const QDomElement & /*elem*/, int /*version*/ )
{
    return false;   // created live as a delete's inverse; not reconstructed here
}
