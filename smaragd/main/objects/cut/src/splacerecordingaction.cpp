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
                                              offset_t timePos )
    : trackPath_( trackPath ), filePath_( filePath ), timePos_( timePos )
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
    if( !wavLink || !wavLink->getSObject().hasDuration() ) {
        return {false, nullptr};
    }
    const length_t waveDur = wavLink->getSObject().getDuration();
    if( waveDur == 0 ) {
        return {false, nullptr};
    }
    const offset_t recStart = timePos_;
    const offset_t recEnd = recStart + (offset_t)waveDur;

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
        const length_t d = lk->getSObject().getDuration();
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
                    cursor - recStart, (length_t)( col.start - cursor ) ) );
            }
            composite.append( new SAddTakeAction(
                col.path, filePath_, col.start - recStart ) );
        }
        cursor = std::max( cursor, std::min( colEnd, recEnd ) );
    }
    if( cursor < recEnd ) {
        // Trailing gap; when nothing overlapped at all this is the whole
        // file at timePos — today's plain placement.
        composite.append( new SPlaceClipAction(
            trackPath_, filePath_, cursor, cursor - recStart,
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
}

bool SPlaceRecordingAction::readXml( const QDomElement &elem, int /*version*/ )
{
    trackPath_ = stringToPath( elem.attribute( "trackPath" ) );
    filePath_ = elem.attribute( "filePath", "" );
    timePos_ = (offset_t)parseFractionOrDouble(
        elem.attribute( "timePos", "0" ).toStdString() ).toDouble();
    return true;
}

static const bool s_reg_placerecording = (
    SActionRegistry::instance().registerType(
        QStringLiteral("place-recording"),
        []{ return new SPlaceRecordingAction; }
    ), true
);
