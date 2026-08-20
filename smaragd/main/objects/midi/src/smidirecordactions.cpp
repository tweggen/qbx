#include "app/objects/midi/smidirecordactions.h"

#include <algorithm>

#include <QDebug>
#include <QDomElement>

#include "app/actions/sactionregistry.h"
#include "app/actions/scompositeaction.h"
#include "app/model/sclipwindow.h"
#include "app/model/slink.h"
#include "app/model/sobjectpath.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/objects/midi/smidiactionsupport.h"
#include "app/objects/midi/smidiclipactions.h"
#include "app/objects/midi/smidicut.h"
#include "app/objects/midi/smidieventactions.h"

#include "tw/events/twtempomap.h"

using namespace strackpath;

namespace {

/** Every `<n .../>` / `<e .../>` child, sorted (the tables are kept sorted). */
std::vector<SEvent> readEvents( const QDomElement &elem )
{
    std::vector<SEvent> ev = smidiactions::readEventChildren( elem );
    std::stable_sort( ev.begin(), ev.end() );
    return ev;
}

// `<e .../>` children, NOT `<n .../>`: a recorded pass carries CCs, bends and
// pressure as well as notes, and the note spelling would read every one of
// them back as a NoteOn. `readEventChildren` accepts both, so a hand-written
// case may still use `<n>` for a pass that is only notes.
void writeEvents( QDomElement &elem, const std::vector<SEvent> &events )
{
    smidiactions::writeEventChildren( elem, events );
}

/**
 * The EVENT columns of a lane that overlap `[from, to)`, in timeline order.
 *
 * "Column" is the lane child, whatever it is: a plain `SMidiCut` placement or
 * a take stack of them. The kind test is `contentKind()`, which a stack
 * answers for its takes - so an audio column is never a candidate and a
 * recorded pass over one becomes a clip of its own beside it rather than a
 * refused action.
 */
struct Column {
    SLink   *link  = nullptr;
    offset_t start = 0;
    length_t dur   = 0;
    int      index = 0;
};

std::vector<Column> eventColumns( SObject *lane, offset_t from, offset_t to )
{
    std::vector<Column> out;
    if( !lane ) return out;
    for( int i = 0; i < lane->childCount(); ++i ) {
        SLink *lk = lane->childAt( i );
        if( !lk || lk->getSObject().isPathContainer() ) continue;   // sub-track
        SObject &obj = lk->getSObject();
        if( !obj.hasDuration() ) continue;
        if( obj.contentKind() != SContentKind::Event ) continue;
        const offset_t s = lk->getStartTime();
        const length_t d = obj.getDurationBlocking();
        if( d == 0 || s + (offset_t) d <= from || s >= to ) continue;
        out.push_back( { lk, s, d, i } );
    }
    std::sort( out.begin(), out.end(),
               []( const Column &a, const Column &b )
               { return a.start < b.start; } );
    return out;
}

/** The window of a placement, or null when it is not one. */
SClipWindow *windowOf( SLink *link, int take )
{
    if( !link ) return nullptr;
    SObject &obj = link->getSObject();
    if( SClipWindow *w = obj.windowTakeAt( take ) ) return w;
    return SClipWindow::of( &obj );
}

}  // namespace

// ---------------------------------------------------------------------------
// add-midi-take
// ---------------------------------------------------------------------------

SAddMidiTakeAction::SAddMidiTakeAction( const QList<int> &clipPath,
                                        std::vector<SEvent> events,
                                        qint64 lengthTicks, const QString &name,
                                        int index, bool activate )
    : clipPath_( clipPath ), events_( std::move( events ) ),
      lengthTicks_( lengthTicks ), takeName_( name ), index_( index ),
      activate_( activate )
{
}

