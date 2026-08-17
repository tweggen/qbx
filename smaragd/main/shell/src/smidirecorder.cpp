#include "app/shell/smidirecorder.h"

#include <algorithm>
#include <map>

#include <QTimer>
#include <QUndoStack>

#include "app/actions/sactionhistory.h"
#include "app/model/slink.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/model/sprojectprops.h"
#include "app/objects/midi/smidirecordactions.h"
#include "app/objects/midi/smidisequence.h"
#include "app/objects/track/strack.h"
#include "app/servicesui/soptions.h"
#include "app/shell/sapplication.h"
#include "app/shell/smidiinputhub.h"
#include "app/shell/ssettings.h"

#include "tw/core/twlog.h"
#include "tw/devices/midi_out_scheduler.h"
#include "tw/events/twtempomap.h"
#include "tw/graph/tw303aenv.h"

namespace {

// Depth-first, root-relative paths - the same walk SAudioRecorder does, with
// the opposite half of the filter. The ORDER is the contract between the
// capture and the placement, so both sides read this one list.
void collectArmedMidi( SObject *container, const QList<int> &path,
                       std::vector<std::pair<STrack *, QList<int> > > &out )
{
    if( !container ) return;
    for( int i = 0; i < container->childCount(); ++i ) {
        SLink *lk = container->childAt( i );
        if( !lk ) continue;
        SObject *child = &lk->getSObject();
        if( !child->isPathContainer() ) continue;
        QList<int> p = path;
        p.append( i );
        if( STrack *t = dynamic_cast<STrack *>( child ) ) {
            if( t->isArmedForRecording() && t->hasMidiTrackInput() )
                out.push_back( { t, p } );
        }
        collectArmedMidi( child, p, out );
    }
}

}  // namespace

SMidiRecorder::SMidiRecorder( SApplication *app, QObject *parent )
    : QObject( parent ), app_( app )
{
    timer_ = new QTimer( this );
    timer_->setInterval( kTickMs );
    // PRECISE, like the MIDI-out pump's: Qt may coalesce a coarse timer by 5 %
    // of its interval, and on Windows a coarse timer rounds to the system
    // tick. What is at stake here is not a due time but ring HEADROOM, and the
    // headroom is 1024 messages either way - but the two timers are siblings
    // and a difference between them would be a difference nobody chose.
    timer_->setTimerType( Qt::PreciseTimer );
    connect( timer_, &QTimer::timeout, this, &SMidiRecorder::poll );
}

SMidiRecorder::~SMidiRecorder()
{
    if( active_ ) stop();
}

double SMidiRecorder::projectRate_() const
{
    const int r = ( app_ && app_->get303aEnvironment() )
                      ? app_->get303aEnvironment()->getSRate() : 48000;
    return r > 0 ? (double) r : 48000.0;
}

SMidiRecorder::Mode SMidiRecorder::modeFromString( const QString &s, bool *ok )
{
    if( ok ) *ok = true;
    if( s.compare( QStringLiteral( "overdub" ), Qt::CaseInsensitive ) == 0 )
        return Mode::Overdub;
    if( s.compare( QStringLiteral( "replace" ), Qt::CaseInsensitive ) == 0 )
        return Mode::Replace;
    if( s.compare( QStringLiteral( "new-take" ), Qt::CaseInsensitive ) == 0 )
        return Mode::NewTake;
    if( ok ) *ok = false;
    return Mode::NewTake;
}

QString SMidiRecorder::modeToString( Mode m )
{
    switch( m ) {
    case Mode::Overdub: return QStringLiteral( "overdub" );
    case Mode::Replace: return QStringLiteral( "replace" );
    case Mode::NewTake: break;
    }
    return QStringLiteral( "new-take" );
}

bool SMidiRecorder::cycleRegion_( offset_t &in, offset_t &out ) const
{
    SProject *p = app_ ? app_->getCurrentProject() : nullptr;
    if( !p ) return false;
    if( !p->prop( SProjectProps::Cycle, false ).toBool() ) return false;
    if( !p->prop( SProjectProps::RangeValid, false ).toBool() ) return false;
    in  = (offset_t) p->prop( SProjectProps::RangeStart, (qulonglong) 0 ).toULongLong();
    out = (offset_t) p->prop( SProjectProps::RangeEnd,   (qulonglong) 0 ).toULongLong();
    return out > in;
}

