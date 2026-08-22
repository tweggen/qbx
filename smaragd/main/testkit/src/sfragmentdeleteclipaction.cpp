#include "app/testkit/sfragmentdeleteclipaction.h"

#include "app/actions/sactionregistry.h"
#include "app/model/sclipwindow.h"
#include "app/model/sexternfile.h"
#include "app/model/sobjectpath.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/model/slink.h"
#include "app/objects/cut/scut.h"

#include <QDebug>
#include <QDomElement>

using namespace strackpath;

// ---------------------------------------------------------------------------
// delete-fragment-clip
// ---------------------------------------------------------------------------

SApplyResult SFragmentDeleteClipAction::apply( SProject *project )
{
    if( !project || clipPath_.isEmpty() ) {
        qWarning() << "delete-fragment-clip: no clip path";
        return { false, nullptr };
    }

    SObject *root = splacements::rootNamed( project, pathRoot_ );
    // placementAt() is the SAME resolver set-clip-volume/resize-clip/
    // slip-clip use (proposal 41 M2b): it resolves the clip's PARENT
    // through containerAt() -- a strictly wider predicate than isLane(),
    // so a clip already nested inside a fragment is reachable here exactly
    // as it is reachable to every other clip-property verb.
    SLink *link = root ? splacements::placementAt( root, clipPath_ ) : nullptr;
    if( !link ) {
        qWarning() << "delete-fragment-clip: no clip at path"
                   << qualifiedToString( pathRoot_, clipPath_ );
        return { false, nullptr };
    }

    SClipWindow *win = SClipWindow::of( &link->getSObject() );
    SExternFile *xf = win ? dynamic_cast<SExternFile *>( &win->windowContent() )
                          : nullptr;
    if( !win || !xf ) {
        qWarning() << "delete-fragment-clip: target at"
                   << qualifiedToString( pathRoot_, clipPath_ )
                   << "is not an audio (SExternFile-backed) clip";
        return { false, nullptr };
    }

    // Capture everything the inverse needs BEFORE the link dies -- same
    // discipline SRemoveSampleAction uses.
    const QString filePath = xf->getFileName();
    const Fraction srcStart = win->contentAnchorExact();
    const length_t cutDuration = win->durationBlocking();
    const length_t loopLength = win->loopLength();
    const offset_t timePos = link->getStartTime();
    twGrainParams grain;
    if( SCut *cut = dynamic_cast<SCut *>( &link->getSObject() ) )
        grain = cut->getGrainParams();

    QList<int> containerPath = clipPath_;
    containerPath.takeLast();

    SFragmentRestoreClipAction *inverse = new SFragmentRestoreClipAction(
        containerPath, timePos, filePath, srcStart, cutDuration, loopLength,
        grain );
    inverse->setPathRoot( pathRoot_ );

    // The cut becomes unreferenced -> deleteLater. If this fragment is
    // placed N times, this is the ONLY child link addressed -- the fragment
    // itself, and therefore every OTHER placement's view of it, loses the
    // child too, because every placement shares this ONE fragment (D2).
    delete link;

    return { true, inverse };
}

void SFragmentDeleteClipAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "clip", qualifiedToString( pathRoot_, clipPath_ ) );
}

bool SFragmentDeleteClipAction::readXml( const QDomElement &elem, int )
{
    clipPath_ = parseInto( pathRoot_, elem.attribute( "clip" ) );
    if( clipPath_.isEmpty() ) {
        qWarning() << "delete-fragment-clip::readXml: missing or empty clip path";
        return false;
    }
    return true;
}

static const bool s_reg_delete_fragment_clip = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "delete-fragment-clip" ),
        []{ return new SFragmentDeleteClipAction; }
    ), true
);

// ---------------------------------------------------------------------------
// restore-fragment-clip (live-only inverse)
// ---------------------------------------------------------------------------

SFragmentRestoreClipAction::SFragmentRestoreClipAction(
        const QList<int> &containerPath, offset_t timePos,
        const QString &filePath, const Fraction &srcStart,
        length_t cutDuration, length_t loopLength,
        const twGrainParams &grain )
    : containerPath_( containerPath ), timePos_( timePos ),
      filePath_( filePath ), srcStart_( srcStart ),
      cutDuration_( cutDuration ), loopLength_( loopLength ), grain_( grain )
{
}

SApplyResult SFragmentRestoreClipAction::apply( SProject *project )
{
    if( !project ) return { false, nullptr };

    SObject *root = splacements::rootNamed( project, pathRoot_ );
    // containerAt(), not laneAt(): the container being restored into may be
    // a fragment (proposal 41 M2b) as readily as an ordinary lane.
    SObject *container = root
        ? splacements::containerAt( root, containerPath_ ) : nullptr;
    if( !container ) {
        qWarning() << "restore-fragment-clip: container gone at"
                   << qualifiedToString( pathRoot_, containerPath_ );
        return { false, nullptr };
    }

    QString mutablePath = filePath_;
    SLink *wavLink = project->linkToFile( mutablePath );
    if( !wavLink ) return { false, nullptr };
    SCut *cut = new SCut( project, wavLink->getSObject() );
    delete wavLink;

    cut->setGrainParamsRaw( grain_ );
    cut->setWindow( srcStart_, ClipLen( cutDuration_ ),
                    WarpedLen( loopLength_ ), grain_.stretch );

    SLink *link = new SLink( *cut, nullptr );
    link->setStartTime( timePos_ );
    link->setParent( container );

    if( container->indexOfChild( link ) < 0 ) return { false, nullptr };

    // No inverse of our own: this action only ever runs AS an inverse, and
    // SActionUndoCommand::redo() re-applies the FORWARD action (the delete)
    // -- SRestoreContainerClipAction's own precedent.
    return { true, nullptr };
}

void SFragmentRestoreClipAction::writeXml( QDomElement & ) const
{
    // Live-only action -- never serialized. Intentionally empty.
}

bool SFragmentRestoreClipAction::readXml( const QDomElement &, int )
{
    // Live-only action -- never deserialized.
    return false;
}