SApplyResult SAddMidiTakeAction::apply( SProject *project )
{
    if( !project || clipPath_.isEmpty() ) return { false, nullptr };
    SObject *mixer = splacements::rootNamed( project, pathRoot_ );
    if( !mixer ) return { false, nullptr };

    QList<int> lanePath = clipPath_;
    const int  idx  = lanePath.takeLast();
    SObject   *lane = splacements::laneAt( mixer, lanePath );
    SLink     *link = lane ? lane->childAt( idx ) : nullptr;
    if( !link || link->getSObject().isPathContainer() ) {
        qWarning() << "add-midi-take: no clip at" << qualifiedToString( pathRoot_, clipPath_ );
        return { false, nullptr };
    }

    // A plain placement becomes a single-take COLUMN first (take 0 = what was
    // there), through the model's registered factory - `objects/midi` never
    // names STakeStack. Only an EVENT placement qualifies: wrapping an audio
    // cut here would build a column the insert below is then bound to refuse,
    // and it would have moved the user's clip for nothing.
    if( link->getSObject().windowTakeCount() == 0 ) {
        if( link->getSObject().contentKind() != SContentKind::Event ) {
            qWarning() << "add-midi-take: the clip at"
                       << qualifiedToString( pathRoot_, clipPath_ ) << "is not an event clip";
            return { false, nullptr };
        }
        link = SClipWindow::wrapIntoTakeColumn( project, lane, link );
        if( !link || link->getSObject().windowTakeCount() == 0 ) {
            qWarning() << "add-midi-take: could not wrap"
                       << qualifiedToString( pathRoot_, clipPath_ ) << "into a take column";
            return { false, nullptr };
        }
    }
    SObject &column = link->getSObject();
    const int prevActive = column.activeWindowTakeIndex();

    // Blocking read (P19): the new take must adopt the column's CURRENT
    // duration, never the stale try-lock fallback - it is an edit path.
    const length_t colDur = column.getDurationBlocking();

    qint64 lengthTicks = lengthTicks_;
    if( lengthTicks <= 0 ) {
        // Derive the sequence's musical length from the column's frames, once,
        // through THE tempo map (POSITION_DOMAINS rule 7).
        lengthTicks = project->tempoMap()
                          .framesToTickLen( (int64_t) colDur, project->getSRate() )
                          .ticks().floorToInt();
    }

    SMidiSequence *seq = new SMidiSequence( project );
    seq->setLengthTicks( lengthTicks );
    seq->setOrigin( SMidiSequence::Origin::Recorded );
    if( !events_.empty() ) seq->setEvents( events_ );
    if( !takeName_.isEmpty() ) seq->setSName( takeName_ );

    SClipWindow *win = SClipWindow::wrapContent( project, *seq );
    if( !win ) { delete seq; return { false, nullptr }; }
    if( !takeName_.isEmpty() ) win->asObject().setSName( takeName_ );
    win->setDurationFromTimeline( colDur );   // take-stack invariant 1

    const int at = ( index_ >= 0 && index_ <= column.windowTakeCount() )
                       ? index_ : column.windowTakeCount();
    if( !column.insertWindowTake( *win, at == column.windowTakeCount() ? -1 : at ) ) {
        // Homogeneity (proposal 37 D8b): the column holds audio takes. Reject
        // rather than build a column that plays notes or audio depending on
        // which lane is active.
        delete &win->asObject();
        return { false, nullptr };
    }
    if( activate_ ) column.setActiveWindowTake( at );

    QList<int> columnPath = lanePath;
    columnPath.append( lane->indexOfChild( link ) );
    return { true, new SRemoveMidiTakeAction( columnPath, at,
                                              activate_ ? prevActive : -2 ) };
}

void SAddMidiTakeAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "clip", qualifiedToString( pathRoot_, clipPath_ ) );
    elem.setAttribute( "index", index_ );
    elem.setAttribute( "activate", activate_ ? 1 : 0 );
    elem.setAttribute( "name", takeName_ );
    elem.setAttribute( "lengthTicks", QString::number( lengthTicks_ ) );
    writeEvents( elem, events_ );
}

bool SAddMidiTakeAction::readXml( const QDomElement &elem, int )
{
    clipPath_    = parseInto( pathRoot_, elem.attribute( "clip" ) );
    index_       = elem.attribute( "index", "-1" ).toInt();
    activate_    = elem.attribute( "activate", "1" ).toInt() != 0;
    takeName_    = elem.attribute( "name", "" );
    lengthTicks_ = elem.attribute( "lengthTicks", "0" ).toLongLong();
    events_      = readEvents( elem );
    return true;
}

static const bool s_reg_add_midi_take = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "add-midi-take" ),
        []{ return new SAddMidiTakeAction; } ), true );

// ---------------------------------------------------------------------------
// remove-midi-take (add-midi-take's inverse; created live)
// ---------------------------------------------------------------------------

SRemoveMidiTakeAction::SRemoveMidiTakeAction( const QList<int> &columnPath,
                                              int takeIndex, int thenActivate )
    : columnPath_( columnPath ), takeIndex_( takeIndex ),
      thenActivate_( thenActivate )
{
}

