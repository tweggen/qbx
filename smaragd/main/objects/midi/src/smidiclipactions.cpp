#include "app/objects/midi/smidiclipactions.h"

#include <QDebug>
#include <QDomElement>
#include <QObject>
#include <cmath>

#include "app/actions/sactionregistry.h"
#include "app/actions/scompositeaction.h"
#include "app/model/seditgroups.h"
#include "app/model/slink.h"
#include "app/model/sobjectpath.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/objects/midi/smidiactionsupport.h"
#include "app/objects/midi/smidicut.h"
#include "tw/events/twtempomap.h"

using namespace strackpath;
using smidiactions::ClipRef;

// ---------------------------------------------------------------------------
// insert-midi-clip
// ---------------------------------------------------------------------------

SInsertMidiClipAction::SInsertMidiClipAction( const QList<int> &trackPath,
                                              offset_t timePos,
                                              qint64 durationTicks,
                                              const QString &name,
                                              std::vector<SEvent> events )
    : trackPath_( trackPath ), timePos_( timePos ),
      durationTicks_( durationTicks ), clipName_( name ),
      events_( std::move( events ) )
{
}

SApplyResult SInsertMidiClipAction::apply( SProject *project )
{
    if( !project ) return { false, nullptr };
    SObject *mixer = splacements::rootContainer( project );
    SObject *lane = splacements::laneAt( mixer, trackPath_ );
    if( !lane ) {
        qWarning() << "insert-midi-clip: no lane at" << pathToString( trackPath_ );
        return { false, nullptr };
    }

    qint64 lengthTicks = durationTicks_;
    if( lengthTicks <= 0 ) {
        // 0 = ONE BAR of the project's time signature, read from the tempo map
        // (which is the only thing that knows what a bar is).
        const twTempoMap &map = project->tempoMap();
        lengthTicks = (qint64) map.ppq() * 4 * map.numerator() / map.denominator();
    }

    SMidiSequence *seq = new SMidiSequence( project );
    seq->setLengthTicks( lengthTicks );
    if( !events_.empty() ) seq->setEvents( events_ );
    if( !clipName_.isEmpty() ) seq->setSName( clipName_ );

    // The window type follows the CONTENT's kind (the wrap factory), exactly as
    // place-clip does for audio - this verb names no window class.
    SClipWindow *win = SClipWindow::wrapContent( project, *seq );
    if( !win ) {
        delete seq;
        return { false, nullptr };
    }
    if( !clipName_.isEmpty() ) win->asObject().setSName( clipName_ );

    SLink *link = new SLink( win->asObject(), nullptr );
    // Beats by default for event content (SLink::defaultTimebaseFor); the
    // frame position is converted to ticks once, here, and the ticks are the
    // authority from now on.
    link->setStartTime( timePos_ );
    link->setParent( lane );

    QList<int> clipPath = trackPath_;
    clipPath.append( lane->indexOfChild( link ) );
    return { true, new SRemoveMidiClipAction( clipPath, trackPath_, timePos_,
                                              lengthTicks, clipName_,
                                              seq->events() ) };
}

void SInsertMidiClipAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "trackPath", pathToString( trackPath_ ) );
    elem.setAttribute( "timePos", QString::fromStdString(
                           Fraction( (int64_t) timePos_, 1 ).toString() ) );
    elem.setAttribute( "duration", QString::number( durationTicks_ ) );
    elem.setAttribute( "name", clipName_ );
}

bool SInsertMidiClipAction::readXml( const QDomElement &elem, int )
{
    trackPath_ = stringToPath( elem.attribute( "trackPath" ) );
    timePos_ = (offset_t) parseFractionOrDouble(
        elem.attribute( "timePos", "0" ).toStdString() ).toDouble();
    durationTicks_ = elem.attribute( "duration", "0" ).toLongLong();
    clipName_ = elem.attribute( "name", "" );
    events_.clear();
    return true;
}

static const bool s_reg_insert_midi_clip = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "insert-midi-clip" ),
        []{ return new SInsertMidiClipAction; } ), true );

// ---------------------------------------------------------------------------

