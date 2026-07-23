#include "app/objects/cut/srestorecontainerclipaction.h"
#include "app/objects/cut/scut.h"
#include "app/model/splacements.h"
#include "app/model/sobjectpath.h"
#include "app/model/sproject.h"
#include "app/model/slink.h"
#include <QDomElement>

SRestoreContainerClipAction::SRestoreContainerClipAction(
        const QList<int> &lanePath, const QList<int> &containerPath,
        offset_t timePos, const Fraction &srcStart, length_t cutDuration,
        length_t loopLength, const twGrainParams &grain )
    : lanePath_( lanePath ), containerPath_( containerPath ),
      timePos_( timePos ), srcStart_( srcStart ), cutDuration_( cutDuration ),
      loopLength_( loopLength ), grain_( grain )
{
}

SApplyResult SRestoreContainerClipAction::apply( SProject *project )
{
    if( !project ) {
        return {false, nullptr};
    }
    SObject *root = splacements::rootContainer( project );
    SObject *lane = splacements::laneAt( root, lanePath_ );
    if( !lane ) {
        return {false, nullptr};
    }
    // The container the deleted clip windowed. If it is gone (its own track was
    // deleted after ours), there is nothing to rebuild over — refuse rather
    // than build a cut over the wrong object.
    SObject *container = strackpath::resolveByPath( root, containerPath_ );
    if( !container || !container->isPathContainer() ) {
        return {false, nullptr};
    }

    SCut *cut = new SCut( project, *container );
    // Same order as SDuplicateClipAction's clone: grain params RAW first (the
    // non-raw setters preserve-span-rescale and would move the window we are
    // about to set), then ONE setWindow to publish and rebuild the chain once.
    cut->setGrainParamsRaw( grain_ );
    cut->setWindow( srcStart_, ClipLen( cutDuration_ ),
                    WarpedLen( loopLength_ ), grain_.stretch );

    SLink *link = new SLink( *cut, nullptr );
    link->setStartTime( timePos_ );
    link->setParent( lane );

    if( lane->indexOfChild( link ) < 0 ) {
        return {false, nullptr};
    }

    // No inverse of our own: this action only ever runs AS an inverse, and
    // SActionUndoCommand::redo() re-applies the FORWARD action (the deletion)
    // rather than anything we return here — which is also why the redo path
    // needs no way to address a nested lane by index.
    return {true, nullptr};
}

void SRestoreContainerClipAction::writeXml( QDomElement & /*elem*/ ) const
{
    // Live-only action — never serialized. Intentionally empty.
}

bool SRestoreContainerClipAction::readXml( const QDomElement & /*elem*/,
                                           int /*version*/ )
{
    // Live-only action — never deserialized.
    return false;
}
