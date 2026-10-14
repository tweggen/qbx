#include "app/objects/midi/seventpassverbs.h"

#include <QDebug>
#include <QDomDocument>
#include <QDomElement>
#include <QStringList>

#include "app/actions/sactionregistry.h"
#include "app/model/sobjectpath.h"
#include "app/model/sproject.h"
#include "app/objects/midi/smidiclipactions.h"
#include "app/objects/midi/smidirecordactions.h"
#include "tw/events/twtempomap.h"

using namespace strackpath;

namespace {

/**
 * An action of `name`, built through the REGISTRY and configured from its own
 * XML form -- `makeAddTrack()`'s pattern in `smidifileactions.cpp`, which this
 * module already uses for `add-track` and for the same layering reason.
 *
 * `attrs` is written onto the element exactly as the verb's own `writeXml`
 * would, so `readXml` reads it back verbatim. Returning null makes the plan
 * fail, which is what a missing verb has to do: the composite is never given a
 * null child.
 */
SAction *makeByName( const QString &name, const QString &pathRoot,
                     const QList<QPair<QString, QString>> &attrs )
{
    SAction *a = SActionRegistry::instance().create( name );
    if( !a ) {
        qWarning() << "place-midi-recording: no verb named" << name
                   << "is registered";
        return nullptr;
    }
    QDomDocument doc;
    QDomElement  el = doc.createElement( name );
    for( const auto &kv : attrs ) el.setAttribute( kv.first, kv.second );
    doc.appendChild( el );
    a->setPathRoot( pathRoot );
    a->readXml( el, a->formatVersion() );
    return a;
}

}  // namespace

SEventPassVerbs::SEventPassVerbs( SProject *project, const QString &pathRoot,
                                  qint64 passTicks, std::vector<SEvent> events,
                                  const QString &clipName )
    : project_( project ), pathRoot_( pathRoot ), passTicks_( passTicks ),
      events_( std::move( events ) ), clipName_( clipName )
{
}

qint64 SEventPassVerbs::ticksOf_( int64_t frames ) const
{
    if( !project_ || frames == 0 ) return 0;
    const bool    neg = frames < 0;
    const int64_t mag = neg ? -frames : frames;
    const qint64  t   = project_->tempoMap()
                           .framesToTickLen( mag, project_->getSRate() )
                           .ticks().floorToInt();
    return neg ? -t : t;
}

SAction *SEventPassVerbs::placePass( const QList<int> &lanePath,
                                     const srecplan::Span &span,
                                     offset_t at, length_t len ) const
{
    // THE WHOLE PASS IS THE EXACT CASE, and it is the only one modes A and
    // `replace` ever ask for: no conversion happens at all, so the clip's
    // musical length is the pass's own `durationTicks` rather than a
    // frames -> ticks round trip of it.
    const bool   whole    = ( at == span.a && len == span.length() );
    const qint64 offTicks = whole ? 0 : ticksOf_( (int64_t) at - (int64_t) span.a );
    const qint64 lenTicks = whole ? passTicks_ : ticksOf_( (int64_t) len );

    // A SUB-SPAN DROPS THE NOTES THAT STARTED BEFORE IT, and nothing is lost
    // by it. `takes-split` is the only planner that places sub-spans, and it
    // walks the pass left to right: every sub-span but the first is
    // immediately preceded by a touched child that received the WHOLE pass as
    // a take, so a note straddling the boundary has its note-on there and runs
    // on through it (the window synthesises the note-off at its end,
    // events/CONTRACT inv. 8). The FIRST sub-span starts at `span.a`, which is
    // pass tick 0, where nothing can straddle. Clamping such a note into the
    // sub-span instead would re-attack it, which is audibly worse than the
    // note simply continuing from where it was played.
    std::vector<SEvent> ev;
    ev.reserve( events_.size() );
    for( SEvent e : events_ ) {
        if( offTicks != 0 ) {
            if( e.t < offTicks ) continue;
            e.t -= offTicks;
        }
        ev.push_back( e );
    }
    // `insert-midi-clip` is born `timebase=beats` like every event placement
    // (D2), so the pass moves with the bar under a later tempo edit.
    return new SInsertMidiClipAction( lanePath, at, lenTicks, clipName_,
                                      std::move( ev ) );
}

