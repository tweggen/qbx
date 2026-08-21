#include "app/shell/smidioutpump.h"

#include <algorithm>
#include <cmath>

#include <QDebug>
#include <QTimer>

#include "app/model/slink.h"
#include "app/model/sobject.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/model/sprojectprops.h"
#include "app/model/ssolorules.h"
#include "app/objects/track/strack.h"
#include "app/servicesui/soptions.h"
#include "app/shell/sapplication.h"
#include "app/shell/ssettings.h"

#include "tw/devices/capture_midi.h"
#include "tw/events/tweventmerge.h"
#include "tw/graph/tw303aenv.h"

namespace {

// Frames -> nanoseconds at the PROJECT rate. Everything the pump measures is
// in project frames (the locator's domain); only the device latency arrives in
// device frames, and SApplication::meterLatencyFrames() has already converted
// that one (proposal 34 - skipping it is a ~9 % error at 44.1 kHz on a 48 kHz
// device). The arithmetic itself lives on SPlayheadClock since proposal 21 L4,
// so the pump and the MIDI recorder cannot round differently.
inline qint64 framesToNs( qint64 frames, qint64 rate )
{
    return SPlayheadClock::framesToNs( frames, rate );
}

inline qint64 nsToFrames( qint64 ns, qint64 rate )
{
    return SPlayheadClock::nsToFrames( ns, rate );
}

inline int clamp7( double v )
{
    int i = (int) std::lround( v );
    if( i < 0 )   i = 0;
    if( i > 127 ) i = 127;
    return i;
}

}  // namespace

SMidiOutPump::SMidiOutPump( QObject *parent )
    : QObject( parent )
{
    timer_ = new QTimer( this );
    timer_->setInterval( kTickMs );
    // PRECISE, not the default coarse timer: Qt is allowed to coalesce a coarse
    // timer by up to 5 % of its interval, and on Windows a coarse timer rounds
    // to the system tick. The pump's whole budget is a few milliseconds of
    // jitter, so it asks for the accurate one explicitly.
    timer_->setTimerType( Qt::PreciseTimer );
    connect( timer_, &QTimer::timeout, this, &SMidiOutPump::tick );

    // The enumeration probes are constructed HERE, at app startup, and never
    // destroyed while the app lives. Two reasons, both load-bearing:
    //   - CaptureMidiOutput registers the most recently CONSTRUCTED live
    //     instance as the one the testkit reads, so a throwaway port minted to
    //     answer "what ports are there?" would unregister a scheduler's live
    //     one when it died;
    //   - constructing them first means any scheduler port (constructed later)
    //     always wins that registration, which is the instance the events
    //     actually go to.
    probeOut_ = audio::createMidiOutput();
    probeIn_  = audio::createMidiInput();
}

SMidiOutPump::~SMidiOutPump()
{
    // Explicit and ordered: every scheduler joins its std::thread here, on the
    // main thread at app teardown, rather than during static destruction where
    // the log sink may already be gone (the ~twPluginRegistry / TwLog hang this
    // repo has recorded).
    if( running_ ) panicAll();
    schedulers_.clear();
    probeOut_.reset();
    probeIn_.reset();
}

bool SMidiOutPump::isActive() const
{
    return running_ && timer_ && timer_->isActive();
}

// ------------------------------------------------------------- port lookup --

std::vector<audio::MidiPortInfo> SMidiOutPump::outputPorts() const
{
    return probeOut_ ? probeOut_->listPorts() : std::vector<audio::MidiPortInfo>();
}

std::vector<audio::MidiPortInfo> SMidiOutPump::inputPorts() const
{
    return probeIn_ ? probeIn_->listPorts() : std::vector<audio::MidiPortInfo>();
}

QString SMidiOutPump::backendName() const
{
    return probeOut_ ? QString::fromLatin1( probeOut_->backendName() )
                     : QStringLiteral( "none" );
}

bool SMidiOutPump::supportsVirtualPorts() const
{
    // tw/devices CONTRACT inv. 18: CoreMIDI and ALSA-seq have the concept,
    // WinMM does not (a loopback driver appears as an ordinary device there).
    // Gated on the BACKEND, never on the platform, and never by calling
    // createVirtualPort() to find out - that would create one.
    return backendName() != QStringLiteral( "winmm" );
}

