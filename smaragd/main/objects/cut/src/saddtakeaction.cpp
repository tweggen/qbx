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
                                int index, bool activate, double stretch,
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
    SObject *mixer = splacements::rootContainer( project );
    if( !mixer ) {
        return {false, nullptr};
    }
    QList<int> lanePath = clipPath_;
    int idx = lanePath.takeLast();
    SObject *lane = splacements::laneAt( mixer, lanePath );
    SLink *link = lane ? lane->childAt( idx ) : nullptr;
    if( !link || link->getSObject().isPathContainer() ) {
        return {false, nullptr};
    }

    STakeStack *stack = dynamic_cast<STakeStack *>( &link->getSObject() );
    if( !stack ) {
        // A plain cut becomes a single-take stack first (take 0 = the cut).
        link = stakes::wrapCutLinkIntoStack( project, lane, link );
        stack = link ? dynamic_cast<STakeStack *>( &link->getSObject() )
                     : nullptr;
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
    SCut *takeCut = new SCut( project, *wavLink );
    twGrainParams gp = takeCut->getGrainParams();
    gp.stretch = stretch_;
    gp.pitchCents = pitchCents_;
    takeCut->setGrainParamsRaw( gp );
    takeCut->setStartOffset( startOffset_ );
    takeCut->setDuration( stack->getDuration() );

    const int at = ( index_ >= 0 && index_ <= stack->nTakes() )
                       ? index_ : stack->nTakes();
    stack->insertTake( *takeCut, at == stack->nTakes() ? -1 : at );
    if( activate_ ) {
        stack->setActiveTake( at );
    }

    // Inverse: remove the take again; restore the previous selection when we
    // changed it (removeTake's own bookkeeping handles the inactive case).
    QList<int> stackPath = lanePath;
    stackPath.append( lane->indexOfChild( link ) );
    SAction *inverse = new SRemoveTakeAction(
        stackPath, at, activate_ ? prevActive : -2 );
    return {true, inverse};
}

void SAddTakeAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "clip", pathToString( clipPath_ ) );
    elem.setAttribute( "filePath", filePath_ );
    elem.setAttribute( "startOffset", QString::fromStdString(
                           Fraction( startOffset_, 1 ).toString() ) );
    elem.setAttribute( "index", index_ );
    elem.setAttribute( "activate", activate_ ? 1 : 0 );
    elem.setAttribute( "stretch", QString::number( stretch_ ) );
    elem.setAttribute( "pitchCents", QString::number( pitchCents_ ) );
}

bool SAddTakeAction::readXml( const QDomElement &elem, int /*version*/ )
{
    clipPath_ = stringToPath( elem.attribute( "clip" ) );
    filePath_ = elem.attribute( "filePath", "" );
    startOffset_ = (offset_t)parseFractionOrDouble(
        elem.attribute( "startOffset", "0" ).toStdString() ).toDouble();
    index_ = elem.attribute( "index", "-1" ).toInt();
    activate_ = elem.attribute( "activate", "1" ).toInt() != 0;
    stretch_ = elem.attribute( "stretch", "1.0" ).toDouble();
    pitchCents_ = elem.attribute( "pitchCents", "0" ).toDouble();
    return true;
}

static const bool s_reg_addtake = (
    SActionRegistry::instance().registerType(
        QStringLiteral("add-take"),
        []{ return new SAddTakeAction; }
    ), true
);
