

#include <QDebug>
#include <QStringList>

#include <iostream>

#include <stdlib.h>
#include <atomic>
#include <cstdint>
#include <vector>
#include <algorithm>
#include <cmath>

#include <qobject.h>

#include "tw/mix/twtrackmix.h"
#include "tw/mix/twmixer.h"
#include "tw/mix/twrewire.h"
#include "tw/mix/twgainstage.h"
#include "tw/plugins/twpluginchain.h"
#include "tw/plugins/twpluginauxtap.h"
#include "tw/plugins/twplugininsert.h"
#include "tw/plugins/twpluginslotproc.h"
#include "tw/graph/twlatch.h"
#include "app/model/sobject.h"
#include "app/model/sproject.h"
#include "app/model/slink.h"
#include "app/model/sappcontext.h"
#include "app/model/splacements.h"
#include "app/model/ssolorules.h"
#include "app/objects/track/strack.h"
#include "app/model/sprojectprops.h"
#include "app/model/sautofade.h"
#include "app/model/sautomationlane.h"
#include "app/model/sclipwindow.h"
#include "app/objects/track/strackrndrinline.h"
#include "app/objects/track/spluginchain.h"
#include "app/objects/track/spluginslot.h"
#include "app/objects/track/sfeelflowbounce.h"
#include "app/persistence/sprojectloader.h"
#include "tw/schedule/capture_aspects.h"  // Preview/Playback/... bits
#include "tw/sidecar/twgrooveaspect.h"    // proposal 40 M3: trained-structure (de)serialize
#include "tw/core/twlog.h"
#include "tw/events/tweventseq.h"

#include <QByteArray>
#include <limits>

// An event edit is never bounded on the right: the consumer is class-1 (a
// synth voice carries state across pages), so a change at `a` can be heard at
// any later position (design 3.2, F9). Spelled once here.
static constexpr offset_t EVENT_DIRTY_END =
    std::numeric_limits<offset_t>::max();

namespace {
// Proposal 41 D6: the channel remap lives on the SLINK (the placement), not
// on the shared content (D2 shares ONE SCut/fragment across every placement
// of an asset, so a remap stored on the content would move every placement
// at once - the opposite of what "a fragment authored on channel 10 placed
// on a track whose instrument wants channel 1" needs). Applied here, at the
// one place that already holds the SLink*, over whatever the content's own
// resolveEventFeed() returned. A NEW table only when actually overriding -
// the common -1 case shares the content's sequence with zero copying.
std::shared_ptr<const twEventSeq> remapEventChannel(
    const std::shared_ptr<const twEventSeq> &seq, int channel )
{
    if( channel < 0 || !seq ) return seq;
    std::vector<twEvent> events = seq->events();
    for( twEvent &e : events ) {
        if( e.channel >= 0 ) e.channel = (int16_t) channel;
    }
    return std::make_shared<const twEventSeq>( std::move( events ), seq->arena() );
}
}  // namespace

using namespace std;

int STrack::serializeSelfAttributes( QTextStream &o )
{
    // nBusses= IS DELIBERATELY NOT WRITTEN ANY MORE (proposal 36 B4).
    //
    // A track no longer HAS a bus count: it has one twTrackMix and one
    // twPluginChain, N channels wide, and N is the PROJECT's channels= — one
    // authority, read on construction and on SProject::channelsChanged. Keeping
    // a derived copy in every <STrack> would be a second authority that agrees
    // with the first only until someone edits a file by hand, and this exact
    // drift has already cost this project once: the loader defaulted the
    // attribute to "1" while the constructor built 2, so a .qxp that omitted it
    // asked for a SHRINK, which Q_ASSERT_X(false) "handled" by returning
    // silently in the build everyone runs (§7 trap 9).
    //
    // The read side stays TOLERANT — see readPreChildrenAttributes — so every
    // existing project still loads. What it does not do any more is decide
    // anything.
    // Which <SPluginChain> is OURS (proposal 08 M4). Every SObject is a child of
    // the project and is serialized from there, so a chain lands in the file
    // whether or not anyone claims it; before M4 nothing did, and a loaded chain
    // (with all its slots) was orphaned — the constructor's fresh empty chain won
    // and ~SProjectLoader dropped the loaded one to refcount 0.
    //
    // A plain attribute, not an <SLink> child: SObject::childEvent treats every
    // child link as a clip placement, and the loader's dependency ordering is
    // built on child links — which is exactly why the load side has to defer
    // (see instantiateFromDomElement).
    if( cpPluginChain_ )
        o << " pluginChainId='"
          << reinterpret_cast<std::uintptr_t>( (SObject *) cpPluginChain_ ) << "'";
    // Written only when it is not the default, so every project written before
    // proposal 37 re-serializes byte-identically (persistence invariant 4).
    if( midiRouting_ != MidiRouting::Auto )
        o << " midiRouting='" << midiRoutingToString( midiRouting_ ) << "'";
    // MIDI output (proposal 37 P7b), each written only when it is not the
    // default, for the same byte-identity reason as midiRouting above. The
    // PORT is the portable NAME; the machine-local device id lives in
    // SSettings, keyed by that name.
    if( !midiOutPort_.isEmpty() )
        o << " midiOutPort='" << midiOutPort_.toHtmlEscaped() << "'";
    if( midiOutChannel_ >= 0 )
        o << " midiOutChannel='" << midiOutChannel_ << "'";
    if( midiOutOffsetMs_ != 0 )
        o << " midiOutOffsetMs='" << midiOutOffsetMs_ << "'";
    // Live input / monitoring (proposal 21 L1b), same non-default-only rule:
    // a project written before this phase re-serializes byte-identically.
    // `liveOwnedLane_` is NEVER written - it is this session's monitoring
    // state, not a property of the project.
    if( !trackInput_.isEmpty() )
        o << " trackInput='" << trackInput_.toHtmlEscaped() << "'";
    if( monitorMode_ != MonitorMode::Auto )
        o << " monitorMode='" << monitorModeToString( monitorMode_ ) << "'";
    // Record mode (plan 53 §5.2), same non-default-only rule: empty means
    // INHERIT, which is what every project written before plan 53 means, so
    // such a project re-serializes byte-identically.
    if( !recordMode_.isEmpty() )
        o << " recordMode='" << recordMode_.toHtmlEscaped() << "'";
    // Fold state (fix/track-list-polish m), written only when true — the
    // default (expanded) is what every project saved before this attribute
    // existed means, so an unfolded track re-serializes byte-identically.
    if( collapsed_ )
        o << " collapsed='true'";
    // The other three pieces of per-track VIEW state (proposal 46 M3), each
    // written only when it is not the default, for the same byte-identity
    // reason as `collapsed` above: a track nobody has resized, expanded or
    // given an automation lane re-serializes exactly as it did before these
    // attributes existed.
    if( laneHeightScale_ != 1.0 )
        o << " laneHeightScale='" << laneHeightScale_ << "'";
    if( takesExpanded_ )
        o << " takesExpanded='true'";
    if( !shownAutomation_.isEmpty() )
        o << " shownAutomation='"
          << encodeShownAutomation( shownAutomation_ ).toHtmlEscaped() << "'";
    // ...and a FIFTH (proposal 48 M2): the mixer strip's narrow flag, on the
    // same terms. See strack.h for why it lives here rather than in the
    // pane-owned, pruned set proposal 48 D9 asks for -- that walk was retired
    // by 46 M3 and the reason D9 gives excludes the four above equally.
    if( mixerStripNarrow_ )
        o << " mixerStripNarrow='true'";
    // The SYSTEM ROLE (proposal 45 D1), written only when it is not None --
    // every ordinary track omits it, so every project written before system
    // lanes re-serializes byte-identically. It is IMMUTABLE, so unlike every
    // other attribute here there is no verb that can change it after a load.
    if( systemRole_ != SSystemRole::None )
        o << " systemRole='" << systemRoleToString( systemRole_ ) << "'";
    SObject::serializeSelfAttributes( o );
    return 0;
}

void STrack::setLaneHeightScale( double s )
{
    if( s < LANE_HEIGHT_SCALE_MIN ) s = LANE_HEIGHT_SCALE_MIN;
    if( s > LANE_HEIGHT_SCALE_MAX ) s = LANE_HEIGHT_SCALE_MAX;
    laneHeightScale_ = s;
}

// See the header: ONE spelling of the shown-lane identity, used by the
// serializer, the loader and the arranger alike.
QString STrack::encodeShownAutomation( const QList<ShownAutomationLane> &lanes )
{
    QStringList parts;
    parts.reserve( lanes.size() );
    for( const ShownAutomationLane &l : lanes ) {
        if( l.target.isEmpty() ) continue;
        parts << ( l.slotIndex >= 0
                       ? QStringLiteral( "%1@%2" ).arg( l.target ).arg( l.slotIndex )
                       : l.target );
    }
    return parts.join( QLatin1Char( ';' ) );
}

QList<STrack::ShownAutomationLane>
STrack::decodeShownAutomation( const QString &text )
{
    QList<ShownAutomationLane> out;
    if( text.isEmpty() ) return out;
    const QStringList parts =
        text.split( QLatin1Char( ';' ), Qt::SkipEmptyParts );
    for( const QString &part : parts ) {
        ShownAutomationLane l;
        const int at = part.lastIndexOf( QLatin1Char( '@' ) );
        if( at >= 0 ) {
            bool ok = false;
            const int slot = part.mid( at + 1 ).toInt( &ok );
            // A trailing '@' with no number, or a non-numeric slot, is a
            // spelling this build does not understand: skip the ENTRY rather
            // than half-read it into a lane pointing at slot 0.
            if( !ok ) continue;
            l.target    = part.left( at );
            l.slotIndex = slot;
        } else {
            l.target = part;
        }
        if( l.target.isEmpty() ) continue;
        // Duplicates would give the track two identical rows.
        if( !out.contains( l ) ) out.append( l );
    }
    return out;
}

STrack::MidiRouting STrack::midiRoutingFromString( const QString &s, bool *ok )
{
    if( ok ) *ok = true;
    if( s.compare( "parent", Qt::CaseInsensitive ) == 0 ) return MidiRouting::Parent;
    if( s.compare( "none",   Qt::CaseInsensitive ) == 0 ) return MidiRouting::None;
    if( s.compare( "auto",   Qt::CaseInsensitive ) == 0 ) return MidiRouting::Auto;
    if( ok ) *ok = false;
    return MidiRouting::Auto;
}

QString STrack::midiRoutingToString( MidiRouting r )
{
    switch( r ) {
    case MidiRouting::Parent: return QStringLiteral( "parent" );
    case MidiRouting::None:   return QStringLiteral( "none" );
    default:                  return QStringLiteral( "auto" );
    }
}

void STrack::setMidiOutput( const QString &port, int channel, int offsetMs )
{
    if( channel < -1 )  channel = -1;
    if( channel > 15 )  channel = 15;
    if( offsetMs >  MIDI_OUT_MAX_OFFSET_MS ) offsetMs =  MIDI_OUT_MAX_OFFSET_MS;
    if( offsetMs < -MIDI_OUT_MAX_OFFSET_MS ) offsetMs = -MIDI_OUT_MAX_OFFSET_MS;
    if( port == midiOutPort_ && channel == midiOutChannel_
        && offsetMs == midiOutOffsetMs_ ) return;

    const bool hadOut = hasMidiOut();
    midiOutPort_     = port;
    midiOutChannel_  = channel;
    midiOutOffsetMs_ = offsetMs;

    // Gaining or losing a port changes `bubblesEventsUp()` under the `auto`
    // rule ("consumed here, or bubbled up"), so the PARENT's feed - and with it
    // its instrument - now sees a different stream. The class-1 consumer is
    // never bounded on the right, hence the open-ended range.
    if( hadOut != hasMidiOut() )
        invalidateRenderPathRange( 0, EVENT_DIRTY_END );
}

// --- live input / monitoring (proposal 21 L1b, design D9) -------------------

void STrack::setTrackInput( const QString &spec )
{
    QString v = spec.trimmed();
    if( v == QStringLiteral( "none" ) ) v.clear();
    if( v == trackInput_ ) return;
    trackInput_ = v;
    // NOTHING is invalidated here. A live input changes what the LIVE LANE
    // renders, never what a frozen page contains: the exclusion wiring (and
    // the epoch bump that goes with it) is applied by SLivePlanBuilder when the
    // lane actually arms, which is the only moment the frozen sum changes.
    emit trackInputChanged();
}

QString STrack::trackInputAudioDevice() const
{
    if( !trackInput_.startsWith( QStringLiteral( "audio:" ) ) ) return QString();
    // audio:<device>:<mask> - the device may itself be empty ("audio::3").
    const QString rest = trackInput_.mid( 6 );
    const int     colon = rest.lastIndexOf( QLatin1Char( ':' ) );
    return ( colon < 0 ) ? rest : rest.left( colon );
}

unsigned STrack::trackInputChannelMask() const
{
    if( !trackInput_.startsWith( QStringLiteral( "audio:" ) ) ) return 0u;
    const QString rest  = trackInput_.mid( 6 );
    const int     colon = rest.lastIndexOf( QLatin1Char( ':' ) );
    if( colon < 0 ) return 0u;
    bool ok = false;
    const unsigned mask = rest.mid( colon + 1 ).toUInt( &ok, 16 );
    return ok ? mask : 0u;
}

QString STrack::trackInputMidiPort() const
{
    // `keyboard` is a whole spelling of its own, not a scheme: it names the
    // ONE in-process port (audio::KeyboardMidiInput::kPortId), and spelling it
    // `midi:keyboard:any` would work too.
    if( trackInput_ == QStringLiteral( "keyboard" ) )
        return QStringLiteral( "keyboard" );
    if( !trackInput_.startsWith( QStringLiteral( "midi:" ) ) ) return QString();
    // midi:<port>:<ch|any> - the port name may contain no colon; the LAST one
    // separates the channel, exactly as the audio spelling separates the mask.
    const QString rest  = trackInput_.mid( 5 );
    const int     colon = rest.lastIndexOf( QLatin1Char( ':' ) );
    const QString port  = ( colon < 0 ) ? rest : rest.left( colon );
    // An empty port ("midi::any") means "the default input port", which is what
    // an empty device means on the audio side.
    return port;
}

int STrack::trackInputMidiChannel() const
{
    if( trackInput_ == QStringLiteral( "keyboard" ) ) return -1;
    if( !trackInput_.startsWith( QStringLiteral( "midi:" ) ) ) return -1;
    const QString rest  = trackInput_.mid( 5 );
    const int     colon = rest.lastIndexOf( QLatin1Char( ':' ) );
    if( colon < 0 ) return -1;
    const QString ch = rest.mid( colon + 1 ).trimmed();
    if( ch.isEmpty() || ch.compare( QStringLiteral( "any" ),
                                    Qt::CaseInsensitive ) == 0 )
        return -1;
    bool ok = false;
    const int v = ch.toInt( &ok );
    return ( ok && v >= 0 && v < 16 ) ? v : -1;
}