QString SMidiOutPump::resolvePortId( const QString &portName ) const
{
    if( portName.isEmpty() ) return QString();

    auto cached = portIdCache_.find( portName );
    if( cached != portIdCache_.end() ) return cached->second;

    QString id;
    // 1. An explicit per-machine mapping wins: this is the whole reason the
    //    project stores a NAME. A WinMM device index means nothing on the next
    //    machine, and a CoreMIDI uniqueID even less.
    id = SSettings::instance().midiPortId( portName );
    if( id.isEmpty() ) {
        // 2. Otherwise match the backend's own list, id first, then name.
        const std::vector<audio::MidiPortInfo> ports = outputPorts();
        for( const audio::MidiPortInfo &p : ports ) {
            if( QString::fromStdString( p.id ).compare(
                    portName, Qt::CaseInsensitive ) == 0 ) { id = QString::fromStdString( p.id ); break; }
        }
        if( id.isEmpty() ) {
            for( const audio::MidiPortInfo &p : ports ) {
                if( QString::fromStdString( p.name ).compare(
                        portName, Qt::CaseInsensitive ) == 0 ) {
                    id = QString::fromStdString( p.id );
                    break;
                }
            }
        }
    }
    // 3. Fall back to the name verbatim - a backend whose ids ARE names (and
    //    the "default" spelling) then still works with no configuration.
    if( id.isEmpty() ) id = portName;

    const_cast<SMidiOutPump *>( this )->portIdCache_[portName] = id;
    return id;
}

audio::MidiOutScheduler *SMidiOutPump::schedulerFor( const QString &portId )
{
    auto it = schedulers_.find( portId );
    if( it != schedulers_.end() ) return it->second.get();
    if( failedPorts_.contains( portId ) ) return nullptr;

    std::unique_ptr<audio::MidiOutput> out = audio::createMidiOutput();
    if( !out ) {
        failedPorts_.append( portId );
        return nullptr;
    }
    auto sched = std::unique_ptr<audio::MidiOutScheduler>(
        new audio::MidiOutScheduler( std::move( out ) ) );
    if( !sched->start( portId.toStdString() ) ) {
        qWarning() << "midi-out: could not open port" << portId
                   << "- this track will not send. Check Options -> MIDI.";
        failedPorts_.append( portId );
        return nullptr;
    }
    audio::MidiOutScheduler *raw = sched.get();
    schedulers_[portId] = std::move( sched );
    return raw;
}

audio::MidiOutScheduler *SMidiOutPump::thruSchedulerFor( const QString &portName )
{
    const QString portId = resolvePortId( portName );
    if( portId.isEmpty() ) return nullptr;
    return schedulerFor( portId );
}

// ------------------------------------------------------------- transport ----

void SMidiOutPump::start()
{
    SApplication &app = SApplication::app();
    if( app.isRenderingActive() ) return;   // renders emit nothing (D6)

    // Read the two Options -> MIDI values ONCE per run: a chase issued under
    // one setting and events emitted under another would be inconsistent, and
    // the run is the natural scope for that consistency.
    SSettings &s = SSettings::instance();
    chaseNoteOns_ = s.value( SOpt::MidiChaseNoteOns,
                             SOpt::def( SOpt::MidiChaseNoteOns ) ).toBool();
    globalOffsetMs_ = s.value( SOpt::MidiOutOffsetMs,
                               SOpt::def( SOpt::MidiOutOffsetMs ) ).toInt();

    // The capture MIDI recording is cleared at the START of a run, exactly as
    // CaptureBackend::clearCapture() clears the audio recording in
    // startOutput(): a captured host time is only meaningful against THIS
    // session's audio block log, so mixing two sessions' messages into one
    // recording would map half of them through the wrong clock.
    if( audio::CaptureMidiOutput *cap = audio::CaptureMidiOutput::active() )
        cap->clear();

    portIdCache_.clear();   // a device may have been re-selected while stopped
    resetRun( app.getGlobalLocatorPos() );
    running_ = true;

    // OPEN THE PORTS NOW, not on the first tick that needs one. Opening a MIDI
    // device is unbounded work - a real WinMM/CoreMIDI open, a thread spawn, a
    // high-resolution timer - and the first tick that has a usable clock is
    // exactly the tick that must not do unbounded work: an event at the very
    // first frame of playback has a due time that is ALREADY in the past by
    // then, so every millisecond spent opening the port is a millisecond that
    // event is late. Measured under a loaded box, lazily opening it put the
    // first note of a run 153 ms (7364 frames) late while every later note was
    // within 12 ms; opened here it is within 8 ms.
    //
    // The lazy path in schedulerFor() stays, because a port can appear
    // mid-run (a track gains one, or is un-muted).
    if( SProject *project = app.getCurrentProject() ) {
        SObject *root = splacements::rootContainer( project );
        std::vector<STrack *> tracks;
        collectTracks( root, ssolo::anySoloInTree( root ), tracks );
        for( STrack *t : tracks ) {
            const QString portId = resolvePortId( t->getMidiOutPort() );
            if( !portId.isEmpty() ) schedulerFor( portId );
        }
    }

    if( timer_ && !timer_->isActive() ) timer_->start();
}