// ------------------------------------------------------------------ start --

bool SMidiRecorder::collectArmed_()
{
    armed_.clear();
    ports_.clear();
    SProject *project = app_ ? app_->getCurrentProject() : nullptr;
    if( !project ) return false;
    std::vector<std::pair<STrack *, QList<int> > > found;
    collectArmedMidi( project->getRootComponent(), QList<int>(), found );
    for( const auto &pr : found ) {
        Armed a;
        a.track   = pr.first;
        a.path    = pr.second;
        a.channel = pr.first->trackInputMidiChannel();
        a.portIndex = portIndexFor_( pr.first->trackInputMidiPort() );
        if( a.portIndex < 0 ) {
            TW_LOGW( "shell", "[MREC] no MIDI input port '%s' for an armed track",
                     pr.first->trackInputMidiPort().toStdString().c_str() );
            continue;
        }
        armed_.push_back( a );
    }
    return !armed_.empty();
}

int SMidiRecorder::portIndexFor_( const QString &portName )
{
    for( std::size_t i = 0; i < ports_.size(); ++i )
        if( ports_[i].name == portName ) return (int) i;
    SMidiInputHub *hub = app_ ? app_->midiInputHub() : nullptr;
    if( !hub ) return -1;
    audio::MidiInFanout::Sink *sink = hub->recorderSink( portName );
    if( !sink ) return -1;
    PortIn p;
    p.name = portName;
    p.sink = sink;
    p.offsetProj = (qint64) ( SSettings::instance().midiInputOffsetMs( portName )
                                  * projectRate_() / 1000.0 + 0.5 );
    ports_.push_back( p );
    return (int) ports_.size() - 1;
}

bool SMidiRecorder::start()
{
    if( active_ ) return true;
    error_.clear();
    lastPassCount_ = lastPlacements_ = lastNotes_ = lastEvents_ = 0;
    lastTracks_ = 0;
    captured_ = clamped_ = 0;

    SProject *project = app_ ? app_->getCurrentProject() : nullptr;
    if( !project ) { error_ = QStringLiteral( "no project" ); return false; }
    if( !collectArmed_() ) {
        error_ = QStringLiteral( "no armed MIDI tracks" );
        return false;
    }

    recordStart_ = app_->getGlobalLocatorPos();
    cycle_       = cycleRegion_( cycleIn_, cycleOut_ );

    // The rings are DRAINED, never cleared: `MidiInRing::clear()` is only safe
    // while the producer is known to be idle, and a performer's finger is not.
    // Popping is the consumer side and is always safe. Whatever was in there
    // belongs to before the button was pressed - a retrospective `place-retro-
    // midi` (design D8) would keep it, and is not implemented.
    for( PortIn &p : ports_ ) {
        audio::MidiInMessage m;
        int dropped = 0;
        while( p.sink && p.sink->pop( m ) ) ++dropped;
        p.msgs.clear();
        if( dropped )
            TW_LOGD( "shell", "[MREC] dropped %d stale message(s) on '%s'",
                     dropped, p.name.toStdString().c_str() );
    }

    // Read ONCE: a settings change mid-take must not make the commit
    // inconsistent with what was captured.
    mode_ = modeFromString(
        SSettings::instance().value( SOpt::MidiRecordMode,
                                     SOpt::def( SOpt::MidiRecordMode ) ).toString() );
    quantize_ = SSettings::instance().value(
        SOpt::MidiRecordQuantize,
        SOpt::def( SOpt::MidiRecordQuantize ) ).toString();

    lastPos_  = (qint64) recordStart_;
    playIter_ = 0;
    clock_.beginRun( (qint64) recordStart_, app_->locatorPublishSeq() );

    active_ = true;
    timer_->start();
    TW_LOGI( "shell", "[MREC] started at %lld: %u track(s) on %u port(s), "
                      "mode=%s quantize=%s%s",
             (long long) recordStart_, (unsigned) armed_.size(),
             (unsigned) ports_.size(),
             modeToString( mode_ ).toStdString().c_str(),
             quantize_.toStdString().c_str(), cycle_ ? " [cycle]" : "" );
    return true;
}

// ------------------------------------------------------------------- tick --