void STrack::setRecordMode( const QString &m )
{
    const QString next = SProjectProps::isRecordMode( m ) ? m : QString();
    if( next == recordMode_ ) return;
    recordMode_ = next;
    // No render consequence: the mode decides how the NEXT recording lands, so
    // nothing cached or frozen depends on it. Deliberately no
    // invalidateRenderPath() and no epoch bump.
}

STrack::MonitorMode STrack::monitorModeFromString( const QString &s, bool *ok )
{
    if( ok ) *ok = true;
    if( s.compare( "on",   Qt::CaseInsensitive ) == 0 ) return MonitorMode::On;
    if( s.compare( "off",  Qt::CaseInsensitive ) == 0 ) return MonitorMode::Off;
    if( s.compare( "auto", Qt::CaseInsensitive ) == 0 ) return MonitorMode::Auto;
    if( ok ) *ok = false;
    return MonitorMode::Auto;
}

QString STrack::monitorModeToString( MonitorMode m )
{
    switch( m ) {
    case MonitorMode::On:  return QStringLiteral( "on" );
    case MonitorMode::Off: return QStringLiteral( "off" );
    default:               return QStringLiteral( "auto" );
    }
}

void STrack::setMonitorMode( MonitorMode m )
{
    if( m == monitorMode_ ) return;
    monitorMode_ = m;
    emit trackInputChanged();
}

bool STrack::monitorEffective( bool playing, bool recording ) const
{
    switch( monitorMode_ ) {
    case MonitorMode::Off: return false;
    case MonitorMode::On:  return true;
    default: break;
    }
    // AUTO is the tape machine: the input is heard while the transport is
    // STOPPED or while a record pass is running, and gives way to the track's
    // own material on plain Play. That is Cubase "Tapemachine" / REAPER
    // "auto", and it is what makes pressing Play an audition of what is on
    // the timeline rather than a duet with the performer.
    return recording || !playing;
}

void STrack::setMidiRouting( MidiRouting r )
{
    if( r == midiRouting_ ) return;
    midiRouting_ = r;
    // The feed is rebuilt on read, so nothing to poke here — but the parent's
    // instrument (and, from P7, its MIDI-out) hears a different stream from
    // now on, and the class-1 consumer is never bounded on the right.
    invalidateRenderPathRange( 0, EVENT_DIRTY_END );
}

SPluginSlot *STrack::instrumentSlot() const
{
    if( !cpPluginChain_ || cpPluginChain_->getSlotCount() <= 0 ) return nullptr;
    SPluginSlot *slot = cpPluginChain_->getSlotAt( 0 );
    // At most one instrument per track, always slot 0 (D3).
    return ( slot && slot->getDescriptor().isInstrument ) ? slot : nullptr;
}

bool STrack::hasEventClips() const
{
    for( SLink *lk : childLinks() )
        if( lk && lk->getSObject().contentKind() == SContentKind::Event )
            return true;
    return false;
}

bool STrack::bubblesEventsUp() const
{
    switch( midiRouting_ ) {
    case MidiRouting::Parent: return true;      // explicit route (Cubase/Logic)
    case MidiRouting::None:   return false;     // keep them local
    default: break;
    }
    // auto: "consumed here, or bubbled up". REAPER's analogue is MIDI passing
    // through a child's empty FX chain to the folder parent's input.
    return instrumentSlot() == nullptr && !hasMidiOut();
}

std::shared_ptr<twEventMerge> STrack::eventFeed()
{
    if( !eventFeed_ ) eventFeed_ = std::make_shared<twEventMerge>();

    // Rebuilt on every read (see the header): child add/remove/reparent, a
    // mute, a solo ANYWHERE in the project and a routing change all move this
    // list, and solo is global — a dirty flag would have to be poked from the
    // whole tree, which is the coupling ssolorules.h exists to avoid.
    std::vector<std::shared_ptr<const twEventSource>> sources;
    // eventClips_ is now TWO source classes at once (proposal 41 D4/M3): our
    // own plain event clips (as always) PLUS any placement whose content
    // answered resolveEventFeed() with something residual — a lane fragment
    // — dual-inserted alongside its audio sum in trackChildWasAdded. A
    // container asset with its own instrument answers nothing there (D4:
    // STrack/SStdMixer do not override resolveEventFeed()), so it never
    // reaches eventClips_ and is never double-triggered.
    sources.push_back( eventClips_ );

    // THE ROOT THAT HOLDS US (QBX-245/QBX-246), object first: all we have is
    // our own identity, and the solo rule's `root` argument is the top of the
    // tree it walks for a soloed ANCESTOR. Asking for the master answered
    // about a tree we are not in whenever this track lives in an arrangement.
    SObject *root = splacements::rootHolding( getProjectSafe(), this );
    const bool anySolo = ssolo::anySoloInForce( root );
    for( SLink *lk : childLinks() ) {
        if( !lk ) continue;
        STrack *child = dynamic_cast<STrack *>( &lk->getSObject() );
        if( !child || !child->bubblesEventsUp() ) continue;
        // A muted or solo-excluded child contributes nothing — the same rule
        // the summing container applies to its audio (3.2.1).
        if( !ssolo::isLaneAudible( root, child, anySolo ) ) continue;
        sources.push_back( child->eventFeed() );
    }
    eventFeed_->setSources( std::move( sources ) );
    return eventFeed_;
}

// Design 4.3: the instrument reads the track's FEED through a twEventSource
// pointer, and the feed is 3.2.1's merge - this track's own event clips plus
// the feeds of every child that bubbles up. Slot 0 gets it; every other slot
// has it taken away, so a demoted slot cannot keep sounding notes.
void STrack::syncInstrumentSlot()
{
    if( !cpPluginChain_ ) return;
    SPluginSlot *instr = instrumentSlot();
    const int n = cpPluginChain_->getSlotCount();
    for( int i = 0; i < n; ++i ) {
        SPluginSlot *slot = cpPluginChain_->getSlotAt( i );
        if( !slot ) continue;
        if( slot == instr ) {
            slot->setEventSource( eventFeed() );
            if( SProject *p = getProjectSafe() ) slot->setTempoMap( p->tempoMap() );
        } else {
            slot->setEventSource( nullptr );
        }
    }
}

void STrack::refreshInstrumentFeed()
{
    // Guarded, so a project with no instrument anywhere pays nothing for this
    // on an ordinary clip edit: instrumentSlot() is two pointer hops.
    if( !instrumentSlot() ) return;
    // The processor already holds the merge OBJECT; this rebuilds its sources.
    eventFeed();
}

QWidget *STrack::getDetailEditWidget( QWidget * )
{
    return NULL;
}

QWidget *STrack::getInlineEditWidget( QWidget * )
{
    return NULL;
}

SObjectRenderer *STrack::getInlineRenderer()
{
    if( !inlineRenderer_ ) {
        inlineRenderer_ = new STrackRendererInline( *this );
    }
    return inlineRenderer_;
}

/**
 * We define a track's duration as the ending point of the last terminated event,
 * otherwise 1.
 */
bool STrack::hasDuration() const
{
    return true;
}

// The end of the last EVENT material reaching this track's feed. Own Event
// children plus the same question of every child that bubbles up - the same
// walk eventFeed() makes, minus the mute/solo resolution: the PROJECT END must
// not shrink because a lane happens to be muted right now.
offset_t STrack::eventEndTime() const
{
    offset_t end = 0;
    for( SLink *lk : childLinks() ) {
        if( !lk ) continue;
        SObject &obj = lk->getSObject();
        if( STrack *child = dynamic_cast<STrack *>( &obj ) ) {
            if( !child->bubblesEventsUp() ) continue;
            const offset_t childEnd = child->eventEndTime();
            if( childEnd <= 0 ) continue;
            const offset_t at = ( lk->hasStartTime() ? lk->getStartTime() : 0 ) + childEnd;
            if( at > end ) end = at;
            continue;
        }
        if( obj.contentKind() != SContentKind::Event ) continue;
        if( !lk->hasStartTime() ) continue;
        const offset_t at = lk->getStartTime()
                          + ( obj.hasDuration() ? (offset_t)obj.getDuration() : 0 );
        if( at > end ) end = at;
    }
    return end;
}

/**
 * We define a track's duration as the ending point of the last terminated event,
 * otherwise 1.
 *
 * PROPOSAL 37 P3b adds the instrument's TAIL (design 3.1): the project end of a
 * track whose slot 0 is an instrument is `last event clip end + tailFrames()`,
 * because the notes stop at the clip end and the sound does not. The tail is
 * added to the EVENT extent only - an audio clip must not gain a synth release,
 * and the max with the audio extent below keeps a longer audio clip winning.
 */
length_t STrack::getDuration() const
{
    if( !lastDurationValid_ ) {
        offset_t first, last = 0;
        int nUndefStart, nUndefDuration;
        getChildrenExtent( first, last, nUndefStart, nUndefDuration );
	//	qWarning( "STrack::getDuration(): last = %d.\n", (int) last );
        if( SPluginSlot *slot = instrumentSlot() ) {
            const offset_t evEnd = eventEndTime();
            // tailFrames() never materializes a plugin (see SPluginSlot): this
            // runs from getChildrenExtent(), which a render can reach.
            if( evEnd > 0 ) {
                const offset_t withTail = evEnd + (offset_t)slot->tailFrames();
                if( withTail > last ) last = withTail;
            }
        }
        if( last>0 ) {
            lastDuration_ = last;
        } else {
            lastDuration_ = 1;
        }
        lastDurationValid_ = true;
    }
    return lastDuration_;
}

std::shared_ptr<twComponent> STrack::getRootComponent()
{
    // PROPOSAL 45 M2 / D12 -- THE MASTER LANE'S OUTPUT IS THE MIXER'S REWIRE,
    // not this track's own.
    //
    // M2 wired the lane's chain and gain stage between the bus sum and the
    // MIXER's rewire, and wireAsMasterLane() now DISCONNECTS this track's own
    // `cpRewire_` so it really is inert (T5, AC2.8). It was not, for two
    // milestones: it carried the identical signal, which is why removing this
    // override used to leave master_meter_postfx green. A
    // meter probe pointed at it would read silence forever -- and
    // twLevelProbe answers a miss by DECAYING (proposal 34: "a page miss must
    // DECAY the meter"), so the symptom is a meter that looks BROKEN rather
    // than a wiring bug that looks like one.
    //
    // Overridden HERE rather than special-cased at the two meter mounts
    // because both already call track.getRootComponent() uniformly: one
    // answer, at the one place every consumer asks. The consequences are named
    // so they are not discovered later -- this override is also what
    // resolveClip(), preview building and the live plan builder's channelMap
    // read, and for the master lane each of those is CORRECT under D3: the
    // mixer's rewire genuinely is this lane's output.
    if( systemRole() == SSystemRole::Master && renderPathOwner_ )
        return renderPathOwner_->getRootComponent();
    return std::static_pointer_cast<twComponent>(cpRewire_);
}

// --- Feel Flow track bounce (proposal 40 M1b) ------------------------------

void STrack::startFeelFlowBounce()
{
    if( !feelFlowBounce_ )
        feelFlowBounce_ = std::make_unique<SFeelFlowTrackBounce>( this );
    feelFlowBounce_->start();
}

bool STrack::isFeelFlowBouncing() const
{
    return feelFlowBounce_ && feelFlowBounce_->isBouncing();
}

bool STrack::feelFlowHasResult() const
{
    return feelFlowBounce_ && feelFlowBounce_->hasResult();
}

bool STrack::feelFlowStale() const
{
    // No bounce ever started: not "fresh", not meaningfully "stale" either,
    // but callers that ask before ever bouncing should get the safe answer.
    return !feelFlowBounce_ || feelFlowBounce_->isStale();
}

std::shared_ptr<const SFeelFlowUiData> STrack::feelFlowForUi() const
{
    if( !feelFlowBounce_ ) return nullptr;   // never bounced: no holder yet
    return feelFlowBounce_->feelFlowForUi();
}

// --- Feel Flow tuning panel + trained mode (proposal 40 M3) ----------------

STrack::FeelFlowMode STrack::feelFlowModeFromString( const QString &s, bool *ok )
{
    if( ok ) *ok = true;
    if( s.compare( "trained", Qt::CaseInsensitive ) == 0 ) return FeelFlowMode::Trained;
    if( s.compare( "adaptive", Qt::CaseInsensitive ) == 0 ) return FeelFlowMode::Adaptive;
    if( ok ) *ok = false;
    return FeelFlowMode::Adaptive;
}

QString STrack::feelFlowModeToString( FeelFlowMode m )
{
    switch( m ) {
    case FeelFlowMode::Trained: return QStringLiteral( "trained" );
    default:                    return QStringLiteral( "adaptive" );
    }
}

void STrack::setFeelFlowTrainedStructureInternal(
    std::unique_ptr<twGrooveTrainedStructure> structure )
{
    feelFlowTrained_ = std::move( structure );
}

std::unique_ptr<twGrooveTrainedStructure> STrack::copyFeelFlowTrainedStructure() const
{
    if( !feelFlowTrained_ ) return nullptr;
    return std::make_unique<twGrooveTrainedStructure>( *feelFlowTrained_ );
}

std::string STrack::feelFlowBouncePath() const
{
    return feelFlowBounce_ ? feelFlowBounce_->bouncePath() : std::string();
}

void STrack::setFeelFlowBandMetricId( const std::string &idList )
{
    // M3d: a comma-separated LIST, normalized here (split, trimmed, empties
    // dropped) so a cleared selector/verb attribute can never leave the
    // band with an unmatchable spelling -- an empty list is the default.
    std::vector<std::string> ids;
    std::string cur;
    for( char c : idList ) {
        if( c == ',' ) {
            if( !cur.empty() ) ids.push_back( cur );
            cur.clear();
        } else if( c != ' ' ) {
            cur += c;
        }
    }
    if( !cur.empty() ) ids.push_back( cur );
    if( ids.empty() ) ids.push_back( "compliance" );

    std::string want;
    for( size_t i = 0; i < ids.size(); i++ ) {
        if( i > 0 ) want += ',';
        want += ids[i];
    }
    if( feelFlowBandMetricId_ == want ) return;
    feelFlowBandMetricId_  = want;
    feelFlowBandMetricIds_ = std::move( ids );
    // Repaint through the SAME funnel an analysis completion uses (proposal
    // 40 M2 AC 3: "repaint rides captureRevalidated()"). A view preference
    // changes pixels, never audio -- no epoch is bumped, nothing is staled.
    SProject *proj = SAppContext::get().getCurrentProject();
    if( proj ) proj->notifyCaptureRevalidated();
}