SAction *SEventPassVerbs::addTakeFromPass( const QList<int> &clipPath,
                                           const srecplan::Span &span,
                                           offset_t takeStart ) const
{
    // The take's window starts where the PIECE starts, and the pass's tick 0
    // is at `span.a`, so every event moves by the difference. `takeStart` may
    // sit up to two frames BEFORE `span.a` (`pieceCanSplitAt`), which makes
    // the offset negative and the events move LATER -- leading silence, the
    // same thing a negative `srcOffset` means on the audio side.
    const qint64 offTicks = ticksOf_( (int64_t) takeStart - (int64_t) span.a );

    std::vector<SEvent> ev;
    ev.reserve( events_.size() );
    for( SEvent e : events_ ) {
        const qint64 t = e.t - offTicks;
        if( t < 0 ) continue;      // before this take's window; see placePass
        e.t = t;
        ev.push_back( e );
    }
    // NO EXTENT, deliberately: `add-take` sets none here either. The piece this
    // take joins IS the span the pass covers -- `takes-split` cut it to exactly
    // that -- so "the whole column" is the right extent, and `lengthTicks = 0`
    // derives the sequence's length from it through the tempo map.
    return new SAddMidiTakeAction( clipPath, std::move( ev ), 0, clipName_,
                                   -1, true );
}

SAction *SEventPassVerbs::splitClip( const QList<int> &clipPath,
                                     offset_t at ) const
{
    // broadcast=0 (plan 53 §5.1), for the reason `SAudioPassVerbs` gives: a
    // recording pass decides the edit for the lanes the RECORDER armed, one by
    // one against each lane's own snapshot.
    return makeByName(
        QStringLiteral( "split-clip" ), pathRoot_,
        { { QStringLiteral( "clip" ),
            qualifiedToString( pathRoot_, clipPath ) },
          { QStringLiteral( "splitTime" ),
            QString::number( (qlonglong) at ) },
          { QStringLiteral( "broadcast" ), QStringLiteral( "0" ) } } );
}

SAction *SEventPassVerbs::removeChild( const QList<int> &lanePath, int index,
                                       offset_t at ) const
{
    (void) lanePath; (void) index; (void) at;
    // UNREACHABLE BY CONSTRUCTION -- see the header. `Replace` is the only
    // planner that removes, and `replace` keeps its own MIDI meaning (plan 53
    // §3.3) rather than going through a planner, so this is never called.
    // Refusing loudly is the only safe answer: a wrong removal here would
    // delete the user's notes, and the plan treats a null verb as a failure
    // that rolls the whole pass back rather than as nothing to do.
    qWarning( "place-midi-recording: no event removal verb -- 'replace' keeps "
              "its own MIDI meaning and does not go through the planner "
              "(plan 53 §3.3)" );
    return nullptr;
}

SAction *SEventPassVerbs::nameTake( const QList<int> &clipPath,
                                    const QString &name ) const
{
    // Through the registry and the verb's own XML form, for the same reason
    // `split-clip` and `merge-columns` are: `set-clip-name` lives in
    // `objects/cut`, which this module may not name (plan 53 P3, QBX-181).
    //
    // take=-1 is "the active take at apply time"; broadcast=0 because a
    // recording plans one lane at a time (plan 53 §5.1).
    return makeByName( QStringLiteral( "set-clip-name" ), pathRoot_,
                       { { QStringLiteral( "clip" ),
                           qualifiedToString( pathRoot_, clipPath ) },
                         { QStringLiteral( "name" ), name },
                         { QStringLiteral( "take" ), QStringLiteral( "-1" ) },
                         { QStringLiteral( "broadcast" ),
                           QStringLiteral( "0" ) } } );
}

SAction *SEventPassVerbs::mergeColumns(
    const QList<QList<int>> &clipPaths, offset_t extentStart,
    length_t extentLen ) const
{
    QStringList parts;
    for( const QList<int> &p : clipPaths )
        parts << qualifiedToString( pathRoot_, p );
    QList<QPair<QString, QString>> attrs{
        { QStringLiteral( "clips" ), parts.join( QLatin1Char( ';' ) ) } };
    // THE GROUP EXTENT, and only when there is one (plan 53 D4 (a), QBX-182).
    // Written exactly as `merge-columns`' own `writeXml` writes it, because
    // this goes in through that verb's `readXml` -- `objects/midi` may not name
    // `SMergeColumnsAction`, so the XML form IS the call.
    if( extentLen >= 0 ) {
        attrs.append( { QStringLiteral( "start" ),
                        QString::fromStdString(
                            Fraction( extentStart, 1 ).toString() ) } );
        attrs.append( { QStringLiteral( "length" ),
                        QString::fromStdString(
                            Fraction( extentLen, 1 ).toString() ) } );
    }
    return makeByName( QStringLiteral( "merge-columns" ), pathRoot_, attrs );
}