void SMidiRecorder::drainRings_()
{
    for( PortIn &p : ports_ ) {
        if( !p.sink ) continue;
        audio::MidiInMessage m;
        while( p.sink->pop( m ) ) {
            if( m.size == 0 ) continue;
            Msg out;
            out.hostNs = m.hostTimeNs;
            out.size   = m.size;
            for( int i = 0; i < (int) m.size && i < 16; ++i )
                out.bytes[i] = m.bytes[i];
            p.msgs.push_back( out );
            ++captured_;
        }
    }
}

void SMidiRecorder::poll()
{
    if( !active_ ) return;
    SApplication &app = *app_;
    tw303aEnvironment *env = app.get303aEnvironment();
    const qint64 rate = env ? (qint64) env->getSRate() : 0;
    if( rate <= 0 ) return;

    const qint64 now = audio::MidiOutScheduler::hostNowNs();
    const qint64 pos = (qint64) app.getGlobalLocatorPos();

    // Wrap counting, so every frame this class computes is UNWRAPPED and
    // monotone - which is what makes the loop-pass arithmetic at the commit a
    // division rather than a search. Identical in shape to SMidiOutPump's.
    const qint64 cycleLen = cycle_ ? (qint64) ( cycleOut_ - cycleIn_ ) : 0;
    if( pos < lastPos_ && cycle_ && pos >= (qint64) cycleIn_
        && pos < (qint64) cycleOut_ && lastPos_ <= (qint64) cycleOut_ + cycleLen )
        ++playIter_;
    lastPos_ = pos;

    clock_.observe( now, pos, app.locatorPublishSeq(), rate,
                    (qint64) app.outputBufferFramesProject(),
                    (qint64) app.meterLatencyFrames(),
                    (qint64) playIter_ * cycleLen );

    // The rings are drained WHATEVER the clock says. A message that arrived
    // before the first publication is exactly the retrospective case (design
    // D6): its host time is kept and mapped at the stop, backwards through
    // whatever anchor exists by then.
    drainRings_();

    stopFrame_ = (offset_t) std::max<qint64>(
        (qint64) recordStart_, pos + (qint64) playIter_ * cycleLen );
}

// -------------------------------------------------------------------- stop --

void SMidiRecorder::stop()
{
    if( !active_ ) return;
    active_ = false;
    timer_->stop();

    // One last drain and one last look at the clock, BEFORE the transport is
    // stopped by whoever owns it: the anchor is only valid while the RT is
    // publishing, and every host time captured this run is mapped through it.
    pollFinal_();
    commit_();

    TW_LOGI( "shell", "[MREC] stopped: %s", describe().toStdString().c_str() );
    armed_.clear();
    for( PortIn &p : ports_ ) p.msgs.clear();
}

void SMidiRecorder::pollFinal_()
{
    active_ = true;      // poll() is guarded on it; this IS the last tick
    poll();
    active_ = false;
}

namespace {

/// An open note-on waiting for its release.
struct OpenNote {
    int    key = 0;
    int    channel = 0;
    qint64 startFrame = 0;
    double velocity = 100.0;
};

}  // namespace