// A full override, not just serializeSelfAttributes(): the trained structure
// is an INLINE, non-<SLink> element (the automation-lane discipline, design
// section 4.4 — "invisible to older loaders, the automation-lane
// discipline") and has to land between the ">" and the child links, exactly
// where SObject::serialize() already puts serializeAutomation(). This
// mirrors SObject::serialize() body-for-body rather than trying to hook into
// it, because SObject offers no seam there — see SMidiSequence::serialize()
// for the same shape used for a different inline payload (<events>).
int STrack::serialize( QTextStream &o )
{
    o << "<" << metaObject()->className();
    if( serializeSelfAttributes( o ) < 0 ) return -1;
    o << ">\n";

    serializeAutomation( o );
    // Proposal 47 M0. STrack mirrors SObject::serialize() body-for-body
    // rather than hooking into it (see the comment above), so every inline
    // child SObject writes has to be repeated here — and one that is NOT
    // repeated is silently dropped on save with no error anywhere.
    serializeSends( o );

    // THE AUX SOURCE (proposal 55 D8), inline and ONLY WHEN SET, so every
    // project without a return re-serializes byte-identically (47 T8). The
    // track is an object id exactly as pluginChainId is; the slot is its
    // stable slotId, never an index; the bus is the plugin's own bus index. A
    // source that did not resolve is written as track='0' so the return stays
    // a return (and stays silent) across a save.
    if( isReturnTrack() ) {
        const std::uintptr_t src = auxTrack_
            ? reinterpret_cast<std::uintptr_t>( (SObject *) auxTrack_.data() ) : 0;
        o << "<auxSource track='" << src << "' slot='" << auxSlotId_
          << "' bus='" << auxBus_ << "'/>\n";
    }

    // Written ONLY when non-default (mode == Trained, or a structure was
    // learned while still in Adaptive mode) -- so a project that has never
    // touched Feel Flow's mode re-serializes BYTE-IDENTICAL to a pre-M3
    // build, and every existing golden stays untouched.
    if( feelFlowMode_ == FeelFlowMode::Trained || feelFlowTrained_ ) {
        o << "<feelflow mode='" << feelFlowModeToString( feelFlowMode_ ) << "'";
        if( !feelFlowTrained_ ) {
            o << "/>\n";
        } else {
            o << ">\n";
            std::vector<uint8_t> blob;
            twGrooveTrainedStructureSerialize( *feelFlowTrained_, blob );
            const QByteArray b64 = QByteArray(
                (const char *) blob.data(), (int) blob.size() ).toBase64();
            o << " <trained data='" << QString::fromLatin1( b64 ) << "'/>\n";
            o << "</feelflow>\n";
        }
    }

    for( SLink *lk : childLinks() ) {
        int res = lk->serialize( o );
        if( res < 0 ) break;
    }

    o << "</" << metaObject()->className() << ">\n";
    return 0;
}

int STrack::readPostChildrenAttributes( QDomElement &element )
{
    SObject::readPostChildrenAttributes( element );   // <automation>

    const QDomElement ffEl = element.firstChildElement( "feelflow" );
    if( ffEl.isNull() ) return 0;   // absent == every pre-M3 project's meaning

    feelFlowMode_ = feelFlowModeFromString( ffEl.attribute( "mode", "adaptive" ) );

    const QDomElement trainedEl = ffEl.firstChildElement( "trained" );
    if( !trainedEl.isNull() ) {
        const QByteArray b64 = trainedEl.attribute( "data" ).toLatin1();
        const QByteArray blob = QByteArray::fromBase64( b64 );
        auto structure = std::make_unique<twGrooveTrainedStructure>();
        if( twGrooveTrainedStructureDeserialize(
                (const uint8_t *) blob.constData(), (uint64_t) blob.size(),
                *structure ) ) {
            feelFlowTrained_ = std::move( structure );
        } else {
            TW_LOGW( "model", "STrack: ignoring malformed <trained> Feel Flow "
                              "structure (%d bytes)", (int) blob.size() );
        }
    }
    return 0;
}

int STrack::seekTo( offset_t ofs )
{
    // seek(): app model -> engine component, an EXTERNAL seek (see
    // twComponent::seek). The mix's own clip cascade below it stays on
    // seekTo(), being internal to the mix.
    if( cpTrackMix_ ) cpTrackMix_->seek( ofs );
    return 0;
}

void STrack::wireAsMasterLane( const std::shared_ptr<twMixer> &sum,
                               const std::shared_ptr<twRewire> &rewire )
{
    if( !sum || !rewire || !cpDspChain_ || !cpGainStage_ ) return;
    cpDspChain_->setInput( 0, sum->linkOutput( 0 ) );
    cpDspChain_->rebuildWiring();
    cpGainStage_->setInput( 0, cpDspChain_->linkOutput( 0 ) );
    rewire->setInput( 0, cpGainStage_->linkOutput( 0 ) );

    // AC2.8, WHICH WAS FALSE UNTIL THIS LINE. The AC says the lane's own
    // twTrackMix and twRewire are unwired; the trackmix genuinely is, and the
    // REWIRE WAS NOT. STrack's ctor wires `cpRewire_.input = cpGainStage_
    // .output` and the four lines above only ALSO take the gain stage's output
    // into the MIXER's rewire -- so the lane's own rewire went on carrying the
    // identical signal. Redundant, not inert.
    //
    // That was found by SABOTAGE, not by reading: removing D12's
    // getRootComponent() override left master_meter_postfx GREEN, because the
    // lane's rewire answered with the same audio the mixer's would have. A
    // green gate over an override nothing could prove was needed.
    //
    // Disconnecting it makes both claims true at once: AC2.8 as written, and
    // D12's override as something a sabotage can actually bite. It also drops
    // a SECOND page cache that nothing invalidates -- the invalidation path
    // bumps the mixer's rewire, so the lane's own could only ever go stale.
    //
    // cpRewire_ itself stays alive and keeps taking setChannels() and
    // bumpContentEpoch(); it is its INPUT that goes, which is the one thing
    // that made it carry audio.
    if( cpRewire_ ) cpRewire_->setInput( 0, nullptr );
}

void STrack::wireAsSendLane( const std::shared_ptr<twMixer> &sum )
{
    if( !sum || !cpDspChain_ || !cpGainStage_ || !cpRewire_ ) return;
    cpDspChain_->setInput( 0, sum->linkOutput( 0 ) );
    cpDspChain_->rebuildWiring();
    cpGainStage_->setInput( 0, cpDspChain_->linkOutput( 0 ) );
    // ...and, UNLIKE the master lane, into this lane's OWN rewire, which is
    // what the master sum already reads as this lane's root component. See the
    // header for why that one difference is the whole of D1.
    cpRewire_->setInput( 0, cpGainStage_->linkOutput( 0 ) );
}

void STrack::unwireSendLane()
{
    if( !cpDspChain_ ) return;
    // Back to the constructor's wiring (strack.cpp's own
    // `cpDspChain_->setInput( 0, cpTrackMix_->linkOutput( 0 ) )`), so a lane
    // that stops being a send lane is an ordinary track rather than one whose
    // chain has no input at all.
    cpDspChain_->setInput( 0, cpTrackMix_ ? cpTrackMix_->linkOutput( 0 )
                                          : nullptr );
    cpDspChain_->rebuildWiring();
    if( cpGainStage_ ) cpGainStage_->setInput( 0, cpDspChain_->linkOutput( 0 ) );
    if( cpRewire_ )    cpRewire_->setInput( 0, cpGainStage_
                                                   ? cpGainStage_->linkOutput( 0 )
                                                   : nullptr );
}

std::shared_ptr<twComponent> STrack::sendTapComponent( bool preFader ) const
{
    if( preFader )
        return std::static_pointer_cast<twComponent>( cpDspChain_ );
    return std::static_pointer_cast<twComponent>( cpGainStage_ );
}

// --- RETURN TRACKS (proposal 55 D1, D3, D8, D9; M2) --------------------------

namespace {
/// Ask the root that holds `lane` to re-run its routing pass. A lane outside
/// every root (removed, alive on the undo stack) has no pass to re-run.
void requestRoutingPass( SObject *lane )
{
    SProject *project = lane ? lane->getProjectSafe() : nullptr;
    if( !project || project->isLoading() ) return;
    if( SObject *root = splacements::rootHolding( project, lane ) )
        root->routingInputsChanged();
}
}  // namespace

void STrack::setAuxSource( STrack *source, quint64 slotId, int bus )
{
    if( bus <= 0 ) { source = nullptr; slotId = 0; bus = 0; }
    if( auxTrack_.data() == source && auxSlotId_ == slotId && auxBus_ == bus )
        return;
    auxTrack_  = source;
    auxSlotId_ = slotId;
    auxBus_    = bus;
    returnState_.clear();   // a new source is announced afresh
    requestRoutingPass( this );
}

int STrack::pluginSlotCount() const
{
    return cpPluginChain_ ? cpPluginChain_->getSlotCount() : 0;
}

quint64 STrack::pluginSlotId( int slotIndex, bool ensure )
{
    SPluginSlot *slot = cpPluginChain_ ? cpPluginChain_->getSlotAt( slotIndex ) : nullptr;
    if( !slot ) return 0;
    return ensure ? slot->ensureSlotId() : slot->slotId();
}

int STrack::pluginSlotIndexOf( quint64 slotId ) const
{
    if( slotId == 0 || !cpPluginChain_ ) return -1;
    const int n = cpPluginChain_->getSlotCount();
    for( int i = 0; i < n; ++i ) {
        SPluginSlot *slot = cpPluginChain_->getSlotAt( i );
        if( slot && slot->slotId() == slotId ) return i;
    }
    return -1;
}

QString STrack::pluginSlotName( int slotIndex ) const
{
    SPluginSlot *slot = cpPluginChain_ ? cpPluginChain_->getSlotAt( slotIndex ) : nullptr;
    if( !slot ) return QString();
    return QString::fromStdString( slot->getEffectiveDescriptor().name );
}

std::uint32_t STrack::pluginSlotRoutedMask( int slotIndex ) const
{
    SPluginSlot *slot = cpPluginChain_ ? cpPluginChain_->getSlotAt( slotIndex ) : nullptr;
    return slot ? slot->routedOutBuses() : 0u;
}

bool STrack::auxBusRoutable( int slotIndex, int bus, QString *why,
                             int *busChannels ) const
{
    auto refuse = [why]( const QString &w ) { if( why ) *why = w; return false; };
    SPluginSlot *slot = cpPluginChain_ ? cpPluginChain_->getSlotAt( slotIndex ) : nullptr;
    if( !slot )
        return refuse( QStringLiteral( "there is no plugin in slot %1" ).arg( slotIndex ) );
    if( bus < 1 )
        return refuse( QStringLiteral( "bus %1 is the MAIN bus, the track's own output; "
                                       "a return reads an aux bus (1 or above)" ).arg( bus ) );
    // D9: on DualMono "the plugin's bus k" has one candidate per channel.
    if( slot->getSlotMode() == audio::twPluginSlotMode::DualMono )
        return refuse( QStringLiteral( "slot %1 runs DualMono (one plugin instance per "
                                       "channel), so its aux bus %2 has no single source" )
                           .arg( slotIndex ).arg( bus ) );
    if( slot->getSlotState() != audio::twPluginSlotState::Active )
        return refuse( QStringLiteral( "the plugin in slot %1 is not loaded, so it has no "
                                       "buses to route" ).arg( slotIndex ) );
    const int nBuses = slot->auxOutBusCount();
    if( bus >= nBuses )
        return refuse( QStringLiteral( "the plugin in slot %1 has no output bus %2 (it "
                                       "reports %3)" ).arg( slotIndex ).arg( bus ).arg( nBuses ) );
    const int ch = slot->auxOutBusChannels( bus );
    if( ch <= 0 )
        return refuse( QStringLiteral( "output bus %1 of slot %2 reports no channels" )
                           .arg( bus ).arg( slotIndex ) );
    if( busChannels ) *busChannels = ch;
    return true;
}

bool STrack::wireAsReturnTrack( STrack *source, const QString &reason, bool *changed )
{
    if( changed ) *changed = false;
    if( !cpDspChain_ || !cpGainStage_ || !cpRewire_ ) return false;

    QString why = reason;
    std::shared_ptr<audio::twPluginInsert> insert;
    int busChannels = 0;
    if( why.isEmpty() && !source )
        why = QStringLiteral( "its source track is gone" );
    if( why.isEmpty() ) {
        const int idx = source->pluginSlotIndexOf( auxSlotId_ );
        if( idx < 0 ) {
            why = QStringLiteral( "the slot it names on '%1' is gone" )
                      .arg( source->getSName() );
        } else if( source->auxBusRoutable( idx, auxBus_, &why, &busChannels ) ) {
            SPluginSlot *slot = source->cpPluginChain_->getSlotAt( idx );
            insert = slot ? slot->getInsert() : nullptr;
            if( !insert ) why = QStringLiteral( "the source slot has no insert" );
        }
    }

    // THE TAP IS ALWAYS THE CHAIN'S INPUT, routed or not: a return that cannot
    // be routed is SILENT (fail closed, D8), never its own empty clip mix. Its
    // shape is fixed at construction (55 D4: the mapping is decided once), so
    // a changed width, bus or bus width builds a new one.
    const idx_t width = (idx_t) ( channels_ > 0 ? channels_ : 1 );
    const std::uint16_t wantBus = (std::uint16_t) ( auxBus_ > 0 ? auxBus_ : 1 );
    const std::uint16_t wantBusCh = insert ? (std::uint16_t) busChannels
                                  : cpAuxTap_ ? cpAuxTap_->busChannels()
                                              : (std::uint16_t) width;
    if( !cpAuxTap_ || cpAuxTap_->getOutputChannels() != width
        || cpAuxTap_->bus() != wantBus || cpAuxTap_->busChannels() != wantBusCh ) {
        if( cpAuxTap_ ) cpAuxTap_->setInput( 0, nullptr );   // while auxInsert_ lives
        auto tap = std::make_shared<audio::twPluginAuxTap>(
            *SAppContext::get().get303aEnvironment(), width, wantBus, wantBusCh );
        tap->init();
        cpAuxTap_ = tap;
        if( changed ) *changed = true;
    }
    if( changed && insert != auxInsert_ ) *changed = true;

    // RE-POINTED ON EVERY PASS, which re-stamps (M1: a tap's pages are
    // stamped against its own epoch, so a mask change stales the insert and
    // not the tap; re-wiring is what makes the tap re-read). Every hop below
    // is re-set for the same reason wireAsSendLane re-sets its own.
    cpAuxTap_->setInput( 0, insert ? insert->linkOutput( 0 ) : nullptr );
    auxInsert_ = insert;
    cpDspChain_->setInput( 0, cpAuxTap_->linkOutput( 0 ) );
    cpDspChain_->rebuildWiring();
    cpGainStage_->setInput( 0, cpDspChain_->linkOutput( 0 ) );
    // A return keeps its OWN rewire, which is what its summing parent reads
    // as its root component -- wireAsSendLane's shape, for 47 D1's reason.
    cpRewire_->setInput( 0, cpGainStage_->linkOutput( 0 ) );

    // ANNOUNCED ONCE per state (47 T6), never per pass: a solo toggle re-runs
    // the pass and must not repeat the line.
    const bool routed = (bool) insert;
    const QString state = routed ? QString() : why;
    if( state != returnState_ ) {
        if( !routed )
            TW_LOGW( "model", "[RETURN] '%s' is SILENT: %s (proposal 55 D8)",
                     getSName().toUtf8().constData(), why.toUtf8().constData() );
        else
            TW_LOGI( "model", "[RETURN] '%s' is routed again",
                     getSName().toUtf8().constData() );
        returnState_ = state;
    }
    return routed;
}