void SMidiOutPump::stop()
{
    if( !running_ ) {
        if( timer_ ) timer_->stop();
        return;
    }
    running_ = false;
    if( timer_ ) timer_->stop();
    // Sustain-off + all-notes-off on every channel this run used, after
    // discarding the queued future: the queue describes a playhead that no
    // longer exists, and a note-on that escaped after the stop is a stuck note.
    panicAll();
    cursors_.clear();
    clock_.beginRun( lastPos_, SApplication::app().locatorPublishSeq() );
}

void SMidiOutPump::locate( offset_t newPos )
{
    if( !running_ ) return;
    panicAll();
    resetRun( newPos );
}

void SMidiOutPump::resetRun( offset_t pos )
{
    cursors_.clear();
    lastPos_        = (qint64) pos;
    // The clock takes the publication counter AS OF NOW, so the first anchor
    // waits for a publication that happened after this run began (see
    // SPlayheadClock::beginRun - zero would be wrong on the second run of a
    // process, and is how the first note went out 59 ms early).
    clock_.beginRun( (qint64) pos, SApplication::app().locatorPublishSeq() );
    playIter_       = 0;
}

void SMidiOutPump::panicAll()
{
    std::uint16_t mask = 0;
    for( const auto &kv : cursors_ ) mask |= kv.second.usedChannels;
    // Nothing sent yet? Still panic every channel: a previous run (or another
    // application on the same port) may have left something sounding, and an
    // all-notes-off nobody needed is inaudible.
    if( mask == 0 ) mask = 0xFFFF;
    for( auto &kv : schedulers_ ) {
        kv.second->flush();
        kv.second->panic( mask );
    }
    for( auto &kv : cursors_ ) {
        kv.second.held.clear();
        kv.second.chased = false;
    }
}

// ----------------------------------------------------------------- helpers --

SMidiOutPump::Cycle SMidiOutPump::readCycle( SProject *project )
{
    Cycle c;
    if( !project ) return c;
    const bool on    = project->prop( SProjectProps::Cycle, false ).toBool();
    const bool valid = project->prop( SProjectProps::RangeValid, false ).toBool();
    const qint64 a = (qint64) project->prop( SProjectProps::RangeStart,
                                             (qulonglong) 0 ).toULongLong();
    const qint64 b = (qint64) project->prop( SProjectProps::RangeEnd,
                                             (qulonglong) 0 ).toULongLong();
    if( on && valid && b > a ) { c.on = true; c.start = a; c.end = b; }
    return c;
}

void SMidiOutPump::collectTracks( SObject *node, bool anySolo,
                                  std::vector<STrack *> &out ) const
{
    if( !node ) return;
    SObject *root = node;
    // The walk starts at the root container; recurse through lanes only.
    std::vector<SObject *> stack{ root };
    while( !stack.empty() ) {
        SObject *cur = stack.back();
        stack.pop_back();
        for( SLink *lk : cur->childLinks() ) {
            if( !lk ) continue;
            SObject *child = &lk->getSObject();
            if( !child->isLane() ) continue;
            STrack *track = dynamic_cast<STrack *>( child );
            if( track ) {
                // A muted or solo-excluded track sends nothing, by the same
                // ssolorules resolution that keeps its audio out of the sum -
                // a MIDI-out that kept playing through a mute would disagree
                // with every other thing the mute does.
                if( track->hasMidiOut()
                    && ssolo::isLaneAudible( root, track, anySolo ) )
                    out.push_back( track );
            }
            stack.push_back( child );
        }
    }
}

