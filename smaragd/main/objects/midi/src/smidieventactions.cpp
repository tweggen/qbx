#include "app/objects/midi/smidieventactions.h"

#include <QDebug>
#include <QDomElement>
#include <algorithm>
#include <cmath>

#include "app/actions/sactionregistry.h"
#include "app/actions/scompositeaction.h"
#include "app/model/seditgroups.h"
#include "app/model/sobjectpath.h"
#include "app/model/sproject.h"
#include "app/objects/midi/smidiactionsupport.h"
#include "app/objects/midi/smidicut.h"

using namespace strackpath;
using smidiactions::ClipRef;

namespace {

/**
 * Commit an absolute new table and hand back the inverse. EVERY content verb
 * ends here, which is what makes "the inverse is the previous state" true by
 * construction rather than by discipline.
 */
SApplyResult commitTable( const ClipRef &ref, const QList<int> &clipPath,
                          int take, std::vector<SEvent> next )
{
    std::vector<SEvent> before = ref.seq->events();
    ref.seq->setEvents( std::move( next ) );
    return { true, new SSetEventsAction( clipPath, std::move( before ), take ) };
}

/**
 * Edit-group fan-out, the shape every clip verb uses (phase 4): a grouped
 * anchor becomes one SCompositeAction over the members' corresponding clips,
 * with broadcast off on the children.
 */
template <typename MakeFn>
bool expandBroadcast( SProject *project, const QList<int> &clipPath,
                      MakeFn make, SApplyResult &out )
{
    QList<QList<int>> targets = seditgroups::expandClipPaths( project, clipPath );
    if( targets.size() <= 1 ) return false;
    SCompositeAction composite;
    for( const QList<int> &p : targets ) composite.append( make( p ) );
    out = composite.apply( project );
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// set-events
// ---------------------------------------------------------------------------

SSetEventsAction::SSetEventsAction( const QList<int> &clipPath,
                                    std::vector<SEvent> events, int take )
    : clipPath_( clipPath ), events_( std::move( events ) ), take_( take )
{
}

SApplyResult SSetEventsAction::apply( SProject *project )
{
    ClipRef ref = smidiactions::resolveClip( project, pathRoot_, clipPath_, take_ );
    if( !ref.valid() ) {
        qWarning() << "set-events: no MIDI clip at" << pathToString( clipPath_ );
        return { false, nullptr };
    }
    return commitTable( ref, clipPath_, take_, events_ );
}

void SSetEventsAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "clip", pathToString( clipPath_ ) );
    elem.setAttribute( "take", take_ );
    smidiactions::writeEventChildren( elem, events_ );
}

bool SSetEventsAction::readXml( const QDomElement &elem, int )
{
    clipPath_ = parseInto( pathRoot_, elem.attribute( "clip" ) );
    take_ = elem.attribute( "take", "-1" ).toInt();
    events_ = smidiactions::readEventChildren( elem );
    return true;
}

static const bool s_reg_set_events = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "set-events" ), []{ return new SSetEventsAction; } ), true );

// ---------------------------------------------------------------------------
// add-note
// ---------------------------------------------------------------------------

SAddNoteAction::SAddNoteAction( const QList<int> &clipPath, qint64 tick,
                                qint64 dur, int key, double velocity,
                                int channel, double releaseVelocity, int take,
                                bool broadcast )
    : clipPath_( clipPath ), tick_( tick ), dur_( dur ), key_( key ),
      velocity_( velocity ), channel_( channel ),
      releaseVelocity_( releaseVelocity ), take_( take ), broadcast_( broadcast )
{
}

QStringList SAddNoteAction::knownAttributes() const
{
    return { "clip", "tick", "dur", "key", "velocity", "channel",
             "releaseVelocity", "take", "broadcast" };
}

SApplyResult SAddNoteAction::apply( SProject *project )
{
    if( broadcast_ ) {
        SApplyResult out{ false, nullptr };
        if( expandBroadcast( project, clipPath_,
                [&]( const QList<int> &p ) -> SAction * {
                    return new SAddNoteAction( p, tick_, dur_, key_, velocity_,
                                               channel_, releaseVelocity_,
                                               take_, false );
                }, out ) )
            return out;
    }
    ClipRef ref = smidiactions::resolveClip( project, pathRoot_, clipPath_, take_ );
    if( !ref.valid() ) {
        qWarning() << "add-note: no MIDI clip at" << pathToString( clipPath_ );
        return { false, nullptr };
    }
    std::vector<SEvent> next = ref.seq->events();
    SEvent e;
    e.kind = twEventKind::NoteOn;
    e.t = tick_;
    e.dur = dur_;
    e.key = key_;
    e.value = velocity_;
    e.value2 = releaseVelocity_;
    e.channel = channel_;
    next.push_back( e );
    return commitTable( ref, clipPath_, take_, std::move( next ) );
}

void SAddNoteAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "clip", pathToString( clipPath_ ) );
    elem.setAttribute( "tick", QString::number( tick_ ) );
    elem.setAttribute( "dur", QString::number( dur_ ) );
    elem.setAttribute( "key", key_ );
    elem.setAttribute( "velocity", QString::number( velocity_ ) );
    elem.setAttribute( "channel", channel_ );
    elem.setAttribute( "releaseVelocity", QString::number( releaseVelocity_ ) );
    elem.setAttribute( "take", take_ );
    elem.setAttribute( "broadcast", broadcast_ ? 1 : 0 );
}

bool SAddNoteAction::readXml( const QDomElement &elem, int )
{
    clipPath_ = parseInto( pathRoot_, elem.attribute( "clip" ) );
    tick_ = elem.attribute( "tick", "0" ).toLongLong();
    dur_ = elem.attribute( "dur", "480" ).toLongLong();
    key_ = elem.attribute( "key", "60" ).toInt();
    velocity_ = elem.attribute( "velocity", "100" ).toDouble();
    channel_ = elem.attribute( "channel", "0" ).toInt();
    releaseVelocity_ = elem.attribute( "releaseVelocity", "64" ).toDouble();
    take_ = elem.attribute( "take", "-1" ).toInt();
    broadcast_ = elem.attribute( "broadcast", "1" ).toInt() != 0;
    return true;
}

static const bool s_reg_add_note = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "add-note" ), []{ return new SAddNoteAction; } ), true );

// ---------------------------------------------------------------------------
// remove-note
// ---------------------------------------------------------------------------

SRemoveNoteAction::SRemoveNoteAction( const QList<int> &clipPath, qint64 tick,
                                      int key, int channel, int take,
                                      bool broadcast )
    : clipPath_( clipPath ), tick_( tick ), key_( key ), channel_( channel ),
      take_( take ), broadcast_( broadcast )
{
}

QStringList SRemoveNoteAction::knownAttributes() const
{
    return { "clip", "tick", "key", "channel", "take", "broadcast" };
}

SApplyResult SRemoveNoteAction::apply( SProject *project )
{
    if( broadcast_ ) {
        SApplyResult out{ false, nullptr };
        if( expandBroadcast( project, clipPath_,
                [&]( const QList<int> &p ) -> SAction * {
                    return new SRemoveNoteAction( p, tick_, key_, channel_,
                                                  take_, false );
                }, out ) )
            return out;
    }
    ClipRef ref = smidiactions::resolveClip( project, pathRoot_, clipPath_, take_ );
    if( !ref.valid() ) {
        qWarning() << "remove-note: no MIDI clip at" << pathToString( clipPath_ );
        return { false, nullptr };
    }
    std::vector<SEvent> all = ref.seq->events();
    std::vector<SEvent> next;
    next.reserve( all.size() );
    int removed = 0;
    for( const SEvent &e : all ) {
        if( smidiactions::matchesNote( e, tick_, key_, channel_ ) ) { ++removed; continue; }
        next.push_back( e );
    }
    if( removed == 0 ) {
        // Nothing addressed: a rejected apply, not a silent no-op, so a typo
        // in a script fails the case instead of testing nothing.
        qWarning() << "remove-note: no note at tick" << tick_ << "key" << key_
                   << "channel" << channel_;
        return { false, nullptr };
    }
    return commitTable( ref, clipPath_, take_, std::move( next ) );
}

void SRemoveNoteAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "clip", pathToString( clipPath_ ) );
    elem.setAttribute( "tick", QString::number( tick_ ) );
    elem.setAttribute( "key", key_ );
    elem.setAttribute( "channel", channel_ );
    elem.setAttribute( "take", take_ );
    elem.setAttribute( "broadcast", broadcast_ ? 1 : 0 );
}

bool SRemoveNoteAction::readXml( const QDomElement &elem, int )
{
    clipPath_ = parseInto( pathRoot_, elem.attribute( "clip" ) );
    tick_ = elem.attribute( "tick", "0" ).toLongLong();
    key_ = elem.attribute( "key", "-1" ).toInt();
    channel_ = elem.attribute( "channel", "-1" ).toInt();
    take_ = elem.attribute( "take", "-1" ).toInt();
    broadcast_ = elem.attribute( "broadcast", "1" ).toInt() != 0;
    return true;
}