void STrack::unwireReturnTrack()
{
    if( !cpAuxTap_ ) return;
    // Back to the constructor's wiring, exactly unwireSendLane's.
    if( cpDspChain_ ) {
        cpDspChain_->setInput( 0, cpTrackMix_ ? cpTrackMix_->linkOutput( 0 ) : nullptr );
        cpDspChain_->rebuildWiring();
        if( cpGainStage_ ) cpGainStage_->setInput( 0, cpDspChain_->linkOutput( 0 ) );
        if( cpRewire_ && cpGainStage_ )
            cpRewire_->setInput( 0, cpGainStage_->linkOutput( 0 ) );
    }
    // The tap lets go of the source insert while auxInsert_ still keeps it
    // alive -- the order 47's unwireSendLane segfault taught.
    cpAuxTap_->setInput( 0, nullptr );
    cpAuxTap_.reset();
    auxInsert_.reset();
    returnState_.clear();
}

bool STrack::returnRouted() const
{
    if( !cpAuxTap_ || !cpDspChain_ || !auxInsert_ ) return false;
    if( twProducerOf( cpDspChain_->getInputPlug( 0 ) ).get() != cpAuxTap_.get() )
        return false;
    return twProducerOf( cpAuxTap_->getInputPlug( 0 ) ).get() == auxInsert_.get();
}

QString STrack::describeReturnTap() const
{
    return cpAuxTap_ ? QString::fromStdString( cpAuxTap_->describe() ) : QString();
}

std::uint64_t STrack::returnTapSilentRenders() const
{
    return cpAuxTap_ ? cpAuxTap_->silentRenders() : 0;
}

bool STrack::applyRoutedAuxMasks( const QHash<quint64, std::uint32_t> &masks )
{
    if( !cpPluginChain_ ) return false;
    bool changed = false;
    const int n = cpPluginChain_->getSlotCount();
    for( int i = 0; i < n; ++i ) {
        SPluginSlot *slot = cpPluginChain_->getSlotAt( i );
        if( !slot ) continue;
        const quint64 id = slot->slotId();
        if( slot->setRoutedOutBuses( id ? masks.value( id, 0u ) : 0u ) ) changed = true;
    }
    return changed;
}

void STrack::bumpRenderChainEpoch()
{
    // Every model change that reaches this track passes through here on the
    // MAIN thread, which is the one place the feed's source list can be rebuilt
    // safely (see refreshInstrumentFeed).
    refreshInstrumentFeed();
    refreshClipGainCurves();
    if( cpTrackMix_ )
        cpTrackMix_->bumpContentEpoch();
    if( cpDspChain_ )
        cpDspChain_->bumpContentEpoch();   // forwards to its inserts
    if( cpGainStage_ )
        cpGainStage_->bumpContentEpoch();
    if( cpRewire_ )
        cpRewire_->bumpContentEpoch();
    // A return's input (proposal 55 D3): its own page cache sits in front of
    // the chain and is stamped against its own epoch.
    if( cpAuxTap_ )
        cpAuxTap_->bumpContentEpoch();
}

// Range-scoped variant (proposal 18 Phase 5): every component in the chain
// speaks the same timeline positions, so one range fits all of them.
void STrack::bumpRenderChainEpochRange( offset_t start, offset_t end )
{
    refreshInstrumentFeed();
    refreshClipGainCurves();
    if( cpTrackMix_ )
        cpTrackMix_->invalidatePagesInRange(start, end);
    if( cpDspChain_ )
        cpDspChain_->invalidatePagesInRange(start, end);   // forwards to its inserts
    if( cpGainStage_ )
        cpGainStage_->invalidatePagesInRange(start, end);
    if( cpRewire_ )
        cpRewire_->invalidatePagesInRange(start, end);
    if( cpAuxTap_ )
        cpAuxTap_->invalidatePagesInRange(start, end);
}

// proposal 41 D11/M7: TOPMOST means Z-ORDER topmost -- latest start time,
// childIndex as the tiebreak (the same total order STrackRendererInline's
// paint loop sorts by) -- not "first match in child-list insertion order",
// which is what this used to return. The two agree whenever clips were
// added in time order (the common case, which is why this bug went
// unnoticed), and disagree the moment an earlier-inserted clip encloses a
// later-inserted, later-starting one: the old code handed back the
// ENCLOSING clip, even though the later one paints on top of it. A single
// pass keeps the best (startTime, childIndex) seen so far rather than
// returning on the first containing match.
SLink *STrack::getTopMostSLinkAt( offset_t queryTime ) const
{
    SLink *best = NULL;
    offset_t bestStart = 0;
    int bestIndex = -1;
    int idx = 0;
    for( SLink *lk : childLinks() ) {
        const int myIndex = idx++;
        // Skip child tracks — they have their own lanes; this query is for the
        // track's own clips.
        if( dynamic_cast<STrack*>( &lk->getSObject() ) ) continue;
        if( !lk->hasStartTime() ) continue;
        offset_t startTime = lk->getStartTime();
        if( queryTime<startTime ) continue;
        bool contains;
        if( lk->getSObject().hasDuration() ) {
            offset_t endTime = startTime+lk->getSObject().getDuration();
            contains = ( queryTime<endTime );
        } else {
            contains = true;
        }
        if( !contains ) continue;
        if( !best || startTime>bestStart
            || ( startTime==bestStart && myIndex>bestIndex ) ) {
            best = lk; bestStart = startTime; bestIndex = myIndex;
        }
    }
    return best;
}

void STrack::checkDurationChanged()
{
    length_t oldDuration = lastDuration_;
    length_t newDuration;
    newDuration = getDuration();
    qWarning( "STrack::checkDurationChanged() called. oldDuration = %d, newDuration = %d.\n",
              (int)oldDuration, (int)newDuration );
    qWarning( "STrack::checkDurationChanged(): newDuration = %d:%d.",
	      (int)(newDuration>>32), (int)newDuration );
    if( newDuration!=oldDuration ) {
        emit durationChanged( newDuration );
    }
}

void STrack::setLiveOwnedLane( bool owned )
{
    if( liveOwnedLane_ == owned ) return;
    liveOwnedLane_ = owned;
    if( owned ) return;

    // Handed back. ONE walk for everything the lane accumulated while the pump
    // owned it (design D7).
    if( !haveDeferredDirty_ ) return;
    const offset_t a = deferredDirtyStart_, b = deferredDirtyEnd_;
    haveDeferredDirty_ = false;
    deferredDirtyStart_ = deferredDirtyEnd_ = 0;
    invalidateRenderPathRange( a, b );
}

void STrack::invalidateRootWalkOrDefer( offset_t start, offset_t end )
{
    if( !liveOwnedLane_ ) {
        invalidateRenderPathRange( start, end );
        return;
    }
    if( !haveDeferredDirty_ ) {
        haveDeferredDirty_  = true;
        deferredDirtyStart_ = start;
        deferredDirtyEnd_   = end;
        return;
    }
    if( start < deferredDirtyStart_ ) deferredDirtyStart_ = start;
    if( end   > deferredDirtyEnd_ )   deferredDirtyEnd_   = end;
}

void STrack::trackChildDurationChanged( length_t newLength )
{
    // durationChanged is connected on the child's OBJECT (see
    // trackChildWasAdded), so sender() is the SObject — an SCut, say — not the
    // SLink. The old dynamic_cast<SLink*>(sender()) was therefore always null,
    // and the engine's clip window silently kept its stale duration (a split
    // clip's head kept sounding over its full pre-split span). Resolve every
    // link of ours that references the sender object and update each placement.
    SObject *obj = dynamic_cast<SObject *>( sender() );
    if( obj && obj->contentKind() == SContentKind::Event ) {
        twFrameRange affected;
        for( SLink *lk : childLinks() ) {
            if( !lk || &lk->getSObject() != obj ) continue;
            if( !eventClips_->hasClip( lk ) ) continue;
            affected.unite( eventClips_->updateClip( lk, lk->getStartTime(),
                                                     newLength ) );
        }
        invalidateRenderPathRange( (offset_t) affected.start, EVENT_DIRTY_END );
        lastDurationValid_ = false;
        checkDurationChanged();
        return;
    }
    if( obj && obj->isLiveRecording() ) {
        // A GROWING RECORDING never entered the bus mixers (see
        // trackChildWasAdded), so there is nothing to update and nothing to
        // invalidate: it is drawn, not rendered. Its length moves ten times a
        // second, which is exactly the traffic design D7 says must not reach
        // the root.
        lastDurationValid_ = false;
        checkDurationChanged();
        return;
    }
    if( obj ) {
        twEditRange affected;
        twFrameRange eventAffected;
        for( SLink *lk : childLinks() ) {
            if( !lk || &lk->getSObject() != obj ) continue;
            if( cpTrackMix_ ) {
                twEditRange r = cpTrackMix_->updateClip(
                    lk, lk->getStartTime(), newLength);
                affected.unite(r.start, r.end);
            }
            // PROPOSAL 41 D4/M3: a fragment placement is dual-inserted
            // (trackChildWasAdded) — keep its residual event window's
            // duration in step with its audio window's. A no-op for every
            // plain audio clip (never in eventClips_).
            if( eventClips_->hasClip( lk ) ) {
                eventAffected.unite( eventClips_->updateClip(
                    lk, lk->getStartTime(), newLength ) );
            }
        }
        // AFTER the engine mutation: stale this chain and every container up
        // to the root over EXACTLY the affected extent (union of the pre-
        // and post-edit clip windows, reported by the mix) — pages elsewhere
        // in the song survive (proposal 18 Phase 5).
        invalidateRootWalkOrDefer( (offset_t) affected.start,
                                   (offset_t) affected.end );
        if( !eventAffected.empty() )
            invalidateRenderPathRange( (offset_t) eventAffected.start, EVENT_DIRTY_END );
    }
    lastDurationValid_ = false;
    checkDurationChanged();
}

void STrack::trackChildWasMoved( offset_t newTime )
{
    SLink *slink = (SLink *) (const SLink *) sender();
    if( slink && slink->getSObject().hasDuration() ) {
        // Blocking read — a stale try-lock duration here would resize the clip
        // window wrongly via updateClip (same class as the insert-path fix).
        length_t duration = slink->getSObject().getDurationBlocking();
        // PROPOSAL 41 D4/M3: no early return — a fragment placement is
        // dual-inserted (trackChildWasAdded), and both its residual event
        // window and its audio sum must move together. For a plain event
        // clip (never in cpTrackMix_) the update below is a no-op.
        if( eventClips_->hasClip( slink ) ) {
            twFrameRange r = eventClips_->updateClip( slink, newTime, duration );
            invalidateRenderPathRange( (offset_t) r.start, EVENT_DIRTY_END );
        }
        twEditRange affected;
        if( cpTrackMix_ ) {
            twEditRange r = cpTrackMix_->updateClip(slink, newTime, duration);
            affected.unite(r.start, r.end);
        }
        // Union of the old and new placements (the mix knew the old one).
        invalidateRenderPathRange( (offset_t) affected.start,
                                   (offset_t) affected.end );
        lastDurationValid_ = false;
        checkDurationChanged();
    }
}

/**
 * We have a new child. Insert it into the clip list on all track mixers.
 */