qint64 SMidiOutPump::dueNsFor( qint64 absFrame, qint64 rate, int trackOffsetMs,
                               qint64 midiLatencyNs ) const
{
    // D6: dueHostTime = hostTime(playhead) + deviceOutputLatency
    //                   - midiOutLatency - globalOffset - trackOffset.
    // hostTime(playhead) is SPlayheadClock's anchor extended at the project
    // rate; the device latency is already folded into that anchor (it is one
    // term for every track, so it is applied once).
    const qint64 offsetNs =
        ( (qint64) trackOffsetMs + (qint64) globalOffsetMs_ ) * 1000000LL;
    return clock_.hostNsForFrame( absFrame, rate ) - midiLatencyNs - offsetNs;
}

void SMidiOutPump::send3( audio::MidiOutScheduler *sched, qint64 dueNs,
                          int status, int d1, int d2 )
{
    const std::uint8_t bytes[3] = { (std::uint8_t) status, (std::uint8_t) d1,
                                    (std::uint8_t) d2 };
    sched->enqueue( dueNs, bytes, 3 );
}

void SMidiOutPump::send2( audio::MidiOutScheduler *sched, qint64 dueNs,
                          int status, int d1 )
{
    const std::uint8_t bytes[2] = { (std::uint8_t) status, (std::uint8_t) d1 };
    sched->enqueue( dueNs, bytes, 2 );
}

// ------------------------------------------------------------------- tick ---