SRemoveMidiClipAction::SRemoveMidiClipAction( const QList<int> &clipPath,
                                              const QList<int> &trackPath,
                                              offset_t timePos,
                                              qint64 durationTicks,
                                              const QString &name,
                                              std::vector<SEvent> events )
    : clipPath_( clipPath ), trackPath_( trackPath ), timePos_( timePos ),
      durationTicks_( durationTicks ), clipName_( name ),
      events_( std::move( events ) )
{
}

SApplyResult SRemoveMidiClipAction::apply( SProject *project )
{
    if( !project || clipPath_.isEmpty() ) return { false, nullptr };
    SObject *mixer = splacements::rootContainer( project );
    SLink *link = mixer ? splacements::placementAt( mixer, clipPath_ ) : nullptr;
    if( !link ) return { false, nullptr };
    delete link;   // the cut becomes unreferenced -> deleteLater
    return { true, new SInsertMidiClipAction( trackPath_, timePos_,
                                              durationTicks_, clipName_,
                                              events_ ) };
}

void SRemoveMidiClipAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "clip", pathToString( clipPath_ ) );
    elem.setAttribute( "trackPath", pathToString( trackPath_ ) );
}

bool SRemoveMidiClipAction::readXml( const QDomElement &, int )
{
    return false;   // created live as insert-midi-clip's inverse
}

// ---------------------------------------------------------------------------
// set-midi-cut
// ---------------------------------------------------------------------------

SSetMidiCutAction::SSetMidiCutAction( const QList<int> &clipPath, int transpose,
                                      double velocityScale, int channel,
                                      int take, bool broadcast )
    : clipPath_( clipPath ), transpose_( transpose ),
      velocityScale_( velocityScale ), channel_( channel ), take_( take ),
      broadcast_( broadcast )
{
}

SApplyResult SSetMidiCutAction::apply( SProject *project )
{
    if( broadcast_ ) {
        QList<QList<int>> targets =
            seditgroups::expandClipPaths( project, clipPath_ );
        if( targets.size() > 1 ) {
            SCompositeAction composite;
            for( const QList<int> &p : targets )
                composite.append( new SSetMidiCutAction(
                    p, transpose_, velocityScale_, channel_, take_, false ) );
            return composite.apply( project );
        }
    }
    ClipRef ref = smidiactions::resolveClip( project, clipPath_, take_ );
    if( !ref.valid() ) {
        qWarning() << "set-midi-cut: no MIDI clip at" << pathToString( clipPath_ );
        return { false, nullptr };
    }
    SAction *inverse = new SSetMidiCutAction(
        clipPath_, ref.cut->getTranspose(), ref.cut->getVelocityScale(),
        ref.cut->getChannelOverride(), take_, false );
    ref.cut->setTranspose( transpose_ );
    ref.cut->setVelocityScale( velocityScale_ );
    ref.cut->setChannelOverride( channel_ );
    return { true, inverse };
}

void SSetMidiCutAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "clip", pathToString( clipPath_ ) );
    elem.setAttribute( "transpose", transpose_ );
    elem.setAttribute( "velocityScale", QString::number( velocityScale_ ) );
    elem.setAttribute( "channel", channel_ );
    elem.setAttribute( "take", take_ );
    elem.setAttribute( "broadcast", broadcast_ ? 1 : 0 );
}

bool SSetMidiCutAction::readXml( const QDomElement &elem, int )
{
    clipPath_ = stringToPath( elem.attribute( "clip" ) );
    transpose_ = elem.attribute( "transpose", "0" ).toInt();
    velocityScale_ = elem.attribute( "velocityScale", "1" ).toDouble();
    channel_ = elem.attribute( "channel", "-1" ).toInt();
    take_ = elem.attribute( "take", "-1" ).toInt();
    broadcast_ = elem.attribute( "broadcast", "1" ).toInt() != 0;
    return true;
}

static const bool s_reg_set_midi_cut = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "set-midi-cut" ),
        []{ return new SSetMidiCutAction; } ), true );

// ---------------------------------------------------------------------------
// set-tempo
// ---------------------------------------------------------------------------

SSetTempoAction::SSetTempoAction( double bpm ) : bpm_( bpm ) {}

