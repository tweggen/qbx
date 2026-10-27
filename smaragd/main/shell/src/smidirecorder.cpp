#include "app/shell/smidirecorder.h"

#include <QTime>

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
#include "app/objects/track/srecordmode.h"
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
        if( !child->isLane() ) continue;
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

bool SMidiRecorder::punchRegion_( offset_t &in, offset_t &out ) const
{
    // ONE RANGE, TWO READINGS, and the cycle flag decides which -- word for
    // word `SAudioRecorder::punchRegion_`, because one press drives both
    // recorders and they may not disagree about what the markers mean.
    offset_t a = 0, b = 0;
    if( cycleRegion_( a, b ) ) return false;
    SProject *p = app_ ? app_->getCurrentProject() : nullptr;
    if( !p ) return false;
    if( !p->prop( SProjectProps::RangeValid, false ).toBool() ) return false;
    in  = (offset_t) p->prop( SProjectProps::RangeStart, (qulonglong) 0 ).toULongLong();
    out = (offset_t) p->prop( SProjectProps::RangeEnd,   (qulonglong) 0 ).toULongLong();
    return out > in;
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
    // check_root_resolution: suspect QBX-247 -- this reason was WRONG: arm-track resolves splacements::rootNamed(), so a track in an arrangement CAN be armed and is then never collected, and start() refuses with "no armed tracks"
    collectArmedMidi( project->masterRoot(), QList<int>(), found );
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

void SMidiRecorder::beginPreCapture()
{
    if( active_ || preCapture_ ) return;
    if( !app_ || !app_->getCurrentProject() ) return;
    // The armed set is collected HERE so the sinks exist while the click runs.
    // `start()` collects it again - arming can change during a count-in - and
    // the buffer is carried across that by port NAME.
    if( !collectArmed_() ) return;

    // DRAINED ONCE, and discarded: this is the instant the button went down,
    // so whatever is in the ring is genuinely from before the gesture. Same
    // reasoning the drain in `start()` used to carry for every take, and the
    // same comment applies - popping is the consumer side and always safe,
    // while `MidiInRing::clear()` is not.
    for( PortIn &p : ports_ ) {
        audio::MidiInMessage m;
        int dropped = 0;
        while( p.sink && p.sink->pop( m ) ) ++dropped;
        p.msgs.clear();
        if( dropped )
            TW_LOGD( "shell", "[MREC] pre-capture dropped %d message(s) from "
                              "before the button on '%s'",
                     dropped, p.name.toStdString().c_str() );
    }

    preCapture_ = true;
    timer_->start();
    TW_LOGI( "shell", "[MREC] pre-capture on %u port(s): a preamble is in "
                      "front of the take",
             (unsigned) ports_.size() );
}

void SMidiRecorder::cancelPreCapture()
{
    if( !preCapture_ ) return;
    preCapture_ = false;
    if( !active_ ) timer_->stop();
    armed_.clear();
    ports_.clear();
    TW_LOGI( "shell", "[MREC] pre-capture cancelled with the preamble" );
}

bool SMidiRecorder::start()
{
    if( active_ ) return true;
    error_.clear();

    // THE PRE-CAPTURE HARVEST COMES FIRST, because `collectArmed_()` below
    // CLEARS `ports_` and the buffer lives in it (QBX-213). One last drain
    // picks up the final tick's worth, and the messages are then carried over
    // by port NAME - an index would not survive a re-collect.
    std::vector<PreMsgs> harvested;
    const bool wasPreCapturing = preCapture_;
    if( wasPreCapturing ) {
        drainRings_();
        for( PortIn &p : ports_ )
            if( !p.msgs.empty() ) {
                for( Msg &m : p.msgs ) m.pre = true;
                harvested.push_back( PreMsgs{ p.name, std::move( p.msgs ) } );
            }
        preCapture_ = false;
    }

    lastPassCount_ = lastPlacements_ = lastNotes_ = lastEvents_ = 0;
    lastTracks_ = 0;
    captured_ = clamped_ = preMsgs_ = uncaught_ = 0;

    SProject *project = app_ ? app_->getCurrentProject() : nullptr;
    if( !project ) {
        error_ = QStringLiteral( "no project" );
        if( wasPreCapturing ) timer_->stop();
        return false;
    }
    if( !collectArmed_() ) {
        error_ = QStringLiteral( "no armed MIDI tracks" );
        if( wasPreCapturing ) timer_->stop();
        return false;
    }

    recordStart_ = app_->getGlobalLocatorPos();
    cycle_       = cycleRegion_( cycleIn_, cycleOut_ );
    punch_       = punchRegion_( punchIn_, punchOut_ );
    stopping_    = false;

    // WHAT THE PREAMBLE COLLECTED IS THE TAKE'S, and is put back on the ports
    // it came from (QBX-213). This is the retrospective-capture buffer
    // `SMidiInputHub::recorderSink()` was already keeping the ring alive for -
    // "a recorder that acquired and released per pass would lose whatever
    // arrived between passes" - and which this very function used to throw
    // away, three lines further down, for the reason below.
    if( wasPreCapturing ) {
        for( PortIn &p : ports_ )
            for( PreMsgs &h : harvested )
                if( h.port == p.name ) {
                    p.msgs = std::move( h.msgs );
                    preMsgs_ += (int) p.msgs.size();
                    captured_ += (int) p.msgs.size();
                    break;
                }
        if( preMsgs_ )
            TW_LOGI( "shell", "[MREC] %d message(s) came from the preamble",
                     preMsgs_ );
    } else {
        // NO PREAMBLE: the button and the take are the same instant, so the
        // rings are DRAINED, never cleared - `MidiInRing::clear()` is only
        // safe while the producer is known to be idle, and a performer's
        // finger is not. Popping is the consumer side and is always safe.
        // Whatever was in there belongs to before the button was pressed.
        //
        // THIS IS STILL THE RULE, and a case gates it: a key pressed before
        // the record button must not appear inside the take. QBX-213 did not
        // widen that - it noticed that with a count-in the button is N bars
        // EARLIER than this line, so the click's notes are not "before the
        // button" at all, and `beginPreCapture()` is where the drain belongs
        // for those takes.
        for( PortIn &p : ports_ ) {
            audio::MidiInMessage m;
            int dropped = 0;
            while( p.sink && p.sink->pop( m ) ) ++dropped;
            p.msgs.clear();
            if( dropped )
                TW_LOGD( "shell", "[MREC] dropped %d stale message(s) on '%s'",
                         dropped, p.name.toStdString().c_str() );
        }
    }

    // Read ONCE: a settings change mid-take must not make the commit
    // inconsistent with what was captured.
    //
    // THE PROJECT IS THE AUTHORITY, not `SOpt::MidiRecordMode` (plan 53 P3).
    // This read the INI directly, in proposal 21 L4's own three-value
    // vocabulary, which made the MIDI half of a press able to disagree with
    // the audio half about the mode of the same gesture and left
    // `takes-split` unreachable for MIDI. `srecmode` is the one resolver now,
    // the same one `SAudioRecorder` asks; the INI key still seeds a brand-new
    // project (`srecmodeseed.cpp`), which is how a user's preference reaches a
    // project at all.
    mode_ = srecmode::projectMode( app_->getCurrentProject() );
    quantize_ = SSettings::instance().value(
        SOpt::MidiRecordQuantize,
        SOpt::def( SOpt::MidiRecordQuantize ) ).toString();
    // THE CATCH RANGE (QBX-213), in frames, read once with its neighbours and
    // for the same reason: a change made mid-take must not leave the commit
    // disagreeing with the capture. Clamped to 0..1000 ms here rather than
    // trusted, because it comes out of the INI.
    {
        int ms = SSettings::instance().value(
            SOpt::MidiRecordCatchRangeMs,
            SOpt::def( SOpt::MidiRecordCatchRangeMs ) ).toInt();
        if( ms < 0 )    ms = 0;
        if( ms > 1000 ) ms = 1000;
        catchFrames_ = (qint64) ( (double) ms * projectRate_() / 1000.0 + 0.5 );
    }

    lastPos_  = (qint64) recordStart_;
    playIter_ = 0;
    clock_.beginRun( (qint64) recordStart_, app_->locatorPublishSeq() );

    active_ = true;
    timer_->start();
    TW_LOGI( "shell", "[MREC] started at %lld: %u track(s) on %u port(s), "
                      "mode=%s quantize=%s%s",
             (long long) recordStart_, (unsigned) armed_.size(),
             (unsigned) ports_.size(),
             mode_.toStdString().c_str(),
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
    if( !active_ ) {
        // THE PREAMBLE'S TICK (QBX-213). Take the messages off the rings and
        // nothing else: there is no run on the clock yet, no playhead to
        // offer it and no pass to belong to. Draining here rather than only
        // at `start()` is what keeps a long count-in from overflowing a
        // 1024-slot ring - the same headroom argument the tick already makes
        // for a take.
        if( preCapture_ ) drainRings_();
        return;
    }
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

    // PUNCH-OUT, LAST, and on this class's own tick: the take ends by itself
    // once the playhead passes the out point, exactly as
    // SAudioRecorder::poll() ends its own. It has to happen HERE rather than
    // at the `record-stop`, because the audio half's punch-out stops the
    // TRANSPORT and this recorder's clock is only valid while the RT is
    // publishing -- see the note on punchRegion_ for what the take looked like
    // before: nothing placed at all.
    if( punch_ && !stopping_ && pos >= (qint64) punchOut_ ) {
        TW_LOGI( "shell", "[MREC] punch-out at %lld", (long long) punchOut_ );
        stop();
    }
}

// -------------------------------------------------------------------- stop --

void SMidiRecorder::stop()
{
    // A STOP THAT ARRIVES WHILE ONLY THE PREAMBLE'S BUFFER IS FILLING ends
    // that too (QBX-213). Nothing was recorded, so nothing is kept - the same
    // answer `cancelRecordPreamble()` gives, given here as well so that
    // "safe to call when not recording" does not quietly mean "and leaves the
    // pre-capture running". No caller takes this route today, because
    // `SApplication::stopRecording()` cancels the preamble before it reaches
    // either recorder; the invariant should not depend on that staying true.
    if( !active_ ) { cancelPreCapture(); return; }
    if( stopping_ ) return;
    stopping_ = true;           // pollFinal_ re-enters poll(), which punches out
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
    stopping_ = false;
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
    /// Was the note-on captured during the PREAMBLE (QBX-213)? Only such a
    /// note is measured against the catch range.
    bool   pre = false;
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
    // UNDER A PUNCH THE WINDOW IS THE MARKERS, exactly (plan 53 P3). Not
    // `stopAbs`: the take ends at the out point however late this tick is, and
    // the in point is where it began if the locator was before it. That is
    // what `record_punch` already pins for audio, and the two halves of one
    // press must agree on the window or the band's take and the keyboard's
    // take are over different bars.
    const qint64 punchA = punch_
        ? std::max<qint64>( (qint64) recordStart_, (qint64) punchIn_ )
        : 0;
    auto passStart = [&]( qint64 pass ) -> qint64 {
        if( cycle_ && loopLen > 0 ) return (qint64) cycleIn_ + pass * loopLen;
        return punch_ ? punchA : (qint64) recordStart_;
    };
    auto passEnd = [&]( qint64 pass ) -> qint64 {
        if( cycle_ && loopLen > 0 ) return passStart( pass ) + loopLen;
        if( punch_ ) return (qint64) punchOut_;
        return std::max( stopAbs, (qint64) recordStart_ );
    };
    // WHERE THE PASS IS PLACED, which is NOT where it started. `passStart` is
    // wrap-counted and therefore unbounded (pass 2 of a 2 s cycle starts at
    // 192000); the timeline position every pass lands on is the LOOP START, so
    // that pass 2 stacks a take on pass 1's column instead of appearing three
    // loops further to the right. Same rule SAudioRecorder's segment planner
    // applies with its modulo - the split here is already at the boundaries,
    // so the modulo is a constant.
    auto passPlacement = [&]( qint64 ) -> qint64 {
        if( cycle_ && loopLen > 0 ) return (qint64) cycleIn_;
        return punch_ ? punchA : (qint64) recordStart_;
    };

    for( std::size_t ai = 0; ai < armed_.size(); ++ai ) {
        const Armed &a = armed_[ai];
        if( a.portIndex < 0 || a.portIndex >= (int) ports_.size() ) continue;
        const PortIn &port = ports_[(std::size_t) a.portIndex];

        std::vector<OpenNote> open;
        auto emit_ = [&]( qint64 startF, qint64 endF, int key, int ch,
                          double vel, double relVel, bool pre ) {
            qint64 pass = passOf( startF );
            qint64 ps   = passStart( pass );
            qint64 pe   = passEnd( pass );
            if( startF < ps ) {
                // THE CATCH RANGE, and only for a note the PREAMBLE captured
                // (QBX-213). A performer plays to the count-in click and
                // anticipates the downbeat; that note is the take's, and it is
                // clamped to the start while KEEPING ITS END - shortened,
                // never moved.
                //
                // TWO CONDITIONS, and the second is not a detail. A caught
                // note must have begun within the range AND must still be
                // SOUNDING when the take begins:
                //
                //   - outside the range it was a different note, played
                //     during the click rather than at the top of the take,
                //     and mashing it onto beat 1 is worse than dropping it;
                //   - a note that began AND ENDED during the click was never
                //     an anticipation of the downbeat at all. Admitting it
                //     would clamp both its ends to the same frame and store a
                //     note of ZERO LENGTH at the take's start, which is junk
                //     the performer never played. "Anticipated the downbeat"
                //     means the finger was still down when the downbeat came,
                //     and that is exactly `endF > ps`.
                //
                // A note captured INSIDE the take keeps the old, unbounded
                // clamp: that one is latency compensation, not anticipation,
                // and a punch-in's lead-in has always clamped to the in point.
                if( pre && ( ps - startF > catchFrames_ || endF <= ps ) ) {
                    ++uncaught_;
                    return;
                }
                startF = ps;
                ++clamped_;
            }
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
                open.push_back( { (int) m.bytes[1], ch, f,
                                  (double) m.bytes[2], m.pre } );
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
                           m.size >= 3 ? (double) m.bytes[2] : 64.0, on.pre );
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
            // A CONTROLLER FROM THE PREAMBLE IS KEPT WHATEVER THE CATCH
            // RANGE SAYS, and that asymmetry with notes is deliberate
            // (QBX-213). A CC, a bend or a program change is the
            // instrument's STATE, not an event: a sustain pedal pressed
            // during the count-in and still down at the downbeat belongs to
            // the take, and dropping it would record the take with the pedal
            // up while the performer has their foot on it. So the clamp to 0
            // below stays unbounded. A NOTE is an event and is bounded,
            // because a note mashed onto beat 1 is audibly wrong.
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
                   on.channel, on.velocity, 64.0, on.pre );
    }

    // ONE UNDO STEP for the whole PRESS, opened by SApplication rather than
    // here (plan 53 §5.4, QBX-165): the audio half and this one belong to the
    // same gesture, and a press that places nothing must leave the undo stack
    // untouched -- which an empty Qt macro does not.
    //
    // Loop record is still one place-midi-recording per pass, exactly as loop
    // AUDIO record is one place-recording per pass; they now share the step.
    int placements = 0, notes = 0, others = 0, passes = 0;
    std::vector<qint64> seenPasses;
    // THE TAKE'S NAME CARRIES THE WALL-CLOCK MINUTE (plan 53 §8 P4, QBX-182),
    // taken ONCE for the whole press, exactly as the audio recorder does -- so
    // the two halves of a mixed press stamp the same minute. It is decision
    // D5's origin marker, which `cleanup-takes` (P6) reads to tell a recorded
    // take from one that came from the user's arrangement; an audio-only
    // marker would make every recorded MIDI take look like arrangement.
    const QString stamp =
        QTime::currentTime().toString( QStringLiteral( "HH:mm" ) );
    // As in the audio recorder: open the scope here as well, so a commit that
    // did not come through SApplication::stopRecording() is still one step
    // (QBX-165). Re-entrant, so the common path just nests.
    app_->beginRecordCommit();
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
            // PER ARMED LANE (plan 53 §5.3, P3), exactly as the audio
            // recorder does it: the lane's own mode, else its nearest ancestor
            // folder's, else the project's. `place-midi-recording` cannot
            // resolve one itself -- `objects/midi` may not depend on
            // `objects/track`, where the folder walk lives -- so the recorder
            // submits the verb with the answer already in it.
            const QString laneMode = srecmode::resolveMode(
                // check_root_resolution: suspect QBX-247 -- the pass lands where collectArmed_ looked; QBX-247 decides which roots that is, and this must use the SAME one
                splacements::rootContainer( project ), armed_[ai].track,
                project );
            app_->submitAction( new SPlaceMidiRecordingAction(
                armed_[ai].path, (offset_t) passPlacement( pass ), len, ev,
                laneMode, quantize_,
                QStringLiteral( "MIDI Recording" ), stamp ) );
            ++placements;
            if( std::find( seenPasses.begin(), seenPasses.end(), pass )
                == seenPasses.end() ) {
                seenPasses.push_back( pass );
                ++passes;
            }
        }
    }
    app_->endRecordCommit();

    lastPlacements_ = placements;
    lastNotes_      = notes;
    lastEvents_     = others;
    lastPassCount_  = passes;
    lastTracks_     = (int) armed_.size();

    // Arm state is deliberately untouched (item e, mirrors SAudioRecorder):
    // the armed track(s) stay armed once the take is placed, ready for the
    // next one. There is no growing-clip visual on the MIDI side to clear —
    // an event clip is placed only at the stop — so nothing else here needs
    // to change.
}

QString SMidiRecorder::describe() const
{
    return QStringLiteral( "rec=%1 start=%2 stop=%3 tracks=%4 ports=%5 mode=%6 "
                           "quantize=%7 msgs=%8 placements=%9 passes=%10 "
                           "notes=%11 events=%12 clamped=%13 anchored=%14 "
                           "pre=%15 uncaught=%16 catch=%17" )
        .arg( active_ ? "on" : "off" )
        .arg( (qlonglong) recordStart_ )
        .arg( (qlonglong) stopFrame_ )
        .arg( active_ ? (int) armed_.size() : lastTracks_ )
        .arg( (int) ports_.size() )
        .arg( mode_ )
        .arg( quantize_ )
        .arg( captured_ )
        .arg( lastPlacements_ )
        .arg( lastPassCount_ )
        .arg( lastNotes_ )
        .arg( lastEvents_ )
        .arg( clamped_ )
        .arg( clock_.hasAnchor() ? 1 : 0 )
        // QBX-213: what the preamble contributed, what the catch range
        // refused, and the range itself in frames - so a case can assert the
        // mechanism and not just its result.
        .arg( preMsgs_ )
        .arg( uncaught_ )
        .arg( (qlonglong) catchFrames_ );
}
