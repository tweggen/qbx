#include "app/objects/cut/sresizeclipaction.h"
#include "app/model/sobjectpath.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/actions/sactionregistry.h"
#include "app/model/slink.h"
#include "app/objects/cut/scut.h"
#include "app/objects/cut/stakestack.h"
#include "app/model/seditgroups.h"
#include "app/actions/scompositeaction.h"
#include "tw/core/twfraction.h"
#include <QDomElement>

using namespace strackpath;

SResizeClipAction::SResizeClipAction( const QList<int> &clipPath,
                                      offset_t startTime, const Fraction &srcStart,
                                      length_t duration, length_t loopLength,
                                      const Fraction &stretch, int take, bool broadcast )
    : clipPath_( clipPath ), startTime_( startTime ),
      srcStart_( srcStart ), duration_( duration ),
      loopLength_( loopLength ), stretch_( stretch ), take_( take ),
      broadcast_( broadcast )
{
}

SApplyResult SResizeClipAction::apply( SProject *project )
{
    if( !project || clipPath_.isEmpty() ) {
        return {false, nullptr};
    }
    // Edit-group broadcast. The slip syncs to the CORRESPONDING take across
    // members (decision 3: drum-timing fix), so an active-take anchor (-1)
    // resolves to its explicit index before fanning out.
    if( broadcast_ ) {
        QList<QList<int>> targets =
            seditgroups::expandClipPaths( project, clipPath_ );
        if( targets.size() > 1 ) {
            int t = take_;
            if( t < 0 ) {
                SObject *mixer = splacements::rootContainer( project );
                if( SLink *anchor = splacements::placementAt( mixer, clipPath_ ) ) {
                    if( STakeStack *stack = dynamic_cast<STakeStack*>(
                            &anchor->getSObject() ) )
                        t = stack->activeTakeIndex();
                }
            }
            SCompositeAction composite;
            for( const QList<int> &p : targets ) {
                composite.append( new SResizeClipAction(
                    p, startTime_, srcStart_, duration_, loopLength_,
                    stretch_, t, false ) );
            }
            return composite.apply( project );
        }
    }
    SObject *mixer = splacements::rootContainer( project );
    if( !mixer ) {
        return {false, nullptr};
    }
    QList<int> trackPath = clipPath_;
    int idx = trackPath.takeLast();
    SObject *track = splacements::laneAt( mixer, trackPath );
    if( !track ) {
        return {false, nullptr};
    }
    SLink *link = track->childAt( idx );
    if( !link ) {
        return {false, nullptr};
    }
    // Take stack: length/loop/stretch write through to every take; the slip
    // targets one take (take_, -1 = active). Decision 3's group sync happens
    // one level up (broadcast layer, phase 4) — this action stays single-clip.
    if( STakeStack *stack = dynamic_cast<STakeStack*>( &link->getSObject() ) ) {
        int t = ( take_ >= 0 ) ? take_ : stack->activeTakeIndex();
        SCut *takeCut = stack->takeCutAt( t );   // may be null (no active take)

        offset_t oldStart  = link->getStartTime();
        Fraction oldAnchor = takeCut ? takeCut->getSrcStart() : Fraction(0);
        length_t oldDur    = stack->getDuration();
        length_t oldLoop   = takeCut ? takeCut->getLoopLength().frames() : 0;
        Fraction oldStretch = takeCut ? takeCut->getStretchExact() : Fraction(1);

        link->setStartTime( startTime_ );
        stack->applyWindowAll( duration_, loopLength_, stretch_ );
        if( takeCut ) {
            takeCut->setWindow( srcStart_, ClipLen( duration_ ),
                                WarpedLen( loopLength_ ), stretch_ );
        }

        SAction *inverse = new SResizeClipAction( clipPath_, oldStart, oldAnchor,
                                                  oldDur, oldLoop, oldStretch,
                                                  take_ );
        return {true, inverse};
    }

    SCut *cut = dynamic_cast<SCut*>( &link->getSObject() );
    if( !cut ) {
        return {false, nullptr};   // only SCut clips have a window to resize
    }

    // Capture the pre-mutation window for the inverse, then apply the new one.
    offset_t oldStart  = link->getStartTime();
    Fraction oldAnchor = cut->getSrcStart();
    length_t oldDur    = cut->getDuration();
    length_t oldLoop   = cut->getLoopLength().frames();
    Fraction oldStretch = cut->getStretchExact();

    link->setStartTime( startTime_ );
    cut->setWindow( srcStart_, ClipLen( duration_ ),
                    WarpedLen( loopLength_ ), stretch_ );

    SAction *inverse = new SResizeClipAction( clipPath_, oldStart, oldAnchor, oldDur,
                                              oldLoop, oldStretch );
    return {true, inverse};
}

void SResizeClipAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "clip", pathToString( clipPath_ ) );
    elem.setAttribute( "startTime", QString::fromStdString( Fraction(startTime_, 1).toString() ) );
    elem.setAttribute( "srcStart", QString::fromStdString( srcStart_.toString() ) );
    elem.setAttribute( "duration", QString::fromStdString( Fraction(duration_, 1).toString() ) );
    elem.setAttribute( "loopLength", QString::fromStdString( Fraction(loopLength_, 1).toString() ) );
    elem.setAttribute( "stretch", QString::fromStdString( stretch_.toString() ) );
    elem.setAttribute( "take", take_ );
    elem.setAttribute( "broadcast", broadcast_ ? 1 : 0 );
}

bool SResizeClipAction::readXml( const QDomElement &elem, int /*version*/ )
{
    clipPath_    = stringToPath( elem.attribute( "clip" ) );
    startTime_   = (offset_t) parseFractionOrDouble( elem.attribute( "startTime", "0" ).toStdString() ).toDouble();
    duration_    = (length_t) parseFractionOrDouble( elem.attribute( "duration", "0" ).toStdString() ).toDouble();
    loopLength_  = (length_t) parseFractionOrDouble( elem.attribute( "loopLength", "0" ).toStdString() ).toDouble();
    stretch_     = parseFractionOrDouble( elem.attribute( "stretch", "1" ).toStdString() );
    QString anchorStr = elem.attribute( "srcStart" );
    if( !anchorStr.isEmpty() ) {
        srcStart_ = parseFractionOrDouble( anchorStr.toStdString() );
    } else {
        // Legacy scripts/actions carry a warped-domain startOffset; migrate
        // by exact division through the (already parsed) stretch.
        Fraction warped = parseFractionOrDouble(
            elem.attribute( "startOffset", "0" ).toStdString() );
        srcStart_ = stretch_ > Fraction(0) ? warped / stretch_ : warped;
    }
    take_        = elem.attribute( "take", "-1" ).toInt();
    broadcast_   = elem.attribute( "broadcast", "1" ).toInt() != 0;
    return true;
}

static const bool s_reg_resizeclip = (
    SActionRegistry::instance().registerType(
        QStringLiteral("resize-clip"),
        []{ return new SResizeClipAction; }
    ), true
);
