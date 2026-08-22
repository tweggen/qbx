#include "app/objects/cut/sresizeclipaction.h"
#include "app/model/sobjectpath.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/actions/sactionregistry.h"
#include "app/model/slink.h"
#include "app/model/sclipwindow.h"
#include "app/objects/cut/scut.h"          // warp anchors only (audio-specific)
#include "app/objects/cut/stakestack.h"
#include "app/model/seditgroups.h"
#include "app/actions/scompositeaction.h"
#include "tw/core/twfraction.h"
#include "tw/core/twlog.h"
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
                SObject *mixer = splacements::rootNamed( project, pathRoot_ );
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
    SObject *mixer = splacements::rootNamed( project, pathRoot_ );
    if( !mixer ) {
        return {false, nullptr};
    }
    // Proposal 41 M2b: resolve through placementAt(), not an inline
    // laneAt()+childAt() duplicate of it -- the inline form used to require
    // the clip's immediate parent to be a LANE, which is exactly what kept
    // resize-clip from reaching a clip already packed into a fragment.
    SLink *link = splacements::placementAt( mixer, clipPath_ );
    if( !link ) {
        return {false, nullptr};
    }
    // Take stack: length/loop/stretch write through to every take; the slip
    // targets one take (take_, -1 = active). Decision 3's group sync happens
    // one level up (broadcast layer, phase 4) — this action stays single-clip.
    if( STakeStack *stack = dynamic_cast<STakeStack*>( &link->getSObject() ) ) {
        int t = ( take_ >= 0 ) ? take_ : stack->activeTakeIndex();
        SClipWindow *takeWin = stack->takeAt( t );  // may be null (no active take)
        // Warp anchors are audio-only (cut/CONTRACT invariant 4), so this is
        // the one place the take's concrete type still matters.
        SCut *takeCut = takeWin ? dynamic_cast<SCut*>( &takeWin->asObject() )
                                : nullptr;

        offset_t oldStart  = link->getStartTime();
        Fraction oldAnchor = takeWin ? takeWin->contentAnchorExact() : Fraction(0);
        // Blocking (P19): a stale oldDur would bake a wrong window into the
        // inverse action (edit path, bounded block).
        length_t oldDur    = stack->getDurationBlocking();
        length_t oldLoop   = takeWin ? takeWin->loopLength() : 0;
        Fraction oldStretch = takeWin ? takeWin->stretchOrRate() : Fraction(1);

        std::vector<twWarpAnchor> oldAnchors =
            takeCut ? takeCut->getGrainParams().warpAnchors
                    : std::vector<twWarpAnchor>();

        link->setStartTime( startTime_ );
        stack->applyWindowAll( duration_, loopLength_, stretch_ );
        if( takeWin ) {
            takeWin->setWindowExact( srcStart_, duration_, loopLength_,
                                     stretch_ );
            if( setAnchors_ && takeCut ) takeCut->setWarpAnchors( anchors_ );
        }

        SResizeClipAction *inverse = new SResizeClipAction(
            clipPath_, oldStart, oldAnchor, oldDur, oldLoop, oldStretch,
            take_ );
        if( setAnchors_ ) inverse->setWarpAnchors( oldAnchors );
        return {true, inverse};
    }

    SClipWindow *win = SClipWindow::of( &link->getSObject() );
    if( !win ) {
        return {false, nullptr};   // only a windowed clip has a window to resize
    }
    // Warp anchors are audio-only (cut/CONTRACT invariant 4).
    SCut *cut = dynamic_cast<SCut*>( &win->asObject() );

    // Capture the pre-mutation window for the inverse, then apply the new one.
    offset_t oldStart  = link->getStartTime();
    Fraction oldAnchor = win->contentAnchorExact();
    length_t oldDur    = win->durationBlocking();   // edit path — never stale (P19)
    length_t oldLoop   = win->loopLength();
    Fraction oldStretch = win->stretchOrRate();

    std::vector<twWarpAnchor> oldAnchors =
        cut ? cut->getGrainParams().warpAnchors : std::vector<twWarpAnchor>();

    // PROPOSAL 41 D5: a rate != 1 on an event-exporting cut is REFUSED, not
    // approximated. POSITION_DOMAINS rule 7 — the tick/frame conversion for
    // a fragment's residual events already happened exactly once, inside its
    // own content's window(s); stretching the OUTER placement too would
    // convert a second time, in the frame domain, and the part would stop
    // following tempo (the Ardour <= 6 defect this whole model exists to
    // avoid). Checked here, at the EDIT surface, so the whole action is
    // refused loudly rather than silently degraded — never a Q_ASSERT (this
    // build compiles those out).
    if( cut && stretch_ != Fraction( 1 ) ) {
        twEventClipResolved residual = cut->getContent().resolveEventFeed( 0 );
        if( residual.seq && !residual.seq->empty() ) {
            TW_LOGW( "cut", "resize-clip: refusing stretch=%s on '%s' -- it "
                            "exports residual events (proposal 41 D5)",
                     stretch_.toString().c_str(), cut->getSName().toUtf8().constData() );
            return {false, nullptr};
        }
    }

    link->setStartTime( startTime_ );
    win->setWindowExact( srcStart_, duration_, loopLength_, stretch_ );
    if( setAnchors_ && cut ) cut->setWarpAnchors( anchors_ );

    SResizeClipAction *inverse = new SResizeClipAction(
        clipPath_, oldStart, oldAnchor, oldDur, oldLoop, oldStretch );
    if( setAnchors_ ) inverse->setWarpAnchors( oldAnchors );
    return {true, inverse};
}

void SResizeClipAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "clip", qualifiedToString( pathRoot_, clipPath_ ) );
    elem.setAttribute( "startTime", QString::fromStdString( Fraction(startTime_, 1).toString() ) );
    elem.setAttribute( "srcStart", QString::fromStdString( srcStart_.toString() ) );
    elem.setAttribute( "duration", QString::fromStdString( Fraction(duration_, 1).toString() ) );
    elem.setAttribute( "loopLength", QString::fromStdString( Fraction(loopLength_, 1).toString() ) );
    elem.setAttribute( "stretch", QString::fromStdString( stretch_.toString() ) );
    elem.setAttribute( "take", take_ );
    elem.setAttribute( "broadcast", broadcast_ ? 1 : 0 );
    if( setAnchors_ ) {
        QStringList toks;
        for( const twWarpAnchor &a : anchors_ )
            toks << QString( "%1:%2" ).arg( a.src ).arg( a.warped );
        elem.setAttribute( "warpAnchors", toks.join( "|" ) );
    }
}

bool SResizeClipAction::readXml( const QDomElement &elem, int /*version*/ )
{
    clipPath_    = parseInto( pathRoot_, elem.attribute( "clip" ) );
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
    // W1: attribute PRESENT (even empty) = replace the anchor list.
    if( elem.hasAttribute( "warpAnchors" ) ) {
        std::vector<twWarpAnchor> parsed;
        for( const QString &tok : elem.attribute( "warpAnchors" )
                 .split( "|", Qt::SkipEmptyParts ) ) {
            const QStringList kv = tok.split( ":" );
            if( kv.size() != 2 ) continue;
            bool okS = false, okW = false;
            const qint64 sv = kv[0].toLongLong( &okS );
            const qint64 wv = kv[1].toLongLong( &okW );
            if( okS && okW ) parsed.push_back( { (int64_t) sv, (int64_t) wv } );
        }
        setWarpAnchors( twWarpMap::sanitize( parsed ) );
    }
    return true;
}

static const bool s_reg_resizeclip = (
    SActionRegistry::instance().registerType(
        QStringLiteral("resize-clip"),
        []{ return new SResizeClipAction; }
    ), true
);
