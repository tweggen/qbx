#include "app/objects/cut/sremovetakeaction.h"
#include "app/objects/cut/saddtakeaction.h"
#include "app/objects/cut/stakestack.h"
#include "app/objects/cut/stakehelpers.h"
#include "app/model/sclipwindow.h"
#include "app/objects/cut/scut.h"        // pitch only (audio-specific)
#include "app/model/sobjectpath.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/model/slink.h"
#include "app/model/sexternfile.h"
#include "app/actions/sactionregistry.h"
#include <QDomElement>

using namespace strackpath;

SRemoveTakeAction::SRemoveTakeAction( const QList<int> &clipPath,
                                      int takeIndex, int thenActivate )
    : clipPath_( clipPath ), takeIndex_( takeIndex ),
      thenActivate_( thenActivate )
{
}

SApplyResult SRemoveTakeAction::apply( SProject *project )
{
    if( !project || clipPath_.isEmpty() ) {
        return {false, nullptr};
    }
    SObject *mixer = splacements::rootNamed( project, pathRoot_ );
    if( !mixer ) {
        return {false, nullptr};
    }
    QList<int> lanePath = clipPath_;
    int idx = lanePath.takeLast();
    SObject *lane = splacements::laneAt( mixer, lanePath );
    SLink *link = lane ? lane->childAt( idx ) : nullptr;
    if( !link ) {
        return {false, nullptr};
    }
    // BOTH SHAPES (proposal 42 M3): DIRECT-only, this was simply REJECTED on
    // a wrapped column -- `remove-take` did nothing at all there.
    STakeStack *stack = stakes::columnOfLink( link );
    if( !stack || takeIndex_ < 0 || takeIndex_ >= stack->nTakes() ) {
        return {false, nullptr};
    }
    SClipWindow *take = stack->takeAt( takeIndex_ );
    if( !take ) {
        return {false, nullptr};
    }

    // Capture the take's window for the inverse. Only file-backed takes are
    // restorable; anything else yields a non-undoable (but valid) removal.
    QString filePath;
    if( SExternFile *xf =
            dynamic_cast<SExternFile *>( &take->windowContent() ) )
        filePath = xf->getFileName();
    const offset_t off = take->startOffset();
    const Fraction stretch = take->stretchOrRate();
    // Pitch is audio-only (cut/CONTRACT invariant 7), so the inverse carries
    // it only for an audio take; add-take is an audio verb anyway.
    SCut *audioCut = dynamic_cast<SCut *>( &take->asObject() );
    const double pitchCents = audioCut ? audioCut->getPitchCents() : 0.0;
    const bool wasActive = ( stack->activeTakeIndex() == takeIndex_ );

    stack->removeTake( takeIndex_ );
    if( thenActivate_ >= -1 ) {
        stack->setActiveTake( thenActivate_ );
    }

    // Invariant 3: a single remaining take collapses to a plain cut.
    SLink *resultLink = link;
    if( stack->nTakes() == 1 ) {
        resultLink = stakes::collapseSingleTakeStack( lane, link );
        if( !resultLink ) resultLink = link;
    }

    QList<int> invPath = lanePath;
    invPath.append( lane->indexOfChild( resultLink ) );
    SAction *inverse = filePath.isEmpty()
        ? nullptr
        : new SAddTakeAction( invPath, filePath, off, takeIndex_,
                              wasActive, stretch, pitchCents );
    return {true, inverse};
}

void SRemoveTakeAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "clip", qualifiedToString( pathRoot_, clipPath_ ) );
    elem.setAttribute( "take", takeIndex_ );
    elem.setAttribute( "thenActivate", thenActivate_ );
}

bool SRemoveTakeAction::readXml( const QDomElement &elem, int /*version*/ )
{
    clipPath_ = parseInto( pathRoot_, elem.attribute( "clip" ) );
    takeIndex_ = elem.attribute( "take", "0" ).toInt();
    thenActivate_ = elem.attribute( "thenActivate", "-2" ).toInt();
    return true;
}

static const bool s_reg_removetake = (
    SActionRegistry::instance().registerType(
        QStringLiteral("remove-take"),
        []{ return new SRemoveTakeAction; }
    ), true
);