void STrack::trackChildWasAdded( SLink &child )
{
    if( child.hasStartTime() ) {
        if( child.getSObject().hasDuration() ) {
            // If we have a new child, with a duration, attach a callback
            // to it, which informs us, if its starttime changes.
            QObject::connect( &child, SIGNAL( startTimeChanged( offset_t ) ),
                              this, SLOT( trackChildWasMoved( offset_t ) ) );
            QObject::connect( &(child.getSObject()), SIGNAL( durationChanged( length_t ) ),
                              this, SLOT( trackChildDurationChanged( length_t ) ) );

            // A LIVE RECORDING goes into NEITHER (proposal 21 L3b, design
            // D7). It has no root component, so inserting it as a clip entry
            // would cost a dummy freeze per page per clip and make
            // `twView::getComponent() returned nullptr` fire once per freeze
            // — the same argument the event route below makes, for the same
            // reason. What is heard while recording is the live monitor lane;
            // this clip is drawn only, and at stop it is replaced by the
            // WAV-backed cut place-recording builds.
            if( child.getSObject().isLiveRecording() ) {
                lastDurationValid_ = false;
                checkDurationChanged();
                return;
            }

            // EVENT material goes into the event clip set and NOT into the bus
            // mixers (design 3.2): a MIDI clip has no page to freeze, and
            // inserting one as a clip entry would cost a dummy freeze per page
            // per clip plus a twView warning per freeze (M5/F12).
            if( child.getSObject().contentKind() == SContentKind::Event ) {
                QObject::connect( &(child.getSObject()),
                                  SIGNAL( eventsChanged( offset_t ) ),
                                  this, SLOT( trackEventClipChanged( offset_t ) ) );
                SLink *key = &child;
                twFrameRange r = eventClips_->insertClip(
                    key, child.getStartTime(),
                    child.getSObject().getDurationBlocking(),
                    [key]( offset_t clipPos ) {
                        // Resolved ONCE per collect, at the window start - the
                        // same coherence rule twView::resolve gives the audio
                        // path (proposal 19 Inv-1).
                        return key->getSObject().resolveEventClip( clipPos );
                    } );
                invalidateRenderPathRange( (offset_t) r.start, EVENT_DIRTY_END );
                lastDurationValid_ = false;
                checkDurationChanged();
                return;
            }

            // Insert the clip into all track mixers with a callback that gets the component
            offset_t startTime = child.getStartTime();
            // BLOCKING read (proposal 19): getDuration()'s try-lock snapshot can
            // return a fresh cut's DEFAULT (0) when a revalidation worker holds
            // the cut mutex (the just-set duration scheduled a Preview job).
            // duration=0 inserts an UNBOUNDED clip that bleeds source past the
            // clip end (takes_recording_placement doubling). Edit path — may block.
            length_t duration = child.getSObject().getDurationBlocking();
            // Capture SLink by reference; callback will call getRootComponent() dynamically
            auto getComponentFn = [&child]() { return child.getRootComponent(); };
            // Inv-1: single resolver — component + slip-folded position from ONE
            // clip snapshot, so the freeze can't straddle a lazy reader build.
            auto resolveFn = [&child]( offset_t off ) {
                return child.getSObject().resolveClip( off );
            };
            twEditRange affected;
            if( cpTrackMix_ ) {
                twEditRange r = cpTrackMix_->insertClip(
                    &child, startTime, duration, getComponentFn, resolveFn);
                affected.unite(r.start, r.end);
            }

            // PROPOSAL 41 D4/M3: a DUAL insertion — a placement can carry
            // both an audio sum (above, cpTrackMix_) AND a RESIDUAL event
            // feed (a fragment placed here, D1) at the SAME time, keyed by
            // the SAME SLink*. resolveEventFeed() is the base-class virtual
            // that answers "nothing" for everything that existed before
            // proposal 41 — an ordinary sample cut, and (deliberately, D4) a
            // CONTAINER ASSET whose folder already has its own instrument —
            // so this check costs one virtual call and adds nothing to any
            // of those paths. Only a fragment with Event-kind children
            // answers non-empty, and eventClips_->insertClip gives it the
            // SAME window-gating / note-off-synthesis / loop machinery a
            // plain event clip already gets, one call site above.
            if( child.getSObject().resolveEventFeed( 0 ).seq ) {
                SLink *key = &child;
                twFrameRange r = eventClips_->insertClip(
                    key, startTime, duration,
                    [key]( offset_t clipPos ) {
                        twEventClipResolved r = key->getSObject().resolveEventFeed( clipPos );
                        // D6: the remap is per-PLACEMENT (the SLink), never
                        // per-content (see remapEventChannel's comment) -
                        // applied here, the one place both are in hand.
                        r.seq = remapEventChannel( r.seq, key->getEventChannelOverride() );
                        return r;
                    } );
                invalidateRenderPathRange( (offset_t) r.start, EVENT_DIRTY_END );
            }

            // We are the summing parent for a child TRACK (a folder lane), so
            // its audibility is ours to enforce — a nested track is not a mixer
            // child, so SStdMixer's null-the-input-plug path never sees it. Seed
            // the entry from the child's CURRENT state under the shared rule: a
            // track that is already muted (or soloed out) when it is reparented
            // in must land silent.
            if( STrack *childTrack = dynamic_cast<STrack*>( &child.getSObject() ) ) {
                QObject::connect( childTrack, SIGNAL( mutedChanged( bool ) ),
                                  this, SLOT( childTrackMuteChanged( bool ) ),
                                  Qt::UniqueConnection );
                // Solo is global: relay it up to the root mixer rather than
                // resolving it here (see childTrackSoloChanged). Both hops are
                // needed — the direct flag of this child, and anything a nested
                // folder below it forwards.
                QObject::connect( childTrack, SIGNAL( soloChanged( bool ) ),
                                  this, SLOT( childTrackSoloChanged() ),
                                  Qt::UniqueConnection );
                QObject::connect( childTrack, SIGNAL( subtreeSoloChanged() ),
                                  this, SLOT( childTrackSoloChanged() ),
                                  Qt::UniqueConnection );
                // The root that HOLDS us (QBX-245), eventFeed's reason.
                SObject *root =
                    splacements::rootHolding( getProjectSafe(), this );
                if( !ssolo::isLaneAudible( root, childTrack ) ) {
                    if( cpTrackMix_ ) {
                        twEditRange r = cpTrackMix_->setClipMuted( &child, true );
                        affected.unite(r.start, r.end);
                    }
                }
            }
            // Only the new clip's extent changed (proposal 18 Phase 5).
            invalidateRenderPathRange( (offset_t) affected.start,
                                       (offset_t) affected.end );

            lastDurationValid_ = false;
            checkDurationChanged();
        }
    }
}

void STrack::trackChildWasRemoved( SLink &child )
{
    if( child.hasStartTime() ) {
        if( child.getSObject().hasDuration() ) {
            if( child.getSObject().isLiveRecording() ) {
                // Never inserted; nothing to remove and nothing stale.
                lastDurationValid_ = false;
                checkDurationChanged();
                return;
            }
            // PROPOSAL 41 D4/M3: a fragment placement is DUAL-inserted (see
            // trackChildWasAdded), so both sets are checked — never an early
            // return the way the pure-event case used to take, or a
            // fragment's audio half would stay in cpTrackMix_ after removal.
            // For a PLAIN event clip (never in cpTrackMix_) and a PLAIN
            // audio clip (never in eventClips_) the other call is a no-op:
            // removeClip on a key that is not present matches nothing and
            // returns an empty range (twtrackmix.cc / twEventClipSet).
            if( eventClips_->hasClip( &child ) ) {
                twFrameRange r = eventClips_->removeClip( &child );
                invalidateRenderPathRange( (offset_t) r.start, EVENT_DIRTY_END );
            }
            // Remove the clip from all track mixers, keyed by the link itself.
            twEditRange affected;
            if( cpTrackMix_ ) {
                twEditRange r = cpTrackMix_->removeClip(&child);
                affected.unite(r.start, r.end);
            }
            // Only the removed clip's extent went silent (proposal 18 Phase 5).
            invalidateRenderPathRange( (offset_t) affected.start,
                                       (offset_t) affected.end );

            lastDurationValid_ = false;
            checkDurationChanged();
        }
    }
}

// PROPOSAL 36 B4: ONE twTrackMix + ONE twPluginChain, N CHANNELS WIDE.
//
// What this replaces is STrack::setNBusses(), which built N parallel width-1
// twTrackMix + twPluginChain pairs and could only ever GROW — a shrink was
// Q_ASSERT_X( false, ... ), i.e. an abort in a Debug build and a SILENT return
// in the RelWithDebInfo build everyone runs (§7 trap 9), leaving stale wiring.
// Growing had its own hazard: the new mixers had to be back-filled with every
// existing clip and re-wired to the slots' per-bus taps, and getting the
// "which ones are new" bookkeeping wrong duplicated clip entries.
//
// None of that exists any more. There is one component of each kind, its width
// is a number, and changing the number creates and destroys nothing:
//
//     twTrackMix(N) -> twPluginChain(N) -> twRewire(N)      [the track root]
//
// So the whole grow/shrink asymmetry is gone, and with it the reason
// project_channels_test had to prove M1 reached no bus count: the project's
// channels= now REACHES this, deliberately, and 6 -> 2 on an undo is ordinary.
//
// This is called from the constructor (with the project's width), from
// SProject::channelsChanged, and by nothing else.
void STrack::setChannels( int n )
{
    if( n < 1 ) n = 1;

    const bool firstTime = !cpTrackMix_;
    if( !firstTime && n == channels_ ) return;

    tw303aEnvironment *env = SAppContext::get().get303aEnvironment();

    if( firstTime ) {
        cpTrackMix_ = std::make_shared<twTrackMix>( *env );
        cpTrackMix_->init();

        cpDspChain_ = std::make_shared<twPluginChain>( *env, (idx_t) n );
        cpDspChain_->init();

        // THE FADER (proposal 37 P3a / D5), POST-FX and pre-rewire. It was a
        // scalar inside twTrackMix until this phase, i.e. ahead of the inserts.
        cpGainStage_ = std::make_shared<twGainStage>( *env );
        cpGainStage_->init();

        cpRewire_ = std::make_shared<twRewire>( *env );
        cpRewire_->init();
        // ONE plug: a wide rewire is single-plug (see twRewire's class note).
        cpRewire_->setNPlugs( 1 );

        // trackmix -> chain -> gain -> rewire
        cpDspChain_->setInput( 0, cpTrackMix_->linkOutput( 0 ) );
        cpDspChain_->rebuildWiring();
        cpGainStage_->setInput( 0, cpDspChain_->linkOutput( 0 ) );
        cpRewire_->setInput( 0, cpGainStage_->linkOutput( 0 ) );

        // A track built after its volume was already set (the loader sets the
        // attribute before setChannels() runs for a re-parented lane) must not
        // silently start at unity.
        cpGainStage_->setGainDb( getVolume() );
        cpGainStage_->setPan( getPan() );     // proposal 49 M2, same reason
    }

    channels_ = n;

    // Width is a property of each component, set independently — there is no
    // rewiring to do and nothing to back-fill, which is the entire point of the
    // collapse.
    cpTrackMix_->setChannels( (idx_t) n );
    cpDspChain_->setChannels( (idx_t) n );
    cpGainStage_->setChannels( (idx_t) n );
    cpRewire_->setChannels( (idx_t) n );

    // The slots have to re-derive the channel-mismatch mapping (proposal 08
    // §Layer 3): a 2->2 plugin is Direct on a stereo track and Unsupported on a
    // 6-channel one, and the verdict is only correct if it is taken against the
    // width the insert will actually be handed. setChannelCount()
    // re-instantiates, so it also re-applies bypass and the saved state chunk.
    if( cpPluginChain_ ) {
        const int nSlots = cpPluginChain_->getSlotCount();
        for( int s = 0; s < nSlots; ++s ) {
            if( SPluginSlot *slot = cpPluginChain_->getSlotAt( s ) )
                slot->setChannelCount( n );
        }
    }

    if( firstTime ) {
        // Initial sync: populate the mix with the clips we already carry. Only
        // reachable on the FIRST call — a later width change touches no clip
        // list at all, which is what retired the "which mixers are new"
        // bookkeeping that used to double-insert entries.
        for( SLink *lk : childLinks() ) {
            if( !lk || !lk->hasStartTime() ) continue;
            // Blocking read — same stale try-lock hazard as trackChildWasAdded.
            length_t duration = lk->getSObject().hasDuration()
                                ? lk->getSObject().getDurationBlocking() : 0;
            auto getComponentFn = [lk]() { return lk->getRootComponent(); };
            // Inv-1: single resolver — component + slip-folded position from ONE
            // clip snapshot, so the freeze can't straddle a lazy reader build.
            auto resolveFn = [lk]( offset_t off ) {
                return lk->getSObject().resolveClip( off );
            };
            cpTrackMix_->insertClip( lk, lk->getStartTime(), duration,
                                     getComponentFn, resolveFn );
            QObject::connect( lk, SIGNAL( startTimeChanged( offset_t ) ),
                              this, SLOT( trackChildWasMoved( offset_t ) ),
                              Qt::UniqueConnection );
            QObject::connect( &(lk->getSObject()), SIGNAL( durationChanged( length_t ) ),
                              this, SLOT( trackChildDurationChanged( length_t ) ),
                              Qt::UniqueConnection );
        }
    } else {
        // Every page in this track's chain is now the wrong shape. §4.5 already
        // treats an old-width page as a MISS, so this is not what makes the
        // change safe — it is what makes it converge instead of playing silence
        // until some other edit invalidates us.
        invalidateRenderPath();
        // A return's tap was built for the old width (55 D4: decided once);
        // the pass builds a new one.
        if( isReturnTrack() ) requestRoutingPass( this );
    }

    emit nChannelsChanged( n );
}

STrack::STrack( SProject *project )
    : SObject( project ),
      inlineRenderer_( 0 ),
      channels_( 0 ),
      cpPluginChain_( 0 ),
      lastDuration_( 1 ),
      lastDurationValid_( true )
{
    // Create the plugin chain model object (container for effect inserts)
    // NOTE: We do NOT call setParent(this) because the plugin chain is NOT an SLink.
    // SObject::childEvent() expects all children to be SLink instances; setting the
    // plugin chain as a Qt child would cause an invalid cast in childEvent().
    // Instead, we manage the chain's lifetime manually via the destructor.
    cpPluginChain_ = new SPluginChain( project );
    connectPluginChain( cpPluginChain_ );

    // The event twin of the bus mixers: ONE per track (events are not per bus).
    eventClips_ = std::make_shared<twEventClipSet>();

    // Add a listener for added child objects.
    // We want to become noticed, if it is new.
    QObject::connect( this, SIGNAL( childObjectAdded( SLink & ) ),
                      this, SLOT( trackChildWasAdded( SLink & ) ) );
    QObject::connect( this, SIGNAL( childObjectRemoved( SLink & ) ),
                      this, SLOT( trackChildWasRemoved( SLink & ) ) );

    // Forward track mute and volume changes to the track mixers
    QObject::connect( this, SIGNAL( mutedChanged( bool ) ),
                      this, SLOT( onTrackMuteChanged( bool ) ) );
    QObject::connect( this, SIGNAL( volumeChanged( double ) ),
                      this, SLOT( onTrackVolumeChanged( double ) ) );
    QObject::connect( this, SIGNAL( panChanged( double ) ),
                      this, SLOT( onTrackPanChanged( double ) ) );

    // WIDTH COMES FROM THE PROJECT (proposal 36 B4), and follows it. There is
    // exactly one authority — SProject::channels(), the attribute M1 added —
    // and this is where it reaches the graph for the first time. The fallback
    // is 2 for a track built without a project, which is the width every
    // project this build has ever written carries.
    setChannels( project ? project->channels() : 2 );
    if( project ) {
        QObject::connect( project, SIGNAL( channelsChanged( int ) ),
                          this, SLOT( setChannels( int ) ),
                          Qt::UniqueConnection );
    }
}

// Signals + the reference + the component provider, in one place so the
// constructor path and the adoption path cannot drift apart (a chain wired by
// only one of them is a chain whose slots never reach the DSP).
void STrack::connectPluginChain( SPluginChain *chain )
{
    if( !chain ) return;

    QObject::connect( chain, SIGNAL( slotInserted( int, SPluginSlot & ) ),
                      this, SLOT( onPluginSlotInserted( int, SPluginSlot & ) ) );
    QObject::connect( chain, SIGNAL( slotRemoved( int, SPluginSlot & ) ),
                      this, SLOT( onPluginSlotRemoved( int, SPluginSlot & ) ) );
    QObject::connect( chain, SIGNAL( slotsReordered( int, int ) ),
                      this, SLOT( onPluginSlotsReordered( int, int ) ) );

    // The track's DSP chain answers SPluginChain::getRootComponent(). Captured
    // by `this` and read lazily, so it follows a later rebuild.
    chain->setComponentProvider( [this]() -> std::shared_ptr<twComponent> {
        if( !cpDspChain_ ) return nullptr;
        return std::static_pointer_cast<twComponent>( cpDspChain_ );
    } );

    // See the member's declaration: without a reference of our own, an adopted
    // chain dies with the loader's temporary handle link.
    cpPluginChainRef_ = new SLink( *chain, nullptr );
}