void SMidiOutPump::tick()
{
    SApplication &app = SApplication::app();
    if( !running_ || app.isRenderingActive() ) return;

    SProject *project = app.getCurrentProject();
    tw303aEnvironment *env = app.get303aEnvironment();
    const qint64 rate = env ? (qint64) env->getSRate() : 0;
    if( !project || rate <= 0 ) return;

    const qint64 now = audio::MidiOutScheduler::hostNowNs();
    const qint64 pos = (qint64) app.getGlobalLocatorPos();
    const Cycle  cyc = readCycle( project );
    const qint64 cycleLen = cyc.on ? cyc.len() : 0;

    // --- wrap / stray-locate detection ------------------------------------
    if( pos < lastPos_ ) {
        if( cyc.on && pos >= cyc.start && pos < cyc.end
            && lastPos_ <= cyc.end + cycleLen ) {
            ++playIter_;              // the engine wrapped at the cycle end
        } else {
            // A backwards jump that is not a wrap is a locate nobody told us
            // about. Treat it as one rather than emit the past.
            panicAll();
            resetRun( (offset_t) pos );
            return;
        }
    }
    lastPos_ = pos;

    // --- the anchor -------------------------------------------------------
    //
    // Re-taken on every position PUBLICATION by the RT thread, not on every
    // position CHANGE: the two differ exactly once, at the start, and that one
    // time is the one that matters. twSpeaker defers the device start until the
    // readahead is primed, so between <toggle-playback> and the first callback
    // the playhead sits still at the locator - anchoring on a change would
    // either hang a due time on a clock that is not running yet, or wait for
    // the SECOND callback and lose the first buffer of events with it.
    //
    // Two corrections, neither of them guessed at:
    //
    //  * THE PUBLISH LAG. twSpeaker publishes engine->currentPosition() AFTER
    //    the pull, so the value seen at `now` means "everything up to P has
    //    been handed to the device" - the frame just delivered is
    //    P - bufferFrames, not P. Without this every note is one device buffer
    //    (~21 ms at 1024 frames / 48 kHz) early.
    //  * THE DEVICE LATENCY. Audio handed over now is HEARD one output latency
    //    later, and the MIDI has to reach the outboard synth at that same
    //    instant (D6). meterLatencyFrames() is reused verbatim because it
    //    already converts DEVICE frames at the DEVICE rate into PROJECT frames
    //    - the ~9 % error proposal 34 records for 44.1 kHz on a 48 kHz device.
    // The first anchor of a run is GUARDED (SPlayheadClock): a locate is
    // published by the UI thread immediately, but the RT thread can still
    // deliver one block against the OLD position before the engine's seek
    // lands, and anchoring on that would put a whole window's due times in the
    // past and flush it at once. NOT GATED: a seek DURING playback has no
    // bespoke case - a timing assertion tight enough to separate the
    // behaviours would be flaky.
    if( !clock_.observe( now, pos, app.locatorPublishSeq(), rate,
                         (qint64) app.outputBufferFramesProject(),
                         (qint64) app.meterLatencyFrames(),
                         (qint64) playIter_ * cycleLen ) )
        return;

    // Where the ear is right now, in wrap-counted frames, and how far ahead of
    // it we are willing to queue.
    const qint64 heardAbs = clock_.frameAtHostNs( now, rate );
    const qint64 lookaheadFrames =
        nsToFrames( (qint64) kLookaheadMs * 1000000LL, rate );
    const qint64 maxBacklog =
        nsToFrames( (qint64) kMaxBacklogMs * 1000000LL, rate );

    // --- the tracks -------------------------------------------------------
    SObject *root = splacements::rootContainer( project );
    const bool anySolo = ssolo::anySoloInTree( root );
    std::vector<STrack *> tracks;
    collectTracks( root, anySolo, tracks );

    // Prune the cursors of tracks that lost their port, were muted, or went
    // away, releasing whatever they were holding first - a mute must not leave
    // a note sounding on the hardware.
    for( auto it = cursors_.begin(); it != cursors_.end(); ) {
        if( std::find( tracks.begin(), tracks.end(), it->first ) == tracks.end() ) {
            audio::MidiOutScheduler *sched = schedulerFor( it->second.portId );
            if( sched ) releaseHeld( it->second, heardAbs, rate, 0, sched );
            it = cursors_.erase( it );
        } else {
            ++it;
        }
    }

    for( STrack *track : tracks ) {
        const QString portId = resolvePortId( track->getMidiOutPort() );
        if( portId.isEmpty() ) continue;
        audio::MidiOutScheduler *sched = schedulerFor( portId );
        if( !sched ) continue;

        Cursor &c = cursors_[track];
        const int offsetMs = track->getMidiOutOffsetMs();
        if( c.portId != portId ) {
            // First sight of this track this run (or its port changed). The
            // frontier starts where the RUN started, never at the current
            // playhead: the first tick with a usable anchor happens one device
            // buffer into playback, and starting at the playhead would drop
            // every event inside that buffer - a note at 0 among them.
            if( !c.portId.isEmpty() ) {
                audio::MidiOutScheduler *old = schedulerFor( c.portId );
                if( old ) releaseHeld( c, heardAbs, rate, offsetMs, old );
            }
            c.portId = portId;
            c.iter   = playIter_;
            c.pos    = clock_.runStartPos();
            c.chased = false;
        }

        const std::shared_ptr<twEventMerge> feed = track->eventFeed();
        if( !feed ) continue;

        // A POSITIVE offset means "send EARLIER", so a track asking for 500 ms
        // early has to be sliced 500 ms further ahead or it is structurally
        // late no matter what the scheduler does.
        const qint64 offsetFrames =
            nsToFrames( ( (qint64) offsetMs + (qint64) globalOffsetMs_ ) * 1000000LL,
                        rate );
        const qint64 target =
            heardAbs + lookaheadFrames + std::max( (qint64) 0, offsetFrames );

        // A cursor that fell far behind (a long GUI stall) DROPS its backlog
        // rather than flushing notes whose moment has passed.
        if( c.pos + (qint64) c.iter * cycleLen < heardAbs - maxBacklog ) {
            releaseHeld( c, heardAbs, rate, offsetMs, sched );
            c.pos    = heardAbs - (qint64) playIter_ * cycleLen;
            c.iter   = playIter_;
            c.chased = false;
        }

        if( !c.chased ) {
            // The chase is what makes "start in the middle of a note" correct:
            // collect() reports what is already sounding at the position and
            // every controller value that got it there (D4's reset + chase).
            feed->collect( c.pos, 1, scratch_ );
            emitChase( scratch_, c, track, c.pos + (qint64) c.iter * cycleLen,
                       rate, sched );
            c.chased = true;
        }

        int guard = 0;
        while( c.pos + (qint64) c.iter * cycleLen < target && ++guard < 64 ) {
            qint64 segEnd = target - (qint64) c.iter * cycleLen;
            bool wrap = false;
            if( cyc.on && segEnd >= cyc.end && c.pos < cyc.end ) {
                segEnd = cyc.end;
                wrap = true;
            }
            if( segEnd > c.pos ) {
                feed->collect( c.pos, segEnd - c.pos, scratch_ );
                emitBlock( scratch_, c, track,
                           c.pos + (qint64) c.iter * cycleLen, rate, sched );
                c.pos = segEnd;
            }
            if( !wrap ) break;
            // LOOP WRAP (D6): every note this pump sent and has not released
            // gets a note-off AT the cycle end, then the chase is re-issued at
            // the cycle start. Without the note-off the wrap leaves a note
            // hanging on the hardware for the rest of the session; without the
            // chase the new pass starts with the previous pass's controllers.
            const qint64 endAbs = cyc.end + (qint64) c.iter * cycleLen;
            releaseHeld( c, endAbs, rate, offsetMs, sched );
            ++c.iter;
            c.pos = cyc.start;
            feed->collect( c.pos, 1, scratch_ );
            emitChase( scratch_, c, track, c.pos + (qint64) c.iter * cycleLen,
                       rate, sched );
        }
    }
}

