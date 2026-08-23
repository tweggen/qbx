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
    SObject *mixer = splacements::rootNamed( project, pathRoot_ );
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
    // A NEGATIVE SOURCE OFFSET IS LEGAL AND MEANS LEADING SILENCE, which is
    // what lets a placement carry material that starts PARTWAY IN.
    //
    // It used to be clamped to 0 here, and that clamp is what made loop
    // recording unable to express a pass that STARTED LATE. A pass beginning
    // mid-cycle needs its material pushed later inside a take that spans the
    // whole cycle region -- i.e. exactly a negative anchor, which `SCut` has
    // supported since proposal 23 ("the clip then opens with silence and its
    // data starts later"). The machinery already handled a pass that ENDED
    // early, because running past the end of a sample is silence too; the
    // asymmetry was this one line.
    offset_t srcOff = srcOffset_;
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
        if( !lk || lk->getSObject().isLane() ) continue;  // sub-track
        if( !lk->getSObject().hasDuration() ) continue;
        const offset_t s = lk->getStartTime();
        const length_t d = lk->getSObject().getDurationBlocking();
        if( d == 0 || s + (offset_t)d <= recStart || s >= recEnd ) continue;
        if( s < recStart ) {
            // A column ALREADY RUNNING at the recording start is left alone,
            // and is dropped from the plan entirely rather than kept in it.
            //
            // IT USED TO BE KEPT, AND THAT SILENTLY THREW THE TAKE AWAY. A
            // column can only receive the recording as a TAKE when it starts
            // WITH it (a take is an alternative for the same window, so an
            // earlier-starting column would need source material from before
            // the recording began), and the plan loop below duly skips it --
            // but it still advanced `cursor` past the column's end, so the
            // column CONSUMED the recording material it covered. When such a
            // column covers the whole recording, cursor reached recEnd, the
            // trailing-gap branch did not fire, and the composite came out
            // EMPTY. An empty composite applies as SUCCESS, so the take was
            // discarded with nothing but this warning to show for it.
            //
            // That is not a corner: two takes recorded from the same locator
            // land a few thousand frames apart (each record-start re-anchors
            // its own placement conversion), so whether take 2 begins just
            // before or just after take 1 is wall-clock jitter -- and only the
            // second spelling lost the audio. qxa.record_stays_armed failed
            // that way in roughly 1 run in 15.
            //
            // Dropping it makes the recording fall through to the SAME
            // trailing-gap branch an empty lane uses, so the take is placed as
            // its own clip, overlapping the older one. Overlapping clips are
            // what the lane already holds whenever two takes do not line up;
            // losing recorded audio is not something a recorder may do.
            qWarning( "place-recording: column at %lld predates the recording "
                      "start; it keeps its span and the take is placed over it",
                      (long long)s );
            continue;
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
        // Always true now: the collection loop drops a column that starts
        // before recStart instead of carrying it into the plan. Kept as the
        // statement of what a column in here means.
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
    elem.setAttribute( "trackPath", qualifiedToString( pathRoot_, trackPath_ ) );
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
    trackPath_ = parseInto( pathRoot_, elem.attribute( "trackPath" ) );
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