// Proposal 08 M4: take over a chain that came out of a project file.
void STrack::adoptPluginChain( SPluginChain *chain )
{
    if( !chain || chain == cpPluginChain_ ) return;

    SPluginChain *old = cpPluginChain_;
    if( old ) {
        QObject::disconnect( old, nullptr, this, nullptr );
        old->setComponentProvider( nullptr );
    }
    // Dropping our reference is what retires the constructor's empty chain: its
    // refcount reaches zero and SObject::removeRef() posts the deleteLater. It
    // must go BEFORE the new link is made, or a save between the two would write
    // both chains and the track would name only one.
    delete cpPluginChainRef_;
    cpPluginChainRef_ = nullptr;

    cpPluginChain_ = chain;
    connectPluginChain( chain );

    // The loaded slots have never been near the DSP: their SLinks were parented
    // to the chain while it had no owner, so slotInserted was emitted into
    // nothing. Do here exactly what onPluginSlotInserted does, for every slot in
    // order — bus count FIRST (it selects the channel-mismatch mapping), then one
    // tap per bus appended to that bus's twPluginChain in slot order.
    const int nSlots = chain->getSlotCount();
    for( int s = 0; s < nSlots; ++s ) {
        SPluginSlot *slot = chain->getSlotAt( s );
        if( !slot ) continue;
        // The loaded slots never emitted slotInserted at anyone, so the
        // audioInvalidated wiring onPluginSlotInserted normally makes has to be
        // made here too — otherwise a bypass or parameter edit on a LOADED
        // project would be inaudible while the same edit on a freshly inserted
        // plugin worked.
        QObject::connect( slot, SIGNAL( audioInvalidated() ),
                          this, SLOT( onPluginSlotAudioInvalidated() ),
                          Qt::UniqueConnection );
        QObject::connect( slot, SIGNAL( audioInvalidatedRange( qint64, qint64 ) ),
                          this, SLOT( onPluginSlotAudioInvalidatedRange( qint64, qint64 ) ),
                          Qt::UniqueConnection );
        slot->setChannelCount( channels_ );
    }
    if( cpDspChain_ ) {
        for( int s = 0; s < nSlots; ++s ) {
            SPluginSlot *slot = chain->getSlotAt( s );
            if( !slot ) continue;
            if( auto insert = slot->getInsert() )
                cpDspChain_->addPlugin( insert );
        }
    }
    // Slot 0 may be the instrument — wire the track's event feed into it.
    // This is the one thing the "do here exactly what onPluginSlotInserted
    // does" loop above forgot: a project LOADED from disk goes through this
    // adopt path, never onPluginSlotInserted, so without this call an
    // instrument's SPluginSlot::setEventSource() is never invoked. Its
    // default is null, so every previously-recorded event clip on the track
    // is silent under ordinary playback/render forever after — while live
    // playing still works, because that goes through the independent
    // twLiveEventSource path (setLiveEventSource), not through eventFeed().
    syncInstrumentSlot();
    invalidateRenderPath();
}

QList<SLink *> STrack::ownedRefLinks() const
{
    // See the declaration: the chain reference is an owned link that is NOT a
    // child link, so it has to be published here to be part of the reference
    // graph at all.
    QList<SLink *> out;
    if( cpPluginChainRef_ ) out.append( cpPluginChainRef_ );
    return out;
}

STrack::~STrack()
{
    // FIRST, and explicitly: SFeelFlowTrackBounce's own destructor joins any
    // in-flight bounce render (see sfeelflowbounce.h/.cpp), which is only
    // safe to do while this track's SProject and CaptureRevalidator are
    // still alive — true here, because ~SProject() deletes its tracks from
    // its own destructor BODY, before its revalidator_ member is torn down.
    // Letting this destruct as an ordinary member (after cpRewire_ etc. are
    // already reset below) would risk the completion callback reading a
    // half-torn-down track.
    feelFlowBounce_.reset();

    DTOR_DEL( inlineRenderer_ );
    // Our reference to the plugin chain (see the member's declaration). The chain
    // object itself is a Qt child of the project and is destroyed with it.
    delete cpPluginChainRef_;
    cpPluginChainRef_ = nullptr;
    // NOTE: cpPluginChain_ is a Qt child of the project, so it will be deleted
    // automatically by Qt's parent-child cleanup. Do NOT manually delete it here
    // to avoid double-delete crashes during project destruction.
    // (Historically it was manually managed, but current design makes it a project child.)

    cpDspChain_.reset();
    cpTrackMix_.reset();
    cpGainStage_.reset();
    cpRewire_.reset();
}

#if 0
int SEndTimeList::compareItems( QCollection::Item item1, QCollection::Item item2 )
{    
    SLink *so1 = (SLink *)item1;
    SLink *so2 = (SLink *)item2;    
    offset_t endTime1 = so1->getStartTime() + so1->getSObject().getDuration();
    offset_t endTime2 = so2->getStartTime() + so2->getSObject().getDuration();
    if( endTime1==endTime2 ) return 0;
    if( endTime1<endTime2 ) return -1;
    return 1;
}

int SStartTimeList::compareItems( QCollection::Item item1, QCollection::Item item2 )
{    
    SLink *so1 = (SLink *)item1;
    SLink *so2 = (SLink *)item2;    
    offset_t startTime1 = so1->getStartTime();
    offset_t startTime2 = so2->getStartTime();
    if( startTime1==startTime2 ) return 0;
    if( startTime1<startTime2 ) return -1;
    return 1;
}
#endif

SEndTimeList::SEndTimeList()
{
}

SEndTimeList::~SEndTimeList()
{
}

SStartTimeList::SStartTimeList()
{
}

SStartTimeList::~SStartTimeList()
{
}

// An event clip of ours changed its notes, its window, or was re-mapped by a
// tempo edit. The clip set caches nothing about the content - it resolves per
// collect - so all this owes is the epoch bump: touchClip reports the extent,
// and the walk to the root stales it from there on (F13: the app-side walk is
// the ONLY path that carries a change from a clip up to the root).
void STrack::trackEventClipChanged( offset_t fromClipPos )
{
    SObject *obj = dynamic_cast<SObject *>( sender() );
    if( !obj ) return;
    twFrameRange affected;
    for( SLink *lk : childLinks() ) {
        if( !lk || &lk->getSObject() != obj ) continue;
        if( !eventClips_->hasClip( lk ) ) continue;
        affected.unite( eventClips_->touchClip( lk ) );
        affected.unite( lk->getStartTime() + fromClipPos,
                        lk->getStartTime() + fromClipPos + 1 );
    }
    if( affected.empty() ) return;
    invalidateRenderPathRange( (offset_t) affected.start, EVENT_DIRTY_END );
}

int STrack::readPreChildrenAttributes( QDomElement &element )
{
    SObject::readPreChildrenAttributes( element );

    // nBusses= IS READ AND IGNORED (proposal 36 B4).
    //
    // It used to decide how many parallel width-1 mixers the track built, and
    // it was a second authority on a number the project already owns. B4
    // retired the concept: a track is ONE twTrackMix and ONE twPluginChain, N
    // channels wide, and N is SProject::channels(). Every .qxp ever written by
    // this build carries nBusses='2' and every one of them still loads — the
    // attribute simply no longer says anything, and the writer no longer emits
    // it (see serializeSelfAttributes).
    //
    // Warning once when a file disagrees with the project's width is worth it:
    // it is the only signal a user would get that an old document meant
    // something different, and it costs one line per track on a file that will
    // not carry the attribute again after the next save.
    const QString data = element.attribute( "nBusses" );
    if( !data.isEmpty() ) {
        bool ok = false;
        const int wanted = data.toInt( &ok );
        if( ok && wanted != channels_ ) {
            qWarning() << "STrack: ignoring legacy nBusses=" << wanted
                       << "- channel width comes from the project ("
                       << channels_ << ") since proposal 36 B4.";
        }
    }

    
    // Absent = auto, which is what every project written before proposal 37
    // means and what a track without a serialized routing should do.
    midiRouting_ = midiRoutingFromString(
        element.attribute( "midiRouting", "auto" ) );

    // MIDI output (P7b). Absent = no output, which is what every project
    // written before proposal 37 means.
    setMidiOutput( element.attribute( "midiOutPort", "" ),
                   element.attribute( "midiOutChannel", "-1" ).toInt(),
                   element.attribute( "midiOutOffsetMs", "0" ).toInt() );

    // Live input / monitoring (L1b). Absent = no input, monitor Auto, which is
    // what every project written before proposal 21 means. ArmedForRecording
    // is read by SObject above and is INERT on load: SLiveMonitor records the
    // set of tracks that arrived armed and refuses to monitor them until the
    // user arms them in this session (design D9, "never starts monitoring on
    // load") - a loaded project must not open the developer's microphone.
    setTrackInput( element.attribute( "trackInput", "" ) );
    setMonitorMode( monitorModeFromString(
        element.attribute( "monitorMode", "auto" ) ) );
    // Absent = inherit. An UNKNOWN value is refused back to inherit rather
    // than carried: a typo would otherwise silently pin a lane to a mode
    // nothing can name.
    {
        const QString rm = element.attribute( "recordMode", "" );
        setRecordMode( SProjectProps::isRecordMode( rm ) ? rm : QString() );
    }

    // Fold state (fix/track-list-polish m). Absent = expanded, which is what
    // every project written before this attribute existed means.
    setCollapsed( element.attribute( "collapsed", "false" ).startsWith( "true" ) );

    // The other three pieces of per-track VIEW state (proposal 46 M3). Absent
    // means the default in every case, which is what every project written
    // before these attributes existed says. setLaneHeightScale() clamps, so a
    // hand-edited file cannot produce a lane six pixels tall or one that fills
    // the arranger; decodeShownAutomation() skips an entry it cannot parse.
    {
        bool scaleOk = false;
        const double scale =
            element.attribute( "laneHeightScale", "1.0" ).toDouble( &scaleOk );
        if( scaleOk ) setLaneHeightScale( scale );
    }
    setTakesExpanded(
        element.attribute( "takesExpanded", "false" ).startsWith( "true" ) );
    setMixerStripNarrow(
        element.attribute( "mixerStripNarrow", "false" ).startsWith( "true" ) );
    setShownAutomationLanes(
        decodeShownAutomation( element.attribute( "shownAutomation" ) ) );

    // The SYSTEM ROLE (proposal 45 D1). Absent = None = an ordinary track,
    // which is what every project written before system lanes means. An
    // UNKNOWN spelling also reads as None, deliberately: a file naming a role
    // this build does not have describes a track the user can still see, move
    // and delete, rather than an untouchable lane nothing understands.
    {
        const QString roleText = element.attribute( "systemRole" );
        bool roleOk = false;
        const SSystemRole role = systemRoleFromString( roleText, &roleOk );
        if( !roleOk )
            qWarning() << "STrack: unknown systemRole=" << roleText
                       << "- loading as an ordinary track.";
        setSystemRole( role );
    }

    return 0;
}

SLink *STrack::instantiateFromDomElement(
    SProjectLoader &projectLoader, QDomElement &element, SObject *parent )
{
    (void) parent;
    SLink *contentLink = NULL;
    // Find the first link child
    QDomNode childNode = element.firstChild();
    STrack *track = new STrack( &projectLoader.getProject() );
    track->readPreChildrenAttributes( element );
    while( !childNode.isNull() ) {
        if( childNode.isElement() ) {
            qWarning() << "found STrack child " << childNode.nodeName() << Qt::endl;
            if( childNode.nodeName() == "SLink" ) {
                QDomElement childElement = childNode.toElement();
                QString objectId = childElement.attribute( "objectId" );
                // Look up the object id.
                contentLink = projectLoader.getObjectDictionary().value( objectId );
                if( contentLink ) {
                    // FIXME: Check, wether this is a track, or create a generic insertion function.
                    SLink *sl = new SLink( contentLink->getSObject(), NULL );
                    if( sl ) {
                        sl->readAttributes( childElement );
                        sl->setParent(track); // was: track->insertChild( sl );
                    } else {
                        qWarning() << "Failed to create SLink for object" << objectId;
                    }
                } else {
                    qWarning() << "Object not found in dictionary:" << objectId;
                }
            }
        }
        childNode = childNode.nextSibling();
    }
    track->readPostChildrenAttributes( element );

    // THE AUX SOURCE (proposal 55 D8), DEFERRED for pluginChainId's reason: the
    // source track's element may not have been instantiated yet. The slot is
    // an id, not an object, so it needs no lookup -- the pass finds it in the
    // source's chain. A track id that does not resolve FAILS CLOSED: the
    // track stays a return (it keeps the bus) with no source, and the pass
    // announces it silent. It is never re-attached by a later object.
    const QDomElement auxEl = element.firstChildElement( "auxSource" );
    if( !auxEl.isNull() ) {
        const int bus = auxEl.attribute( "bus", "0" ).toInt();
        if( bus > 0 ) {
            track->auxBus_    = bus;
            track->auxSlotId_ = auxEl.attribute( "slot", "0" ).toULongLong();
            const QString srcId = auxEl.attribute( "track" );
            SProjectLoader *loader = &projectLoader;
            projectLoader.deferResolve( [loader, track, srcId]() {
                SLink *lk = loader->getObjectDictionary().value( srcId );
                STrack *src = lk ? dynamic_cast<STrack *>( &lk->getSObject() ) : nullptr;
                if( !src ) {
                    TW_LOGW( "model", "[RETURN] '%s': its aux source track %s is not "
                             "in the project; the return stays silent",
                             track->getSName().toUtf8().constData(),
                             srcId.toUtf8().constData() );
                    return;
                }
                track->auxTrack_ = src;
            } );
        }
    }

    // Adopt our serialized plugin chain — DEFERRED (proposal 08 M4).
    //
    // It cannot happen here, and it cannot happen in readPostChildrenAttributes
    // either: pluginChainId is a plain attribute, so the loader's dependency
    // ordering (which only looks at <SLink objectId> children) gives no guarantee
    // that the <SPluginChain> element has been instantiated by the time this
    // track is built — with the chain listed after the track in the file, it
    // certainly has not. deferResolve runs the lookup at the end of
    // createObjects(), when the dictionary is complete.
    const QString chainId = element.attribute( "pluginChainId" );
    if( !chainId.isEmpty() ) {
        SProjectLoader *loader = &projectLoader;
        projectLoader.deferResolve( [loader, track, chainId]() {
            SLink *chainLink = loader->getObjectDictionary().value( chainId );
            if( !chainLink ) {
                qWarning() << "STrack: plugin chain" << chainId
                           << "not found in the project; keeping the empty one";
                return;
            }
            SPluginChain *chain =
                dynamic_cast<SPluginChain *>( &chainLink->getSObject() );
            if( !chain ) {
                qWarning() << "STrack: object" << chainId << "is not an SPluginChain";
                return;
            }
            track->adoptPluginChain( chain );
        } );
    }

    return new SLink( *track );
}