// --------------------------------------------------------------- emission ---

void SMidiOutPump::emitBlock( const twEventBlock &block, Cursor &c,
                              STrack *track, qint64 windowStartAbs, qint64 rate,
                              audio::MidiOutScheduler *sched )
{
    const int forcedCh = track->getMidiOutChannel();
    const int offsetMs = track->getMidiOutOffsetMs();
    const qint64 midiLatencyNs =
        sched->output() ? (qint64) sched->output()->latencyNs() : 0;

    for( const twEvent &e : block.events ) {
        const int ch = ( forcedCh >= 0 ) ? forcedCh
                                         : ( e.channel >= 0 ? (int) e.channel : 0 );
        const qint64 dueNs = dueNsFor( windowStartAbs + e.time, rate, offsetMs,
                                       midiLatencyNs );
        switch( e.kind ) {
        case twEventKind::NoteOn: {
            const int vel = clamp7( e.value );
            if( vel == 0 ) {          // velocity 0 IS a note-off on the wire
                send3( sched, dueNs, 0x80 | ( ch & 0x0F ), (int) e.key & 0x7F, 64 );
                break;
            }
            send3( sched, dueNs, 0x90 | ( ch & 0x0F ), (int) e.key & 0x7F, vel );
            c.held.push_back( Held{ ch, (int) e.key & 0x7F } );
            c.usedChannels |= (std::uint16_t) ( 1u << ( ch & 0x0F ) );
            break;
        }
        case twEventKind::NoteOff:
        case twEventKind::NoteChoke:
        case twEventKind::NoteEnd: {
            send3( sched, dueNs, 0x80 | ( ch & 0x0F ), (int) e.key & 0x7F,
                   e.value > 0 ? clamp7( e.value ) : 64 );
            const int key = (int) e.key & 0x7F;
            for( auto it = c.held.begin(); it != c.held.end(); ++it ) {
                if( it->channel == ch && it->key == key ) { c.held.erase( it ); break; }
            }
            break;
        }
        case twEventKind::ControlChange:
            send3( sched, dueNs, 0xB0 | ( ch & 0x0F ),
                   (int) ( e.paramId & 0x7F ), clamp7( e.value ) );
            c.usedChannels |= (std::uint16_t) ( 1u << ( ch & 0x0F ) );
            break;
        case twEventKind::ProgramChange:
            send2( sched, dueNs, 0xC0 | ( ch & 0x0F ), clamp7( e.value ) );
            break;
        case twEventKind::ChannelPressure:
            send2( sched, dueNs, 0xD0 | ( ch & 0x0F ), clamp7( e.value ) );
            break;
        case twEventKind::PolyPressure:
            send3( sched, dueNs, 0xA0 | ( ch & 0x0F ), (int) e.key & 0x7F,
                   clamp7( e.value ) );
            break;
        case twEventKind::PitchBend: {
            // twEvent stores the SIGNED offset from centre, the same spelling
            // twSmf reads and writes; the wire wants 14 bits with 8192 centre.
            int v = (int) std::lround( e.value ) + 8192;
            if( v < 0 ) v = 0;
            if( v > 16383 ) v = 16383;
            send3( sched, dueNs, 0xE0 | ( ch & 0x0F ), v & 0x7F, ( v >> 7 ) & 0x7F );
            break;
        }
        default:
            // Metadata (tempo, markers, lyrics) describes the SCORE, not a
            // performance gesture: it never goes on the wire. Sysex needs the
            // arena and a ring slot longer than kMaxMessageBytes - P9.
            break;
        }
    }
}