SApplyResult SRemoveMidiTakeAction::apply( SProject *project )
{
    if( !project || columnPath_.isEmpty() ) return { false, nullptr };
    SObject *mixer = splacements::rootNamed( project, pathRoot_ );
    if( !mixer ) return { false, nullptr };

    QList<int> lanePath = columnPath_;
    const int  idx  = lanePath.takeLast();
    SObject   *lane = splacements::laneAt( mixer, lanePath );
    SLink     *link = lane ? lane->childAt( idx ) : nullptr;
    if( !link ) return { false, nullptr };
    SObject &column = link->getSObject();
    if( takeIndex_ < 0 || takeIndex_ >= column.windowTakeCount() )
        return { false, nullptr };

    // The take's whole state, captured BEFORE it is removed, so the inverse
    // restores the notes and not just an empty lane. A lost take is not an undo.
    SClipWindow *take = column.windowTakeAt( takeIndex_ );
    std::vector<SEvent> events;
    qint64  lengthTicks = 0;
    QString takeName;
    if( take ) {
        takeName = take->asObject().getSName();
        if( SMidiCut *cut = dynamic_cast<SMidiCut *>( &take->asObject() ) ) {
            if( SMidiSequence *seq = cut->sequence() ) {
                events      = seq->events();
                lengthTicks = seq->lengthTicks();
            }
        }
    }
    const bool wasActive = ( column.activeWindowTakeIndex() == takeIndex_ );

    column.removeWindowTake( takeIndex_ );
    if( thenActivate_ >= -1 ) column.setActiveWindowTake( thenActivate_ );

    // Take-stack invariant 3: a single remaining take collapses back to a
    // plain placement, so an undo leaves the tree exactly as it was found.
    SLink *result = link;
    if( column.windowTakeCount() == 1 ) {
        if( SLink *plain = SClipWindow::collapseTakeColumn( lane, link ) )
            result = plain;
    }

    QList<int> invPath = lanePath;
    invPath.append( lane->indexOfChild( result ) );
    return { true, new SAddMidiTakeAction( invPath, std::move( events ),
                                           lengthTicks, takeName, takeIndex_,
                                           wasActive ) };
}

void SRemoveMidiTakeAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "clip", qualifiedToString( pathRoot_, columnPath_ ) );
    elem.setAttribute( "take", takeIndex_ );
    elem.setAttribute( "thenActivate", thenActivate_ );
}

bool SRemoveMidiTakeAction::readXml( const QDomElement &, int )
{
    return false;   // created live as add-midi-take's inverse
}

// ---------------------------------------------------------------------------
// place-midi-recording
// ---------------------------------------------------------------------------

SPlaceMidiRecordingAction::SPlaceMidiRecordingAction(
    const QList<int> &trackPath, offset_t timePos, qint64 durationTicks,
    std::vector<SEvent> events, const QString &mode, const QString &quantize,
    const QString &name )
    : trackPath_( trackPath ), timePos_( timePos ),
      durationTicks_( durationTicks ), events_( std::move( events ) ),
      mode_( mode ), quantize_( quantize ), clipName_( name )
{
}

bool SPlaceMidiRecordingAction::isKnownMode( const QString &mode )
{
    return mode == QStringLiteral( "new-take" )
        || mode == QStringLiteral( "overdub" )
        || mode == QStringLiteral( "replace" );
}