static const bool s_reg_remove_note = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "remove-note" ), []{ return new SRemoveNoteAction; } ), true );

// ---------------------------------------------------------------------------
// set-notes
// ---------------------------------------------------------------------------

SSetNotesAction::SSetNotesAction( const QList<int> &clipPath,
                                  std::vector<SEvent> notes, int take,
                                  bool broadcast )
    : clipPath_( clipPath ), notes_( std::move( notes ) ), take_( take ),
      broadcast_( broadcast )
{
}

QStringList SSetNotesAction::knownAttributes() const
{
    return { "clip", "take", "broadcast" };
}

SApplyResult SSetNotesAction::apply( SProject *project )
{
    if( broadcast_ ) {
        SApplyResult out{ false, nullptr };
        if( expandBroadcast( project, clipPath_,
                [&]( const QList<int> &p ) -> SAction * {
                    return new SSetNotesAction( p, notes_, take_, false );
                }, out ) )
            return out;
    }
    ClipRef ref = smidiactions::resolveClip( project, pathRoot_, clipPath_, take_ );
    if( !ref.valid() ) {
        qWarning() << "set-notes: no MIDI clip at" << pathToString( clipPath_ );
        return { false, nullptr };
    }
    // Absolute new NOTE state. Everything that is not a note survives, so
    // drawing notes never destroys the controller data underneath them.
    std::vector<SEvent> next;
    for( const SEvent &e : ref.seq->events() )
        if( e.kind != twEventKind::NoteOn ) next.push_back( e );
    for( const SEvent &e : notes_ ) next.push_back( e );
    return commitTable( ref, clipPath_, take_, std::move( next ) );
}

void SSetNotesAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "clip", pathToString( clipPath_ ) );
    elem.setAttribute( "take", take_ );
    elem.setAttribute( "broadcast", broadcast_ ? 1 : 0 );
    for( const SEvent &e : notes_ ) smidiactions::writeNoteChild( elem, e );
}

bool SSetNotesAction::readXml( const QDomElement &elem, int )
{
    clipPath_ = parseInto( pathRoot_, elem.attribute( "clip" ) );
    take_ = elem.attribute( "take", "-1" ).toInt();
    broadcast_ = elem.attribute( "broadcast", "1" ).toInt() != 0;
    notes_ = smidiactions::readEventChildren( elem );
    return true;
}

QString SSetNotesAction::mergeKey() const
{
    // Root-qualified for the reason set-plugin-param's key is: clip {0,0}
    // exists in every root (proposal 09 D21).
    return QStringLiteral( "set-notes:" ) + qualifiedToString( pathRoot_, clipPath_ )
         + ":" + QString::number( take_ );
}

bool SSetNotesAction::mergeWith( const SAction *later )
{
    const SSetNotesAction *o = dynamic_cast<const SSetNotesAction *>( later );
    if( !o ) return false;
    notes_ = o->notes_;   // the LATER absolute state wins
    return true;
}

static const bool s_reg_set_notes = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "set-notes" ), []{ return new SSetNotesAction; } ), true );

// ---------------------------------------------------------------------------
// add-event
// ---------------------------------------------------------------------------

SAddEventAction::SAddEventAction( const QList<int> &clipPath,
                                  const SEvent &event, int take )
    : clipPath_( clipPath ), event_( event ), take_( take )
{
}

QStringList SAddEventAction::knownAttributes() const
{
    return { "clip", "kind", "tick", "channel", "key", "p", "v", "v2",
             "text", "blob", "take" };
}

SApplyResult SAddEventAction::apply( SProject *project )
{
    ClipRef ref = smidiactions::resolveClip( project, pathRoot_, clipPath_, take_ );
    if( !ref.valid() ) {
        qWarning() << "add-event: no MIDI clip at" << pathToString( clipPath_ );
        return { false, nullptr };
    }
    std::vector<SEvent> next = ref.seq->events();
    next.push_back( event_ );
    return commitTable( ref, clipPath_, take_, std::move( next ) );
}

void SAddEventAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "clip", pathToString( clipPath_ ) );
    elem.setAttribute( "kind", smidievents::kindToString( event_ ) );
    elem.setAttribute( "tick", QString::number( event_.t ) );
    elem.setAttribute( "channel", event_.channel );
    if( event_.key >= 0 ) elem.setAttribute( "key", event_.key );
    elem.setAttribute( "p", QString::number( event_.paramId ) );
    elem.setAttribute( "v", QString::number( event_.value, 'g', 17 ) );
    if( event_.value2 != 0.0 )
        elem.setAttribute( "v2", QString::number( event_.value2, 'g', 17 ) );
    if( !event_.text.isEmpty() ) elem.setAttribute( "text", event_.text );
    if( !event_.blob.isEmpty() )
        elem.setAttribute( "blob", QString::fromLatin1( event_.blob.toHex() ) );
    elem.setAttribute( "take", take_ );
}

bool SAddEventAction::readXml( const QDomElement &elem, int )
{
    clipPath_ = parseInto( pathRoot_, elem.attribute( "clip" ) );
    event_ = SEvent();
    quint32 meta = 0;
    const QString kind = elem.attribute( "kind", "cc" );
    if( !smidievents::kindFromString( kind, event_.kind, meta ) ) {
        event_.kind = twEventKind::Unknown;
        event_.rawKind = kind;
    } else if( event_.kind == twEventKind::Unknown ) {
        event_.paramId = meta;
    }
    event_.t = elem.attribute( "tick", "0" ).toLongLong();
    event_.channel = elem.attribute( "channel", "-1" ).toInt();
    event_.key = elem.attribute( "key", "-1" ).toInt();
    if( elem.hasAttribute( "p" ) ) event_.paramId = elem.attribute( "p" ).toUInt();
    event_.value = elem.attribute( "v", "0" ).toDouble();
    event_.value2 = elem.attribute( "v2", "0" ).toDouble();
    event_.text = elem.attribute( "text", "" );
    if( elem.hasAttribute( "blob" ) )
        event_.blob = QByteArray::fromHex( elem.attribute( "blob" ).toLatin1() );
    take_ = elem.attribute( "take", "-1" ).toInt();
    return true;
}

static const bool s_reg_add_event = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "add-event" ), []{ return new SAddEventAction; } ), true );

// ---------------------------------------------------------------------------
// remove-event
// ---------------------------------------------------------------------------

SRemoveEventAction::SRemoveEventAction( const QList<int> &clipPath,
                                        const QString &kind, qint64 tick,
                                        int channel, int paramId, int take )
    : clipPath_( clipPath ), kind_( kind ), tick_( tick ), channel_( channel ),
      paramId_( paramId ), take_( take )
{
}

QStringList SRemoveEventAction::knownAttributes() const
{
    return { "clip", "kind", "tick", "channel", "p", "take" };
}

SApplyResult SRemoveEventAction::apply( SProject *project )
{
    ClipRef ref = smidiactions::resolveClip( project, pathRoot_, clipPath_, take_ );
    if( !ref.valid() ) {
        qWarning() << "remove-event: no MIDI clip at" << pathToString( clipPath_ );
        return { false, nullptr };
    }
    twEventKind want = twEventKind::Unknown;
    quint32 wantMeta = 0;
    const bool knownKind = smidievents::kindFromString( kind_, want, wantMeta );

    std::vector<SEvent> next;
    int removed = 0;
    for( const SEvent &e : ref.seq->events() ) {
        const bool kindOk = kind_.isEmpty()
                          || ( knownKind ? e.kind == want : e.rawKind == kind_ );
        const bool match = kindOk && e.t == tick_
                        && ( channel_ < 0 || e.channel == channel_ )
                        && ( paramId_ < 0 || (int) e.paramId == paramId_ );
        if( match ) { ++removed; continue; }
        next.push_back( e );
    }
    if( removed == 0 ) {
        qWarning() << "remove-event: nothing matched kind" << kind_ << "tick"
                   << tick_;
        return { false, nullptr };
    }
    return commitTable( ref, clipPath_, take_, std::move( next ) );
}

void SRemoveEventAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "clip", pathToString( clipPath_ ) );
    elem.setAttribute( "kind", kind_ );
    elem.setAttribute( "tick", QString::number( tick_ ) );
    elem.setAttribute( "channel", channel_ );
    elem.setAttribute( "p", paramId_ );
    elem.setAttribute( "take", take_ );
}

bool SRemoveEventAction::readXml( const QDomElement &elem, int )
{
    clipPath_ = parseInto( pathRoot_, elem.attribute( "clip" ) );
    kind_ = elem.attribute( "kind", "" );
    tick_ = elem.attribute( "tick", "0" ).toLongLong();
    channel_ = elem.attribute( "channel", "-1" ).toInt();
    paramId_ = elem.attribute( "p", "-1" ).toInt();
    take_ = elem.attribute( "take", "-1" ).toInt();
    return true;
}