void SMidiRecorder::commit_()
{
    SProject *project = app_ ? app_->getCurrentProject() : nullptr;
    if( !project ) return;
    const qint64 rate = (qint64) projectRate_();
    const twTempoMap &map = project->tempoMap();

    const qint64 loopLen = cycle_ ? (qint64) ( cycleOut_ - cycleIn_ ) : 0;
    const qint64 stopAbs = (qint64) stopFrame_;

    // armed index -> pass -> the events of that pass, in the PASS WINDOW's
    // ticks. Both keys are ordered, so the commit below walks tracks in the
    // collection order and passes in time order without sorting anything.
    std::map<std::size_t, std::map<qint64, std::vector<SEvent> > > byTrackPass;

    // The pass a wrap-counted project frame belongs to, and where that pass
    // starts. ARITHMETIC, never wrap detection (design D7's rule, restated for
    // events): the mapping is linear in host time, so the pass is a division.
    auto passOf = [&]( qint64 f ) -> qint64 {
        if( !cycle_ || loopLen <= 0 ) return 0;
        if( f < (qint64) cycleIn_ ) return 0;
        return ( f - (qint64) cycleIn_ ) / loopLen;
    };
    auto passStart = [&]( qint64 pass ) -> qint64 {
        if( !cycle_ || loopLen <= 0 ) return (qint64) recordStart_;
        return (qint64) cycleIn_ + pass * loopLen;
    };
    auto passEnd = [&]( qint64 pass ) -> qint64 {
        if( !cycle_ || loopLen <= 0 ) return std::max( stopAbs, (qint64) recordStart_ );
        return passStart( pass ) + loopLen;
    };
    // WHERE THE PASS IS PLACED, which is NOT where it started. `passStart` is
    // wrap-counted and therefore unbounded (pass 2 of a 2 s cycle starts at
    // 192000); the timeline position every pass lands on is the LOOP START, so
    // that pass 2 stacks a take on pass 1's column instead of appearing three
    // loops further to the right. Same rule SAudioRecorder's segment planner
    // applies with its modulo - the split here is already at the boundaries,
    // so the modulo is a constant.
    auto passPlacement = [&]( qint64 ) -> qint64 {
        return ( cycle_ && loopLen > 0 ) ? (qint64) cycleIn_
                                         : (qint64) recordStart_;
    };

    for( std::size_t ai = 0; ai < armed_.size(); ++ai ) {
        const Armed &a = armed_[ai];
        if( a.portIndex < 0 || a.portIndex >= (int) ports_.size() ) continue;
        const PortIn &port = ports_[(std::size_t) a.portIndex];

        std::vector<OpenNote> open;
        auto emit_ = [&]( qint64 startF, qint64 endF, int key, int ch,
                          double vel, double relVel ) {
            qint64 pass = passOf( startF );
            qint64 ps   = passStart( pass );
            qint64 pe   = passEnd( pass );
            if( startF < ps ) { startF = ps; ++clamped_; }   // clamp, never drop
            if( startF >= pe ) return;
            if( endF > pe ) endF = pe;                       // held across a wrap
            if( endF < startF ) endF = startF;
            SEvent e;
            e.kind    = twEventKind::NoteOn;
            e.channel = ch;
            e.key     = key;
            e.value   = vel;
            e.value2  = relVel;
            e.t   = map.framesToTickLen( startF - ps, (int) rate ).ticks().floorToInt();
            e.dur = map.framesToTickLen( endF - startF, (int) rate ).ticks().floorToInt();
            byTrackPass[ai][pass].push_back( e );
        };

        for( const Msg &m : port.msgs ) {
            if( m.size < 2 ) continue;
            const int status = m.bytes[0] & 0xF0;
            const int ch     = m.bytes[0] & 0x0F;
            if( m.bytes[0] >= 0xF0 ) continue;      // system / sysex: not P8b
            if( a.channel >= 0 && ch != a.channel ) continue;

            // THE MAPPING, design D6, verbatim: the project frame that was
            // being HEARD when this byte arrived, minus the port's own
            // correction (POSITIVE ms = EARLIER).
            const qint64 f = clock_.frameAtHostNs( m.hostNs, rate )
                             - port.offsetProj;

            if( status == 0x90 && m.size >= 3 && m.bytes[2] > 0 ) {
                open.push_back( { (int) m.bytes[1], ch, f, (double) m.bytes[2] } );
                continue;
            }
            if( status == 0x80 || ( status == 0x90 && m.size >= 3
                                    && m.bytes[2] == 0 ) ) {
                const int key = (int) m.bytes[1];
                // The LAST matching note-on: a re-attack of a held key before
                // its release is two notes, and the release ends the newer one.
                for( int i = (int) open.size() - 1; i >= 0; --i ) {
                    if( open[(std::size_t) i].key != key
                        || open[(std::size_t) i].channel != ch ) continue;
                    const OpenNote on = open[(std::size_t) i];
                    open.erase( open.begin() + i );
                    emit_( on.startFrame, f, key, ch, on.velocity,
                           m.size >= 3 ? (double) m.bytes[2] : 64.0 );
                    break;
                }
                continue;
            }

            // Everything else that is a channel-voice message is kept as it
            // was played: CC (sustain included), bend, program, pressure.
            SEvent e;
            e.channel = ch;
            switch( status ) {
            case 0xA0: e.kind = twEventKind::PolyPressure;
                       e.key = (int) m.bytes[1];
                       e.value = m.size >= 3 ? (double) m.bytes[2] : 0.0; break;
            case 0xB0: e.kind = twEventKind::ControlChange;
                       e.paramId = (quint32) m.bytes[1];
                       e.value = m.size >= 3 ? (double) m.bytes[2] : 0.0; break;
            case 0xC0: e.kind = twEventKind::ProgramChange;
                       e.value = (double) m.bytes[1]; break;
            case 0xD0: e.kind = twEventKind::ChannelPressure;
                       e.value = (double) m.bytes[1]; break;
            case 0xE0: e.kind = twEventKind::PitchBend;
                       e.value = m.size >= 3
                           ? (double) ( ( (int) m.bytes[2] << 7 )
                                        | (int) m.bytes[1] ) - 8192.0
                           : 0.0; break;
            default: continue;
            }
            const qint64 pass = passOf( f );
            qint64 at = f - passStart( pass );
            if( at < 0 ) at = 0;
            if( f >= passEnd( pass ) ) continue;
            e.t = map.framesToTickLen( at, (int) rate ).ticks().floorToInt();
            byTrackPass[ai][pass].push_back( e );
        }

        // A note still held at the stop is closed THERE. A recording with an
        // unterminated note is not a recording.
        for( const OpenNote &on : open )
            emit_( on.startFrame, std::max( stopAbs, on.startFrame ), on.key,
                   on.channel, on.velocity, 64.0 );
    }

    // ONE UNDO STEP for the whole take: every track, every pass, plus the
    // input quantise each placement folds in. Loop record is one
    // place-midi-recording per pass, exactly as loop AUDIO record is one
    // place-recording per pass.
    QUndoStack *undo = app_->actionHistory() ? app_->actionHistory()->undoStack()
                                             : nullptr;
    int placements = 0, notes = 0, others = 0, passes = 0;
    std::vector<qint64> seenPasses;
    if( undo ) undo->beginMacro( QStringLiteral( "MIDI Recording" ) );
    for( std::size_t ai = 0; ai < armed_.size(); ++ai ) {
        auto it = byTrackPass.find( ai );
        if( it == byTrackPass.end() ) continue;
        for( auto &pp : it->second ) {
            const qint64 pass = pp.first;
            std::vector<SEvent> &ev = pp.second;
            if( ev.empty() ) continue;
            std::stable_sort( ev.begin(), ev.end() );
            const qint64 ps  = passStart( pass );
            const qint64 pe  = passEnd( pass );
            const qint64 len = map.framesToTickLen( std::max<qint64>( pe - ps, 1 ),
                                                    (int) rate ).ticks().floorToInt();
            for( const SEvent &e : ev )
                ( e.kind == twEventKind::NoteOn ? notes : others ) += 1;
            app_->submitAction( new SPlaceMidiRecordingAction(
                armed_[ai].path, (offset_t) passPlacement( pass ), len, ev,
                modeToString( mode_ ), quantize_,
                QStringLiteral( "MIDI Recording" ) ) );
            ++placements;
            if( std::find( seenPasses.begin(), seenPasses.end(), pass )
                == seenPasses.end() ) {
                seenPasses.push_back( pass );
                ++passes;
            }
        }
    }
    if( undo ) undo->endMacro();

    lastPlacements_ = placements;
    lastNotes_      = notes;
    lastEvents_     = others;
    lastPassCount_  = passes;
    lastTracks_     = (int) armed_.size();

    for( const Armed &a : armed_ )
        if( a.track ) a.track->setArmedForRecording( false );
}

QString SMidiRecorder::describe() const
{
    return QStringLiteral( "rec=%1 start=%2 stop=%3 tracks=%4 ports=%5 mode=%6 "
                           "quantize=%7 msgs=%8 placements=%9 passes=%10 "
                           "notes=%11 events=%12 clamped=%13 anchored=%14" )
        .arg( active_ ? "on" : "off" )
        .arg( (qlonglong) recordStart_ )
        .arg( (qlonglong) stopFrame_ )
        .arg( active_ ? (int) armed_.size() : lastTracks_ )
        .arg( (int) ports_.size() )
        .arg( modeToString( mode_ ) )
        .arg( quantize_ )
        .arg( captured_ )
        .arg( lastPlacements_ )
        .arg( lastPassCount_ )
        .arg( lastNotes_ )
        .arg( lastEvents_ )
        .arg( clamped_ )
        .arg( clock_.hasAnchor() ? 1 : 0 );
}
