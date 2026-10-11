#include "app/objects/cut/saddtakeaction.h"
#include "app/objects/cut/sremovetakeaction.h"
#include "app/objects/cut/stakestack.h"
#include "app/objects/cut/stakehelpers.h"
#include "app/objects/cut/scut.h"
#include "app/model/sobjectpath.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/model/slink.h"
#include "app/actions/sactionregistry.h"
#include "tw/core/twfraction.h"
#include "tw/sources/twgrainparams.h"
#include <QDomElement>

using namespace strackpath;

SAddTakeAction::SAddTakeAction( const QList<int> &clipPath,
                                const QString &filePath, offset_t startOffset,
                                int index, bool activate, const Fraction &stretch,
                                double pitchCents )
    : clipPath_( clipPath ), filePath_( filePath ),
      startOffset_( startOffset ), index_( index ), activate_( activate ),
      stretch_( stretch ), pitchCents_( pitchCents )
{
}

SApplyResult SAddTakeAction::apply( SProject *project )
{
    if( !project || clipPath_.isEmpty() || filePath_.isEmpty() ) {
        return {false, nullptr};
    }
    SObject *mixer = splacements::rootNamed( project, pathRoot_ );
    if( !mixer ) {
        return {false, nullptr};
    }
    QList<int> lanePath = clipPath_;
    int idx = lanePath.takeLast();
    SObject *lane = splacements::placementLaneAt( mixer, lanePath, "add-take" );
    SLink *link = lane ? lane->childAt( idx ) : nullptr;
    if( !link || link->getSObject().isLane() ) {
        return {false, nullptr};
    }

    // BOTH SHAPES (proposal 42 M3). The bare cast this replaced matched the
    // DIRECT one only, so on a WRAPPED column the wrapper looked like a plain
    // clip and `wrapCutLinkIntoStack` -- which needs only an `SClipWindow` --
    // happily succeeded on it, producing a COLUMN INSIDE A COLUMN: the new
    // take became a sibling of the entire old column rather than of its takes.
    // Measured: a two-take column whose take 0 was itself a two-take column.
    STakeStack *stack = stakes::columnOfLink( link );
    if( !stack ) {
        // A plain cut becomes a single-take stack first (take 0 = the cut).
        link = stakes::wrapCutLinkIntoStack( project, lane, link );
        stack = stakes::columnOfLink( link );
        if( !stack ) {
            return {false, nullptr};   // clip was not an SCut
        }
    }

    QString mutablePath = filePath_;
    SLink *wavLink = project->linkToFile( mutablePath );
    if( !wavLink ) {
        return {false, nullptr};
    }

    const int prevActive = stack->activeTakeIndex();

    // The take cut: same window duration as the stack (invariant 1), its own
    // slip and grain params. Grain params go in RAW — like split-clip, the
    // caller supplies final values (setGrainParams would preserve-span-
    // rescale offset/duration, which double-applies the factor here).
    SCut *takeCut = new SCut( project, wavLink->getSObject() );
    delete wavLink;   // temp link; the cut holds its own ref on the wave
    twGrainParams gp = takeCut->getGrainParams();
    gp.stretch = stretch_;
    gp.pitchCents = pitchCents_;
    takeCut->setGrainParamsRaw( gp );
    takeCut->setStartOffset( startOffset_ );
    // Blocking (P19): the new take must adopt the column's CURRENT duration,
    // never the stale try-lock fallback (edit path, bounded block).
    takeCut->setDuration( stack->getDurationBlocking() );

    const int at = ( index_ >= 0 && index_ <= stack->nTakes() )
                       ? index_ : stack->nTakes();
    if( !stack->insertTake( *takeCut, at == stack->nTakes() ? -1 : at ) ) {
        // Homogeneity (proposal 37 D8b): the column already holds a different
        // kind of material. Reject rather than build a column that plays
        // audio or notes depending on which lane is active.
        delete takeCut;
        return {false, nullptr};
    }
    // ACTIVATING A TAKE CLEARS THE COMP MAP (plan 53 B4).
    //
    // Without this the activation is a lie the UI tells: twCompMap::takeAt
    // answers -1 only BEFORE the first segment, so a map that starts at 0
    // covers the whole column and decides every position itself. The newly
    // activated take is then badged active and COMPLETELY INAUDIBLE -- the
    // user records a take, sees it selected, and hears the old one.
    //
    // Clearing is the right semantic rather than mapping the new take over
    // the comped span: an empty map MEANS "ask activeTake_", which is exactly
    // what activating asked for. Take extents (plan 53 §4.1) will give the
    // recorded span its own segment; that is A0a's job, not this one's.
    const twCompMap prevMap = stack->compMap();
    if( activate_ ) {
        if( !prevMap.empty() ) {
            stack->setCompMap( twCompMap{} );
        }
        stack->setActiveTake( at );
    }

    // Inverse: remove the take again; restore the previous selection when we
    // changed it (removeTake's own bookkeeping handles the inactive case).
    QList<int> stackPath = lanePath;
    stackPath.append( lane->indexOfChild( link ) );
    SRemoveTakeAction *inverse = new SRemoveTakeAction(
        stackPath, at, activate_ ? prevActive : -2 );
    // The map is NOT derivable from the surviving takes, so only this action
    // can hand it back. Carried whenever we cleared one.
    if( activate_ && !prevMap.empty() ) {
        inverse->setRestoreCompMap( prevMap );
    }
    return {true, inverse};
}

void SAddTakeAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "clip", qualifiedToString( pathRoot_, clipPath_ ) );
    elem.setAttribute( "filePath", filePath_ );
    elem.setAttribute( "startOffset", QString::fromStdString(
                           Fraction( startOffset_, 1 ).toString() ) );
    elem.setAttribute( "index", index_ );
    elem.setAttribute( "activate", activate_ ? 1 : 0 );
    elem.setAttribute( "stretch", QString::fromStdString( stretch_.toString() ) );
    elem.setAttribute( "pitchCents", QString::number( pitchCents_ ) );
}

bool SAddTakeAction::readXml( const QDomElement &elem, int /*version*/ )
{
    clipPath_ = parseInto( pathRoot_, elem.attribute( "clip" ) );
    filePath_ = elem.attribute( "filePath", "" );
    startOffset_ = (offset_t)parseFractionOrDouble(
        elem.attribute( "startOffset", "0" ).toStdString() ).toDouble();
    index_ = elem.attribute( "index", "-1" ).toInt();
    activate_ = elem.attribute( "activate", "1" ).toInt() != 0;
    stretch_ = parseFractionOrDouble( elem.attribute( "stretch", "1" ).toStdString() );
    pitchCents_ = elem.attribute( "pitchCents", "0" ).toDouble();
    return true;
}

static const bool s_reg_addtake = (
    SActionRegistry::instance().registerType(
        QStringLiteral("add-take"),
        []{ return new SAddTakeAction; }
    ), true
);