static const bool s_reg_remove_event = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "remove-event" ), []{ return new SRemoveEventAction; } ), true );

// ---------------------------------------------------------------------------
// quantize-notes
// ---------------------------------------------------------------------------

SQuantizeNotesAction::SQuantizeNotesAction( const QList<int> &clipPath,
                                            const QString &grid, double strength,
                                            double swing, int take,
                                            bool broadcast )
    : clipPath_( clipPath ), grid_( grid ), strength_( strength ),
      swing_( swing ), take_( take ), broadcast_( broadcast )
{
}

QStringList SQuantizeNotesAction::knownAttributes() const
{
    return { "clip", "grid", "strength", "swing", "take", "broadcast" };
}

qint64 SQuantizeNotesAction::gridTicks( const QString &grid, int ppq )
{
    QString g = grid.trimmed().toLower();
    bool triplet = false, dotted = false;
    if( g.endsWith( 't' ) ) { triplet = true; g.chop( 1 ); }
    else if( g.endsWith( 'd' ) ) { dotted = true; g.chop( 1 ); }
    const QStringList parts = g.split( '/' );
    if( parts.size() != 2 ) return 0;
    bool okA = false, okB = false;
    const qint64 num = parts[0].toLongLong( &okA );
    const qint64 den = parts[1].toLongLong( &okB );
    if( !okA || !okB || num <= 0 || den <= 0 ) return 0;
    // A quarter note is ppq ticks, so 1/den of a whole note is 4*ppq/den.
    qint64 t = ( 4LL * ppq * num ) / den;
    if( triplet ) t = ( t * 2 ) / 3;
    if( dotted )  t = ( t * 3 ) / 2;
    return t;
}

SApplyResult SQuantizeNotesAction::apply( SProject *project )
{
    if( broadcast_ ) {
        SApplyResult out{ false, nullptr };
        if( expandBroadcast( project, clipPath_,
                [&]( const QList<int> &p ) -> SAction * {
                    return new SQuantizeNotesAction( p, grid_, strength_, swing_,
                                                     take_, false );
                }, out ) )
            return out;
    }
    ClipRef ref = smidiactions::resolveClip( project, pathRoot_, clipPath_, take_ );
    if( !ref.valid() ) {
        qWarning() << "quantize-notes: no MIDI clip at" << pathToString( clipPath_ );
        return { false, nullptr };
    }
    const qint64 grid = gridTicks( grid_, ref.seq->ppq() );
    if( grid <= 0 ) {
        qWarning() << "quantize-notes: cannot parse grid" << grid_;
        return { false, nullptr };
    }
    const double strength = std::max( 0.0, std::min( 1.0, strength_ ) );
    const double swing = std::max( -0.9, std::min( 0.9, swing_ ) );

    std::vector<SEvent> next = ref.seq->events();
    for( SEvent &e : next ) {
        if( e.kind != twEventKind::NoteOn ) continue;
        qint64 slot = ( e.t + grid / 2 ) / grid;
        // Swing displaces every ODD slot; an even one is always on the grid.
        const qint64 target = slot * grid
                            + ( ( slot & 1 ) ? (qint64) llround( swing * grid ) : 0 );
        e.t = e.t + (qint64) llround( strength * (double) ( target - e.t ) );
        if( e.t < 0 ) e.t = 0;
    }
    return commitTable( ref, clipPath_, take_, std::move( next ) );
}

void SQuantizeNotesAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "clip", pathToString( clipPath_ ) );
    elem.setAttribute( "grid", grid_ );
    elem.setAttribute( "strength", QString::number( strength_ ) );
    elem.setAttribute( "swing", QString::number( swing_ ) );
    elem.setAttribute( "take", take_ );
    elem.setAttribute( "broadcast", broadcast_ ? 1 : 0 );
}

bool SQuantizeNotesAction::readXml( const QDomElement &elem, int )
{
    clipPath_ = parseInto( pathRoot_, elem.attribute( "clip" ) );
    grid_ = elem.attribute( "grid", "1/16" );
    strength_ = elem.attribute( "strength", "1.0" ).toDouble();
    swing_ = elem.attribute( "swing", "0" ).toDouble();
    take_ = elem.attribute( "take", "-1" ).toInt();
    broadcast_ = elem.attribute( "broadcast", "1" ).toInt() != 0;
    return true;
}

static const bool s_reg_quantize_notes = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "quantize-notes" ),
        []{ return new SQuantizeNotesAction; } ), true );