namespace {

/**
 * Re-derive `startTime` for every beats-timebase link in the project.
 *
 * It walks the PROJECT's own children rather than the arrangement tree: every
 * SObject is a child of the project (that is how the file is written), so this
 * one loop reaches nested containers, take-stack lanes and unplaced assets
 * alike, and reaches each link exactly once. The root link is not a child of
 * anything, so it is done by hand.
 */
int rederiveBeatsLinks( SProject *project )
{
    int moved = 0;
    for( QObject *child : project->children() ) {
        SObject *obj = dynamic_cast<SObject *>( child );
        if( !obj ) continue;
        for( SLink *lk : obj->childLinks() )
            if( lk && lk->rederiveStartTime() ) ++moved;
    }
    return moved;
}

}  // namespace

SApplyResult SSetTempoAction::apply( SProject *project )
{
    if( !project ) return { false, nullptr };
    if( !( bpm_ > 0.0 ) ) {
        qWarning() << "set-tempo: bpm must be positive, got" << bpm_;
        return { false, nullptr };
    }
    const double oldBpm = project->getBPMTempo();
    project->setBPMTempo( bpm_ );
    // The map moved, so every beats link's frame position is stale. Doing it
    // HERE, inside the action, is what makes the inverse exact: undo restores
    // the map and re-derives the same way, and the ticks never changed.
    rederiveBeatsLinks( project );
    return { true, new SSetTempoAction( oldBpm ) };
}

bool SSetTempoAction::mergeWith( const SAction *later )
{
    const SSetTempoAction *o = dynamic_cast<const SSetTempoAction *>( later );
    if( !o ) return false;
    bpm_ = o->bpm_;
    return true;
}

void SSetTempoAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "bpm", QString::number( bpm_, 'g', 17 ) );
}

bool SSetTempoAction::readXml( const QDomElement &elem, int )
{
    bool ok = false;
    bpm_ = elem.attribute( "bpm", "120" ).toDouble( &ok );
    if( !ok ) {
        qWarning() << "set-tempo::readXml: invalid bpm"
                   << elem.attribute( "bpm" );
        return false;
    }
    return true;
}

static const bool s_reg_set_tempo = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "set-tempo" ), []{ return new SSetTempoAction; } ), true );

// ---------------------------------------------------------------------------
// set-link-timebase
// ---------------------------------------------------------------------------

SSetLinkTimebaseAction::SSetLinkTimebaseAction( const QList<int> &clipPath,
                                                const QString &timebase )
    : clipPath_( clipPath ), timebase_( timebase )
{
}

SApplyResult SSetLinkTimebaseAction::apply( SProject *project )
{
    if( !project || clipPath_.isEmpty() ) return { false, nullptr };
    SObject *mixer = splacements::rootContainer( project );
    SLink *link = mixer ? splacements::placementAt( mixer, clipPath_ ) : nullptr;
    if( !link ) {
        qWarning() << "set-link-timebase: no clip at" << pathToString( clipPath_ );
        return { false, nullptr };
    }
    const QString before =
        ( link->getTimebase() == SLink::Timebase::Beats ) ? "beats" : "time";
    SLink::Timebase want;
    if( timebase_.compare( "beats", Qt::CaseInsensitive ) == 0 )
        want = SLink::Timebase::Beats;
    else if( timebase_.compare( "time", Qt::CaseInsensitive ) == 0 )
        want = SLink::Timebase::Time;
    else {
        qWarning() << "set-link-timebase: expected beats|time, got" << timebase_;
        return { false, nullptr };
    }
    link->setTimebase( want );
    return { true, new SSetLinkTimebaseAction( clipPath_, before ) };
}

void SSetLinkTimebaseAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "clip", pathToString( clipPath_ ) );
    elem.setAttribute( "timebase", timebase_ );
}

bool SSetLinkTimebaseAction::readXml( const QDomElement &elem, int )
{
    clipPath_ = stringToPath( elem.attribute( "clip" ) );
    timebase_ = elem.attribute( "timebase", "beats" );
    return true;
}

static const bool s_reg_set_link_timebase = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "set-link-timebase" ),
        []{ return new SSetLinkTimebaseAction; } ), true );