void STrack::onPluginSlotInserted( int index, SPluginSlot &slot )
{
    // BEFORE the bus guard, and UniqueConnection because a track whose bus count
    // grows re-runs this path: the slot has no way to invalidate the pages above
    // itself (proposal 08 M5 — see SPluginSlot::audioInvalidated()), so this
    // connection is what makes a bypass toggle and a parameter edit audible.
    QObject::connect( &slot, SIGNAL( audioInvalidated() ),
                      this, SLOT( onPluginSlotAudioInvalidated() ),
                      Qt::UniqueConnection );
    QObject::connect( &slot, SIGNAL( audioInvalidatedRange( qint64, qint64 ) ),
                      this, SLOT( onPluginSlotAudioInvalidatedRange( qint64, qint64 ) ),
                      Qt::UniqueConnection );

    // Sync the model change to all DSP plugin chains
    // Pre-allocate inserts for all buses to ensure they're fully initialized
    // before the audio thread accesses them
    if( channels_ > 0 ) {
        // Tell the slot the CHANNEL count FIRST: it is what selects the
        // channel-mismatch mapping (proposal 08 §Layer 3, re-derived from page
        // width by proposal 36 B4), and the insert must not be built against
        // the wrong one.
        slot.setChannelCount( channels_ );

        std::shared_ptr<audio::twPluginInsert> insert = slot.getInsert();
        if( !insert ) {
            // Insert creation failed - the slot will handle the error
            return;
        }
        if( cpDspChain_ ) {
            cpDspChain_->addPlugin( insert );
        }
        // Slot 0 may just have become (or stopped being) the instrument.
        syncInstrumentSlot();
        // The project end moves with the instrument's tail (design 3.1).
        lastDurationValid_ = false;
        invalidateRenderPath();
        // Proposal 55 T5: an undo of remove-plugin brings a slot back under
        // the id a return names; the pass re-finds it.
        if( auxReferenced_ ) requestRoutingPass( this );
    }
}

void STrack::onPluginSlotRemoved( int index, SPluginSlot &slot )
{
    // Remove by IDENTITY, never by index. `index` is a position in the model's
    // childOrder_; twPluginChain::plugins_ is a separate vector, and the two
    // agree only as long as every structural change is mirrored into both. When
    // they disagreed, removePlugin(index) erased a DIFFERENT insert than the one
    // being deleted: the model dropped the right slot, the audio path dropped
    // the wrong one, and nothing reported it. Identity cannot miss.
    //
    // peekInsert (not getInsert) because this is a teardown path — it must
    // never INSTANTIATE a plugin just to look up what to erase.
    (void)index;
    if( cpDspChain_ ) {
        cpDspChain_->removePlugin( slot.peekInsert() );
    }
    // Take the feed away from the slot that is leaving, and hand it to whatever
    // slot 0 is now. A departing instrument must not keep a live source.
    slot.setEventSource( nullptr );
    syncInstrumentSlot();
    lastDurationValid_ = false;
    invalidateRenderPath();
    // Proposal 55: a return of the departing slot must go silent NOW, not at
    // the next solo toggle; its tap keeps the insert alive until then.
    if( auxReferenced_ ) requestRoutingPass( this );
}

void STrack::onPluginSlotAudioInvalidated()
{
    // A parameter edit can move tailFrames() (the 303's Decay knob does), and
    // with it the project end of an instrument track.
    lastDurationValid_ = false;
    // Exactly what the insert/remove/reorder handlers do: bumpRenderChainEpoch()
    // on us (the twTrackMix + twPluginChain + the rewire) and on every
    // container above us, via the root's walk.
    invalidateRenderPath();
}

void STrack::onPluginSlotsReordered( int fromIndex, int toIndex )
{
    // Make the SAME move in plugins_, do not merely re-wire. rebuildWiring()
    // alone wires plugins_ in ITS existing order, so the reorder was inaudible
    // AND it left plugins_ permanently out of step with the model — which is
    // what made a later removal target the wrong insert.
    if( cpDspChain_ ) {
        cpDspChain_->reorderPlugin( fromIndex, toIndex );
    }
    syncInstrumentSlot();
    invalidateRenderPath();
    // Proposal 55: a return names its slot by id, so a reorder moves nothing
    // it reads -- but the pass is cheap and keeps "all routing wiring happens
    // in the pass" literally true (inv. 6).
    if( auxReferenced_ ) requestRoutingPass( this );
}

// Mute belongs to the mixer CHANNEL, not to the track: it is enforced by
// whoever sums this track, never inside our own output. So this deliberately
// does NOT touch our own twTrackMixes — our rendered pages stay valid and keep
// carrying our material, which is what lets an asset window a muted track
// (SCut::buildCapture_ freezes our component directly; nobody sums it there).
//
// The two summing parents both react on their own:
//   - SStdMixer   -> trackMuteSoloChanged() -> reconnectTracksToMixer(), which
//                    nulls our input plug (and already did this for solo).
//   - STrack      -> childTrackMuteChanged() below, which mutes our clip entry.
//
// A SYSTEM LANE IS THE ONE LANE WITH NO SUMMING PARENT, so neither of those two
// reacts for it and its mute would be a flag nothing reads -- exactly the
// SStdMixer::volume_ defect D6 refuses solo for. Proposal 45 AC5.4 says mute on
// the master is legitimate, and the mechanism it names is the one already in the
// tree: twGainStage's AUDIO mute, ramped over ~1.5 ms, which P3a built and left
// unwired for everything except P5's `self:Muted` lane. The master lane's gain
// stage sits between its insert chain and the mixer's rewire (D3), so muting it
// silences the whole mix and nothing else -- and it composes with a `self:Muted`
// lane rather than fighting it: setMuteCurve() governs where a curve exists,
// setMuted() where one does not.
//
// USER TRACKS ARE UNTOUCHED, deliberately. Their mute stays STRUCTURAL (the
// parent nulls the plug), which is what keeps a muted track's own capture full
// of its material -- an asset windowing a muted track must not be silence.
void STrack::onTrackMuteChanged( bool muted )
{
    if( systemRole() == SSystemRole::None || !cpGainStage_ ) return;
    cpGainStage_->setMuted( muted );
    // THE SAME INVALIDATION THE FADER USES, and for the same reason: the mute
    // is baked into every frozen page from the gain stage downstream, so
    // without it the mix keeps being served from what was frozen before the
    // button was pressed. invalidateRenderPath() is what onTrackVolumeChanged
    // calls one screen below, and for the master lane it reaches the mixer
    // through renderPathOwner_ (D11) -- a plain bumpRenderChainEpoch() on this
    // track is NOT a substitute and was measured not to be: the render came
    // back byte-identical to the unmuted one.
    invalidateRenderPath();
}

// A child TRACK of ours (a folder lane) changed its mute. We are the summing
// parent here, so we enforce it — the root mixer's null-the-input-plug trick
// cannot reach a nested track. Mute is per-lane (it changes nobody else's
// audibility), so re-applying our own children is enough.
void STrack::childTrackMuteChanged( bool /*muted*/ )
{
    // Not the top-down pass, so there is no root being handed down here: a
    // mute gesture on one child starts and ends with us, and the root has to
    // be looked up from our own identity (QBX-245).
    applyChildTrackAudibility( splacements::rootHolding( getProjectSafe(), this ) );
}

// A lane at or below one of our child tracks changed its solo flag. Solo is
// GLOBAL — it re-decides the audibility of every lane in the project, including
// our siblings and our parent's siblings — so this must not be resolved here.
// Relay it up; the root mixer answers with one whole-tree re-application
// (SStdMixer::applyAudibility), which calls back into
// applyChildTrackAudibility() on us.
void STrack::childTrackSoloChanged()
{
    emit subtreeSoloChanged();
}

// Enforce the shared audibility rule on our child TRACKS (folder lanes), and
// recurse. A lane's clip entry is muted exactly when the lane is not audible;
// twTrackMix::setClipMuted no-ops (and reports an empty range) when the flag is
// already right, so this is idempotent and costs nothing when nothing changed.
void STrack::applyChildTrackAudibility( SObject *root )
{
    // THE ROOT ARRIVES AS A PARAMETER (QBX-245). This is the hot top-down
    // pass -- SStdMixer::applyAudibility drives it on every mute and every
    // solo change, and it recurses through every folder -- and the driver is
    // the root itself, so there is nothing to look up. Resolving it per level
    // instead would be a whole-tree search per folder, and resolving it as
    // the MASTER (what this did) answered about a tree an arrangement's lanes
    // are not in: `anySoloInForce` read the wrong tree in per-root scope and
    // `underSoloedAncestor` never found their real folder ancestors.
    const bool anySolo = ssolo::anySoloInForce( root );

    // QBX-256. In Global scope a lane can be audible for ONE reason only --
    // that it carries the soloed lane's arrangement -- and then the clips it
    // holds of its own are not what the user soloed. Computed once per lane,
    // and false whenever solo is not in force or the scope is per-root, so
    // the clip loop below is then a no-op on a flag that is already right.
    //
    // THE THIRD TERM IS NOT DECORATION, and return_track_routing.qxa is what
    // named it: a lane that is INAUDIBLE must keep its clip entries as they
    // were. Its plug is nulled anyway, so its own clips change nothing it
    // sums -- but a ROUTING dependent still taps it (a return reads its
    // source's aux bus, which is an edge no containment walk follows), and
    // muting the source's clips starved the return. Measured: the solo-the-
    // return render came back SILENT where it must equal the return alone.
    // So this is for a lane that is audible AND audible for one reason only.
    const bool leakGuard =
        anySolo
        && ssolo::scopeOf( this ) == SProject::SoloScope::Global
        && ssolo::isLaneAudible( root, this, anySolo )
        && !ssolo::ownMaterialSoloed( this );

    twEditRange affected;
    for( SLink *lk : childLinks() ) {
        if( !lk ) continue;
        STrack *child = dynamic_cast<STrack*>( &lk->getSObject() );
        if( !child ) {
            // A CLIP entry. This pass is the only writer of a clip entry's
            // mute flag (twTrackMix::setClipMuted has no other caller), so
            // owning the plain clips here as well costs nothing and keeps one
            // owner: outside the leak case every clip is handed `false`,
            // which setClipMuted reports as an empty range.
            if( cpTrackMix_ ) {
                const bool clipAudible =
                    !leakGuard || ssolo::clipReachesSoloedLane( &lk->getSObject() );
                twEditRange r = cpTrackMix_->setClipMuted( lk, !clipAudible );
                affected.unite( r.start, r.end );
            }
            continue;
        }
        // The second, separate term (proposal 21 L1b, design D3): a NESTED
        // member of the live closure is excluded from the frozen sum through
        // its parent's twTrackMix, exactly the way an inaudible lane is - and
        // for the same reason the mixer nulls a top-level member's plug. It is
        // NOT a mute: the child keeps feeding events upward and keeps metering.
        const bool audible = ssolo::isLaneAudible( root, child, anySolo )
                             && !child->isLiveOwnedLane();
        {
            if( cpTrackMix_ ) {
                twEditRange r = cpTrackMix_->setClipMuted( lk, !audible );
                affected.unite( r.start, r.end );
            }
        }
        child->applyChildTrackAudibility( root );   // nested folders, same root
    }

    // Only the lanes that actually flipped gained or lost material — and the
    // chain above us has to be staled the same way, or the summed pages we
    // already froze keep serving the pre-edit mix (AC6 / the mute precedent).
    if( !affected.empty() ) {
        // A muted lane stops contributing EVENTS as well as audio (3.2.1), and
        // an instrument is a class-1 consumer - a change at `a` can be heard at
        // any later position - so the range is open-ended when we hold one.
        const offset_t end = instrumentSlot() ? EVENT_DIRTY_END
                                              : (offset_t) affected.end;
        invalidateRenderPathRange( (offset_t) affected.start, end );
    }
}

// --- the folder-sum preview (proposal 39 M3, design D3) ---------------------
//
// Everything below runs on the MAIN thread from paintEvent (and from the
// assert-envelope verb, which is the same call). It must never block, freeze,
// demand a page or wait: every probe it touches is an index into an array a
// child's own preview already built, which is the whole reason the exact
// answer (STrack::getPreview() over this folder, post-fader and behind
// requestPage()) is not used. See the header for the three rules.

bool STrack::hasChildTracks() const
{
    for( SLink *lk : childLinks() ) {
        if( !lk ) continue;
        if( dynamic_cast<STrack *>( &lk->getSObject() ) ) return true;
    }
    return false;
}