void SMidiOutPump::emitChase( const twEventBlock &block, Cursor &c,
                              STrack *track, qint64 atAbs, qint64 rate,
                              audio::MidiOutScheduler *sched )
{
    const twEventState &st = block.chase;
    const int forcedCh = track->getMidiOutChannel();
    const int offsetMs = track->getMidiOutOffsetMs();
    const qint64 midiLatencyNs =
        sched->output() ? (qint64) sched->output()->latencyNs() : 0;
    const qint64 dueNs = dueNsFor( atAbs, rate, offsetMs, midiLatencyNs );

    auto chanOf = [&]( int16_t evCh ) {
        return ( forcedCh >= 0 ) ? forcedCh : ( evCh >= 0 ? (int) evCh : 0 );
    };
    auto markUsed = [&]( int ch ) {
        c.usedChannels |= (std::uint16_t) ( 1u << ( ch & 0x0F ) );
    };

    // Controllers ALWAYS (D6). Enqueued before the note-ons and with the same
    // due time: MidiOutScheduler sorts its pending list STABLY, so equal due
    // times reach the wire in the order produced here - a CC before the note
    // it sets up.
    for( const auto &kv : st.cc ) {
        const int ch = chanOf( kv.first.first );
        send3( sched, dueNs, 0xB0 | ( ch & 0x0F ),
               (int) ( kv.first.second & 0x7F ), clamp7( kv.second ) );
        markUsed( ch );
    }
    for( const auto &kv : st.program ) {
        const int ch = chanOf( kv.first );
        send2( sched, dueNs, 0xC0 | ( ch & 0x0F ), clamp7( (double) kv.second ) );
        markUsed( ch );
    }
    for( const auto &kv : st.bend ) {
        const int ch = chanOf( kv.first );
        int v = (int) std::lround( kv.second ) + 8192;
        if( v < 0 ) v = 0;
        if( v > 16383 ) v = 16383;
        send3( sched, dueNs, 0xE0 | ( ch & 0x0F ), v & 0x7F, ( v >> 7 ) & 0x7F );
        markUsed( ch );
    }
    for( const auto &kv : st.pressure ) {
        const int ch = chanOf( kv.first );
        send2( sched, dueNs, 0xD0 | ( ch & 0x0F ), clamp7( kv.second ) );
        markUsed( ch );
    }

    // Note-ons only when asked for. Default OFF for MIDI out (D6): re-attacking
    // a hardware synth's notes on every locate is usually a surprise rather
    // than a service, which is not true of an in-app instrument.
    if( !chaseNoteOns_ ) return;
    for( const twHeldNote &h : st.notes ) {
        const int ch = chanOf( h.channel );
        const int vel = clamp7( h.velocity > 0 ? h.velocity : 100.0 );
        send3( sched, dueNs, 0x90 | ( ch & 0x0F ), (int) h.key & 0x7F, vel );
        c.held.push_back( Held{ ch, (int) h.key & 0x7F } );
        markUsed( ch );
    }
}

void SMidiOutPump::releaseHeld( Cursor &c, qint64 atAbs, qint64 rate,
                                int offsetMs, audio::MidiOutScheduler *sched )
{
    if( c.held.empty() || !sched ) { c.held.clear(); return; }
    const qint64 midiLatencyNs =
        sched->output() ? (qint64) sched->output()->latencyNs() : 0;
    const qint64 dueNs = dueNsFor( atAbs, rate, offsetMs, midiLatencyNs );
    for( const Held &h : c.held )
        send3( sched, dueNs, 0x80 | ( h.channel & 0x0F ), h.key & 0x7F, 64 );
    c.held.clear();
}