SApplyResult SPlaceMidiRecordingAction::apply( SProject *project )
{
    if( !project ) return { false, nullptr };
    SObject *mixer = splacements::rootNamed( project, pathRoot_ );
    SObject *lane  = splacements::laneAt( mixer, trackPath_ );
    if( !lane ) {
        qWarning() << "place-midi-recording: no lane at"
                   << qualifiedToString( pathRoot_, trackPath_ );
        return { false, nullptr };
    }
    const QString mode = isKnownMode( mode_ ) ? mode_
                                              : QStringLiteral( "new-take" );

    const twTempoMap &map  = project->tempoMap();
    const int         rate = project->getSRate();
    qint64 durationTicks = durationTicks_;
    if( durationTicks <= 0 )
        durationTicks = (qint64) map.ppq() * 4 * map.numerator() / map.denominator();
    // The pass window in FRAMES, derived once, here.
    const length_t durationFrames = (length_t)
        map.ticksToFrames( TickLen( Fraction( durationTicks, 1 ) ), rate )
           .floorToInt();

    SCompositeAction composite;
    QList<int>       clipPath;      // what `quantize` will address

    const std::vector<Column> cols =
        eventColumns( lane, timePos_, timePos_ + (offset_t) durationFrames );

    if( cols.empty() ) {
        // Nothing here: a new clip carrying the pass. `insert-midi-clip` gives
        // it `timebase=beats` like every event placement, so the take moves
        // with the bar under a later tempo edit.
        clipPath = trackPath_;
        clipPath.append( lane->childCount() );   // insert-midi-clip appends
        composite.append( new SInsertMidiClipAction( trackPath_, timePos_,
                                                     durationTicks, clipName_,
                                                     events_ ) );
    } else {
        const Column &col = cols.front();
        clipPath = trackPath_;
        clipPath.append( col.index );

        // WHERE THE PASS SITS IN THE CLIP'S CONTENT, exactly. The window owns
        // the map (slip + rate), so the base tick is asked for rather than
        // computed here - two callers converting independently is how a
        // rounding difference becomes an off-by-one note.
        SClipWindow *win = windowOf( col.link, -1 );
        Fraction     baseTicks( 0, 1 );
        Fraction     invRate( 1, 1 );
        if( win ) {
            baseTicks = win->timelineToSourceExact(
                Fraction( (int64_t) timePos_ - (int64_t) col.start, 1 ) );
            const Fraction r = win->stretchOrRate();
            if( r.numerator != 0 ) invRate = Fraction( 1, 1 ) / r;
        }
        const qint64 base = baseTicks.floorToInt();
        // Pass-window ticks -> the content's ticks.
        auto rebase = [&]( qint64 t ) -> qint64
        { return base + ( Fraction( t, 1 ) * invRate ).floorToInt(); };
        const qint64 winEnd =
            base + ( Fraction( durationTicks, 1 ) * invRate ).floorToInt();

        std::vector<SEvent> pass = events_;
        for( SEvent &e : pass ) {
            const qint64 endT = e.endTick();
            e.t   = rebase( e.t );
            if( e.kind == twEventKind::NoteOn )
                e.dur = std::max<qint64>( 0, rebase( endT ) - e.t );
        }

        if( mode == QStringLiteral( "new-take" ) ) {
            composite.append( new SAddMidiTakeAction(
                clipPath, pass,
                win ? (qint64) 0 : durationTicks,   // 0 = derive from the column
                clipName_, -1, true ) );
        } else {
            // Overdub / replace both END as one absolute `set-events` on the
            // column's ACTIVE take, which is the universal inverse every event
            // edit already uses (37 3.4) - so undo is the previous table,
            // verbatim, whichever mode was used.
            std::vector<SEvent> merged;
            smidiactions::ClipRef ref =
                smidiactions::resolveClip( project, pathRoot_, clipPath, -1 );
            if( !ref.valid() ) {
                qWarning() << "place-midi-recording: the column at"
                           << pathToString( clipPath )
                           << "holds no event take to merge into";
                return { false, nullptr };
            }
            merged = ref.seq->events();
            if( mode == QStringLiteral( "replace" ) ) {
                // Only NOTES inside the pass window go; CCs and everything
                // outside survive, exactly as `set-notes` promises.
                merged.erase(
                    std::remove_if( merged.begin(), merged.end(),
                                    [&]( const SEvent &e ) {
                                        return e.kind == twEventKind::NoteOn
                                            && e.t >= base && e.t < winEnd;
                                    } ),
                    merged.end() );
            }
            merged.insert( merged.end(), pass.begin(), pass.end() );
            std::stable_sort( merged.begin(), merged.end() );
            composite.append( new SSetEventsAction( clipPath, merged, -1 ) );
        }
    }

    // INPUT QUANTISE, inside this verb's own composite: one undo entry covers
    // the placement AND the quantise, rather than leaving the user two steps
    // to undo for one recording.
    if( !quantize_.isEmpty() && quantize_ != QStringLiteral( "off" )
        && SQuantizeNotesAction::gridTicks( quantize_, project->tempoMap().ppq() ) > 0 ) {
        composite.append( new SQuantizeNotesAction( clipPath, quantize_, 1.0,
                                                    0.0, -1, false ) );
    }

    return composite.apply( project );
}

void SPlaceMidiRecordingAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "trackPath", qualifiedToString( pathRoot_, trackPath_ ) );
    elem.setAttribute( "timePos", QString::fromStdString(
                           Fraction( (int64_t) timePos_, 1 ).toString() ) );
    elem.setAttribute( "durationTicks", QString::number( durationTicks_ ) );
    elem.setAttribute( "mode", mode_ );
    elem.setAttribute( "quantize", quantize_ );
    elem.setAttribute( "name", clipName_ );
    writeEvents( elem, events_ );
}

bool SPlaceMidiRecordingAction::readXml( const QDomElement &elem, int )
{
    trackPath_     = parseInto( pathRoot_, elem.attribute( "trackPath" ) );
    timePos_       = (offset_t) parseFractionOrDouble(
        elem.attribute( "timePos", "0" ).toStdString() ).toDouble();
    durationTicks_ = elem.attribute( "durationTicks", "0" ).toLongLong();
    mode_          = elem.attribute( "mode", "new-take" );
    quantize_      = elem.attribute( "quantize", "off" );
    clipName_      = elem.attribute( "name", "" );
    events_        = readEvents( elem );
    return true;
}

static const bool s_reg_place_midi_recording = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "place-midi-recording" ),
        []{ return new SPlaceMidiRecordingAction; } ), true );