namespace {

// One walk's running state. The min/max accumulators are int32 and are clamped
// ONCE, by the caller, at the end: accumulating in preview_t (a signed char)
// wraps, and a wrap makes two loud children draw QUIETER than one child.
struct SChildSumWalk
{
    const SEnvelopeWindow &win;
    std::vector<int32_t>  &sumMin;
    std::vector<int32_t>  &sumMax;
    SObject               *root;      // for ssolo; may be null (plain mute)
    bool                   anySolo;
    bool                   any;       // did anything non-zero contribute?
};

double sChildSumLinearGain( double db )
{
    return pow( 10.0, db / 20.0 );
}

// Add ONE clip's drawn envelope, scaled by the accumulated lane gain.
//
// The clip gets ITS OWN PIXEL SPAN, exactly as STrackRendererInline's clip loop
// sizes the rect it draws that clip's waveform into. Handing a clip the whole
// lane window instead would be silently wrong rather than merely imprecise: a
// cut's collect clamps a negative clip-relative position to 0 (see
// cutSourceTimeOf), so a clip starting after the window's left edge would smear
// its audio across every column instead of occupying the ones it covers.
void sChildSumAddClip( SChildSumWalk &w, SLink &lk, double gain )
{
    SObject &obj = lk.getSObject();
    if( !lk.hasStartTime() ) return;
    if( !obj.hasDuration() ) return;

    // Through the RENDERER, never the concrete type (main/timeline/CONTRACT.md
    // inv. 2). An object with no waveform - an event clip - declines and
    // contributes nothing, which is the right answer rather than a bug.
    SObjectRenderer *rndr = obj.getInlineRenderer();
    if( !rndr ) return;

    const double t0 = (double) w.win.leftTime;
    const double t1 = (double) w.win.rightTime;
    if( t1 <= t0 ) return;
    const double wpx = (double) w.win.width;
    if( wpx < 1.0 ) return;

    const double st = (double) lk.getStartTime();
    const double en = st + (double) obj.getDuration();
    if( st >= t1 || en <= t0 ) return;                 // outside the window

    int isx = (int) ( ( st - t0 ) * wpx / ( t1 - t0 ) );
    int iex = (int) ( ( en - t0 ) * wpx / ( t1 - t0 ) );
    if( isx < 0 ) isx = 0;
    if( iex > w.win.width ) iex = w.win.width;
    if( iex <= isx ) return;

    SEnvelopeWindow sub;
    sub.leftTime  = (offset_t) ( t0 + (double) isx * ( t1 - t0 ) / wpx );
    sub.rightTime = (offset_t) ( t0 + (double) iex * ( t1 - t0 ) / wpx );
    sub.width     = iex - isx;

    std::vector<preview_t> pv( (size_t) sub.width, preview_t{ 0, 0 } );
    if( !rndr->collectEnvelope( lk, sub, pv.data() ) ) return;

    for( int i = 0; i < sub.width; ++i ) {
        if( !pv[i].min && !pv[i].max ) continue;
        w.sumMin[isx + i] += (int32_t) llround( (double) pv[i].min * gain );
        w.sumMax[isx + i] += (int32_t) llround( (double) pv[i].max * gain );
        w.any = true;
    }
}

// Add `track`'s own clips at `gain`, then recurse into its child tracks with
// their own faders folded in. The FOLDER being drawn is never passed here with
// its own gain applied - the top-level caller starts each child at that child's
// gain alone, which is what "up to but EXCLUDING the folder" means.
void sChildSumAddTrack( SChildSumWalk &w, STrack &track, double gain )
{
    for( SLink *lk : track.childLinks() ) {
        if( !lk ) continue;
        STrack *child = dynamic_cast<STrack *>( &lk->getSObject() );
        if( child ) {
            // THE shared audibility rule, never a local mute/solo chain
            // (main/timeline/CONTRACT.md inv. 10).
            if( !ssolo::isLaneAudible( w.root, child, w.anySolo ) ) continue;
            sChildSumAddTrack( w, *child,
                               gain * sChildSumLinearGain(
                                          child->volumeDbSnapshot() ) );
        } else {
            sChildSumAddClip( w, *lk, gain );
        }
    }
}

}  // namespace

bool STrack::collectChildSumEnvelope( const SEnvelopeWindow &win,
                                      preview_t *out ) const
{
    if( !out ) return false;
    const int width = win.width < 1 ? 1 : win.width;
    if( win.rightTime <= win.leftTime ) return false;

    STrack &self = const_cast<STrack &>( *this );
    if( !self.hasChildTracks() ) return false;

    // The root that HOLDS us (QBX-245), eventFeed's reason. One lookup per
    // call: the walk below carries it down rather than re-asking.
    SObject *root = splacements::rootHolding( self.getProjectSafe(), &self );

    std::vector<int32_t> sumMin( (size_t) width, 0 );
    std::vector<int32_t> sumMax( (size_t) width, 0 );
    SEnvelopeWindow useWin = win;
    useWin.width = width;
    SChildSumWalk w{ useWin, sumMin, sumMax, root,
                     ssolo::anySoloInForce( root ), false };

    // OUR OWN FADER IS NOT IN THE PRODUCT: each direct child starts at ITS OWN
    // gain. That is the rule the proposal adopts - the lane a waveform is drawn
    // on never scales it - and it is what makes M3.6's pair meaningful (a
    // child's fader moves the overlay, ours does not).
    for( SLink *lk : self.childLinks() ) {
        if( !lk ) continue;
        STrack *child = dynamic_cast<STrack *>( &lk->getSObject() );
        if( !child ) continue;
        if( !ssolo::isLaneAudible( root, child, w.anySolo ) ) continue;
        sChildSumAddTrack( w, *child,
                           sChildSumLinearGain( child->volumeDbSnapshot() ) );
    }

    if( !w.any ) return false;      // nothing to draw, so draw NOTHING

    // ONE clamp, here, on an int32 sum (design D3 / AC M3.2).
    for( int i = 0; i < width; ++i ) {
        int32_t mn = sumMin[i];
        int32_t mx = sumMax[i];
        if( mn < -127 ) mn = -127;
        if( mn >  127 ) mn =  127;
        if( mx < -127 ) mx = -127;
        if( mx >  127 ) mx =  127;
        out[i].min = (previewPart_t) mn;
        out[i].max = (previewPart_t) mx;
    }
    return true;
}

// --- automation (proposal 37 P5, design D5) ---------------------------------

void STrack::pushTrackAutomation()
{
    if( !cpGainStage_ ) return;

    SAutomationLane *vol = automationLane( QStringLiteral( "self:Volume" ) );
    // TRIM (the default, design §11 decision 3) keeps the fader meaningful: the
    // curve is an OFFSET in dB. Every other consuming mode is absolute, i.e.
    // the lane IS the value and the stored fader is not summed in.
    const bool absolute = vol && vol->mode() != SAutomationMode::Trim;
    cpGainStage_->setVolumeCurve( vol ? vol->snapshot() : nullptr, absolute );

    SAutomationLane *mute = automationLane( QStringLiteral( "self:Muted" ) );
    cpGainStage_->setMuteCurve( mute ? mute->snapshot() : nullptr );

    // THE `self:Pan` LANE (proposal 49 D3, M3). Same Trim/Read rule as the
    // fader's, in the pan domain: Trim SUMS with the stored pan, Read replaces
    // it. The clamp is the gain stage's, applied once after the sum.
    SAutomationLane *pan = automationLane( QStringLiteral( "self:Pan" ) );
    const bool panAbsolute = pan && pan->mode() != SAutomationMode::Trim;
    cpGainStage_->setPanCurve( pan ? pan->snapshot() : nullptr, panAbsolute );
}

void STrack::applyAutomationToEngine()
{
    pushTrackAutomation();
    // Also the CLIPS': a load reads a cut's inline <automation> before the
    // track's clip entries exist, so the pull has to happen from here too.
    refreshClipGainCurves();
}

void STrack::onAutomationChanged( SAutomationLane &lane, offset_t start, offset_t end )
{
    pushTrackAutomation();
    // twGainStage is class INFINITY and pure — the output of a frame depends on
    // that frame's input, the scalar and the frame's position, never on how the
    // component got there. So the invalidation is EXACT: only the pages the
    // curve actually moved go stale (design §4.5).
    (void) lane;
    if( end > start ) invalidateRenderPathRange( start, end );
}

// The curve lives on the clip's WINDOW object; the entry that consumes it lives
// in OUR twTrackMix. A take stack answers for its ACTIVE take, which is the one
// the mix resolves to.
void STrack::refreshClipGainCurves()
{
    if( !cpTrackMix_ ) return;

    // THE PLAYBACK AUTO-FADE (plan 53 P5, decisions D3a and D3b). The policy,
    // the ms-to-frames conversion and the seamless test are all in `sautofade`
    // (model) and none of it is here, for two reasons. The comp-boundary half
    // of the same feature is applied in `objects/cut`, which this slice may not
    // see and which may not see this one; and the Clip Properties panel shows
    // the user which edges the auto-fade skips, so the mix and the edit surface
    // have to read ONE answer to that question rather than two.
    const length_t autoFrames = sautofade::framesOf( getProjectSafe() );
    const QHash<SLink *, sautofade::Seams> seams =
        autoFrames > 0 ? sautofade::laneSeams( *this )
                       : QHash<SLink *, sautofade::Seams>();

    for( SLink *lk : childLinks() ) {
        if( !lk || !lk->hasStartTime() ) continue;
        SObject *obj = &lk->getSObject();
        if( obj->isLane() ) continue;          // a nested lane, not a clip
        // The BLOCKING duration, for the reason `sautofade::laneSeams` states
        // at its own call: this is an edit path, and the try-lock form can
        // answer with a pre-edit length under worker contention.
        const length_t placedLen = obj->getDurationBlocking();
        // The window whose PARAMETERS this placement carries — the placement's
        // own window when it has one, the active take when it does not. NOT
        // `windowTakeAt(-1)`, which since proposal 42 M2 forwards through a
        // window and would make a wrapped column's own clip gain and volume
        // inaudible in favour of the take's (`set-clip-volume` on a wrapped
        // column edits the WRAPPER, so the mix has to read the wrapper).
        // A TAKE COLUMN THAT RENDERS ITS COMP MAP APPLIES ITS TAKES' OWN
        // GAIN ITSELF, PER REGION (QBX-185), so this entry must carry NEUTRAL
        // values or the active take's trim lands twice: once over its own
        // regions by `twCompColumn` and once over the whole clip by the
        // entry. Asked through `SObject::appliesOwnWindowGain()` because this
        // slice may not depend on `objects/cut` -- the same seam
        // `clipFade()` is.
        //
        // It is the PLACEMENT's object that is asked, before the
        // `parametersOf` hop below: a WRAPPED column answers false (the
        // wrapper's own trim belongs to this entry and composes with each
        // take's), and a DEGENERATE one answers false too (it builds no
        // column, so its active take's trim must keep arriving here exactly
        // as it always has -- that is what keeps old projects byte-identical).
        const bool ownGain = obj->appliesOwnWindowGain();
        if( SClipWindow *w = SClipWindow::parametersOf( *obj ) )
            obj = &w->asObject();
        cpTrackMix_->setClipGainCurve(
            lk, ownGain ? nullptr
                        : obj->automationCurve( QStringLiteral( "cut:Gain" ) ) );
        // THE PER-CLIP STATIC VOLUME (per-clip volume/pan proposal).
        // obj->getVolume() is in dB — the same unit twGainStage's own static
        // scalar uses — so converting to linear here is what makes it compose
        // with cut:Gain's linear factor as a PRODUCT, i.e. the two sum in dB
        // (see ClipEntry::gainScalar / twGainStage's own "TRIM SUMS IN dB",
        // mix/CONTRACT.md inv. 21). Every SObject answers getVolume() at 0 dB
        // by default, so this is a no-op for every clip that never set one.
        cpTrackMix_->setClipGainScalar(
            lk, ownGain ? 1.0 : std::pow( 10.0, obj->getVolume() / 20.0 ) );
        // THE CLIP FADE (proposal 43 N5), through the same funnel and read
        // from the same window: it is a third factor in the mix's per-frame
        // product, beside the gain curve and the static volume.
        //
        // ...WITH THE PROJECT'S AUTO-FADE FILLED INTO THE EDGES THAT HAVE NO
        // FADE OF THEIR OWN AND DO NOT REFUSE ONE. It travels as part of the
        // same value and through the same setter, deliberately: the mix stays
        // a function of the fade it is handed and learns nothing about where
        // the lengths came from, so there is exactly one place that can be
        // asked why an edge does or does not fade.
        //
        // AN EVENT CLIP GETS NO AUTO-FADE, and this guard is the whole reason
        // the kind is asked for here. A fade is AUDIO-only (cut/CONTRACT
        // inv. 4) -- `set-clip-fade` REFUSES an event clip rather than
        // ignoring it -- and the auto-fade is a fade. The reason is not
        // bookkeeping: an event clip's edges are note-offs, not steps in a
        // waveform, so there is no click to remove, and the audio at those
        // edges is an instrument's ATTACK and RELEASE. A 3 ms ramp over them
        // would clip the attack and cut the release of every note at a clip
        // boundary in the project -- an auto-fade doing damage in exactly the
        // place it was added to prevent it.
        //
        // It is a real hazard and not a hypothetical: the mix applies
        // `ClipEntry::fade` to whatever a clip entry resolves to, and an
        // event clip IS one of this lane's `childLinks()` with a start time,
        // which is why it already travels three lines above for the gain
        // curve, the volume and the pan.
        twClipFade eff = ownGain ? twClipFade{} : obj->clipFade();
        if( !ownGain && autoFrames > 0
            && obj->contentKind() == SContentKind::Audio ) {
            const sautofade::Seams sm = seams.value( lk );
            eff = sautofade::effectiveFade( eff, placedLen, autoFrames,
                                            sm.in, sm.out );
        }
        cpTrackMix_->setClipFade( lk, eff );
        // THE CLIP'S PAN (proposal 49 D2, M1), the fourth factor, from the
        // same window: a wrapped column pans by the WRAPPER's value and a
        // direct one by the active take's -- exactly the object set-clip-pan
        // edits. The mix applies it only at width 2 (D1).
        cpTrackMix_->setClipPan( lk, ownGain ? 0.0 : obj->getPan() );
    }
}

void STrack::onPluginSlotAudioInvalidatedRange( qint64 start, qint64 end )
{
    // CLASS 1, so the range is open-ended by construction: the plugin's DSP
    // state at any position depends on everything before it (design F9).
    if( end <= start ) { invalidateRenderPath(); return; }
    invalidateRenderPathRange( (offset_t) start, (offset_t) end );
}

void STrack::onTrackVolumeChanged( double gainDb )
{
    // THE FADER IS POST-FX (proposal 37 D5, executed by P3a). It used to be
    // twTrackMix::setTrackGain(), which applied the scalar BEFORE the inserts;
    // twGainStage sits between the last insert and the rewire, so an insert now
    // sees the unfadered signal and the track's root -- the metering tap and the
    // mixer's input alike -- sees the faded one.
    if( cpGainStage_ ) {
        cpGainStage_->setGainDb( gainDb );
    }
    // Gain is baked into frozen pages from the gain stage downstream.
    invalidateRenderPath();
}

void STrack::onTrackPanChanged( double pan )
{
    // TRACK PAN IS POST-FX AND POST-FADER (proposal 49 D2): twGainStage, beside
    // the fader, so an insert sees the unpanned image and the root -- the
    // metering tap, the mixer input and the post-fader send tap -- sees the
    // panned one. The pre-fader send tap is the gain stage's INPUT, so it is
    // pre-pan too.
    if( cpGainStage_ ) {
        cpGainStage_->setPan( pan );
    }
    // invalidateRenderPath(), NEVER an epoch bump alone: proposal 45 measured
    // a muted master render coming back BYTE-IDENTICAL to the unmuted one with
    // only the bump, because the value is baked into every page downstream.
    invalidateRenderPath();
}

// Self-registration with the project loader (proposal 14, Phase 5): the
// persistence module names no concrete types; each slice registers its own
// element name. Relies on the app being an OBJECT library (no TU elision).
static const bool s_registered_strack =
    ( SProjectLoader::registerSObjectClass( "STrack",
          STrack::instantiateFromDomElement,
          // A CONTAINER of clips and nested lanes: one unloadable clip costs
          // its own link, never the track (proposal 37 D8a).
          SElementKind::Container ), true );
