#include "app/objects/cut/splacerecordingaction.h"
#include "app/objects/cut/splaceclipaction.h"
#include "app/objects/cut/saddtakeaction.h"
#include "app/model/sobjectpath.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/model/slink.h"
#include "app/actions/sactionregistry.h"
#include "app/actions/scompositeaction.h"
#include "tw/core/twfraction.h"
#include <QDomElement>
#include <algorithm>

using namespace strackpath;

SPlaceRecordingAction::SPlaceRecordingAction( const QList<int> &trackPath,
                                              const QString &filePath,
                                              offset_t timePos,
                                              offset_t srcOffset,
                                              length_t length )
    : trackPath_( trackPath ), filePath_( filePath ), timePos_( timePos ),
      srcOffset_( srcOffset ), length_( length )
{
}

SApplyResult SPlaceRecordingAction::apply( SProject *project )
{
    if( !project || filePath_.isEmpty() ) {
        return {false, nullptr};
    }
    SObject *mixer = splacements::rootContainer( project );
    SObject *lane = splacements::laneAt( mixer, trackPath_ );
    if( !lane ) {
        return {false, nullptr};
    }
    QString mutablePath = filePath_;
    SLink *wavLink = project->linkToFile( mutablePath );
    if( !wavLink ) {
        return {false, nullptr};
    }
    // The link is a planning temp only (the planned place-clip/add-take
    // sub-actions create their own links); read the duration and release it —
    // an unparented leftover link would pin the wave forever.
    const bool hasDur = wavLink->getSObject().hasDuration();
    // Blocking reads throughout this planner (P19): the whole gap/take plan is
    // derived from these durations; a stale try-lock value mis-plans it.
    const length_t waveDur =
        hasDur ? wavLink->getSObject().getDurationBlocking() : 0;
    delete wavLink;
    if( !hasDur || waveDur == 0 ) {
        return {false, nullptr};
    }
    // The SUB-RANGE of the recording this call places (proposal 21 L3b): the
    // whole file by default, one loop pass when the recorder is cycling.
    offset_t srcOff = srcOffset_ < 0 ? 0 : srcOffset_;
    if( srcOff >= (offset_t) waveDur ) return {false, nullptr};
    length_t span = (length_t)( (offset_t) waveDur - srcOff );
    if( length_ > 0 && length_ < span ) span = length_;

    const offset_t recStart = timePos_;
    const offset_t recEnd = recStart + (offset_t)span;

    // The lane's columns overlapping the recording span. Paths are computed
    // NOW and stay valid through the plan: place-clip appends new links and
    // add-take wraps in place (index-preserving) — existing indices never
    // shift.
    struct Column { offset_t start; length_t dur; QList<int> path; };
    QList<Column> columns;
    for( int i = 0; i < lane->childCount(); ++i ) {
        SLink *lk = lane->childAt( i );
        if( !lk || lk->getSObject().isPathContainer() ) continue;  // sub-track
        if( !lk->getSObject().hasDuration() ) continue;
        const offset_t s = lk->getStartTime();
        const length_t d = lk->getSObject().getDurationBlocking();
        if( d == 0 || s + (offset_t)d <= recStart || s >= recEnd ) continue;
        if( s < recStart ) {
            // A column already running at the recording start is left alone
            // ("as applicable"); its span still consumes recording material.
            qWarning( "place-recording: column at %lld predates the "
                      "recording start, skipped", (long long)s );
        }
        columns.append( { s, d, QList<int>() } );
        columns.last().path = trackPath_;
        columns.last().path.append( i );
    }
    std::sort( columns.begin(), columns.end(),
               []( const Column &a, const Column &b )
               { return a.start < b.start; } );

    // Plan: takes for covered columns, plain cuts for the gaps.
    SCompositeAction composite;
    offset_t cursor = recStart;
    for( const Column &col : columns ) {
        const offset_t colEnd = col.start + (offset_t)col.dur;
        if( col.start >= recStart ) {
            if( col.start > cursor ) {
                composite.append( new SPlaceClipAction(
                    trackPath_, filePath_, cursor,
                    srcOff + cursor - recStart,
                    (length_t)( col.start - cursor ) ) );
            }
            composite.append( new SAddTakeAction(
                col.path, filePath_, srcOff + col.start - recStart ) );
        }
        cursor = std::max( cursor, std::min( colEnd, recEnd ) );
    }
    if( cursor < recEnd ) {
        // Trailing gap; when nothing overlapped at all this is the whole
        // file at timePos — today's plain placement.
        composite.append( new SPlaceClipAction(
            trackPath_, filePath_, cursor, srcOff + cursor - recStart,
            (length_t)( recEnd - cursor ) ) );
    }

    return composite.apply( project );
}

void SPlaceRecordingAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "trackPath", pathToString( trackPath_ ) );
    elem.setAttribute( "filePath", filePath_ );
    elem.setAttribute( "timePos", QString::fromStdString(
                           Fraction( timePos_, 1 ).toString() ) );
    // Only when non-default: the round-trip audit requires that an attribute a
    // fixture does not declare must not appear.
    if( srcOffset_ != 0 )
        elem.setAttribute( "srcOffset", QString::fromStdString(
                               Fraction( srcOffset_, 1 ).toString() ) );
    if( length_ != 0 )
        elem.setAttribute( "length", QString::fromStdString(
                               Fraction( length_, 1 ).toString() ) );
}

bool SPlaceRecordingAction::readXml( const QDomElement &elem, int /*version*/ )
{
    trackPath_ = stringToPath( elem.attribute( "trackPath" ) );
    filePath_ = elem.attribute( "filePath", "" );
    timePos_ = (offset_t)parseFractionOrDouble(
        elem.attribute( "timePos", "0" ).toStdString() ).toDouble();
    srcOffset_ = (offset_t)parseFractionOrDouble(
        elem.attribute( "srcOffset", "0" ).toStdString() ).toDouble();
    length_ = (length_t)parseFractionOrDouble(
        elem.attribute( "length", "0" ).toStdString() ).toDouble();
    return true;
}

static const bool s_reg_placerecording = (
    SActionRegistry::instance().registerType(
        QStringLiteral("place-recording"),
        []{ return new SPlaceRecordingAction; }
    ), true
);
