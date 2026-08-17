#include "app/testkit/smidiintestactions.h"

#include <algorithm>
#include <cstdlib>
#include <memory>

#include <QCoreApplication>
#include <QDebug>
#include <QDomElement>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringList>
#include <QTextStream>
#include <QThread>

#include "app/actions/sactionregistry.h"
#include "app/shell/sapplication.h"

#include "tw/devices/capture_midi.h"
#include "tw/devices/midi_out_scheduler.h"
#include "tw/events/twevent.h"
#include "tw/events/tweventseq.h"
#include "tw/events/twsmf.h"

namespace smidiin {
namespace {
qint64 g_lastInjectedHostNs = 0;
}
qint64 lastInjectedHostNs() { return g_lastInjectedHostNs; }
void   setLastInjectedHostNs( qint64 ns ) { g_lastInjectedHostNs = ns; }
}  // namespace smidiin

namespace {

// The MIDI backend this process is actually using. Read from the environment
// for the same reason smidiouttestactions.cpp does: constructing a throwaway
// port to ask it would register ITSELF as the live capture port.
QString midiBackendName()
{
    const char *env = std::getenv( "SMARAGD_MIDI_BACKEND" );
    QString v = env ? QString::fromLatin1( env ).trimmed().toLower() : QString();
    return v.isEmpty() ? QStringLiteral( "default" ) : v;
}

// The port to inject on.
//
// `active()` is whichever CaptureMidiInput was constructed last — the app's,
// once it opens one (proposal 21 L3a). Nothing does today, so the first call
// mints one for the process and opens it: a script must not have to wait for a
// consumer to exist before it can describe an input performance, and this way
// the verbs' own behaviour is the same before and after that consumer lands.
audio::CaptureMidiInput *injectionPort( QString &why )
{
    if( midiBackendName() != QStringLiteral( "capture" ) ) {
        why = QString( "the MIDI backend is '%1', not 'capture' - there is no "
                       "capture input to inject into. A --test-case run "
                       "defaults to capture; something set SMARAGD_MIDI_BACKEND." )
                  .arg( midiBackendName() );
        return nullptr;
    }

    if( audio::CaptureMidiInput *live = audio::CaptureMidiInput::active() )
        return live;

    static std::unique_ptr<audio::CaptureMidiInput> owned;
    if( !owned ) {
        owned = std::make_unique<audio::CaptureMidiInput>();
        owned->open( "capture" );
    }
    return owned.get();
}

// Pump the event loop until `predicate` holds or the budget runs out. Every
// clock a case cares about — the locator, the meters, the MIDI-out pump — only
// advances while events are processed, so a bare sleep here would freeze the
// very thing being waited for.
template <typename Fn>
bool pumpUntil( Fn predicate, int timeoutMs )
{
    QElapsedTimer t;
    t.start();
    while( !predicate() ) {
        if( t.elapsed() > timeoutMs ) return false;
        QCoreApplication::processEvents();
        QThread::msleep( 1 );
    }
    return true;
}

// Wait until wall clock reaches `dueNs`, pumping. Sub-millisecond precision is
// not claimed: what the pacing has to be is HONEST (real time, on the one
// steady clock), not exact.
void waitUntilNs( qint64 dueNs )
{
    while( audio::MidiOutScheduler::hostNowNs() < dueNs ) {
        QCoreApplication::processEvents();
        const qint64 left = dueNs - audio::MidiOutScheduler::hostNowNs();
        if( left > 2000000 ) QThread::msleep( 1 );
    }
}

// "90 3C 64" / "903C64" -> bytes. Empty on a malformed string.
QVector<quint8> parseHexBytes( const QString &s )
{
    QVector<quint8> out;
    QString compact = s;
    compact.remove( QLatin1Char( ' ' ) ).remove( QLatin1Char( ',' ) );
    if( compact.size() % 2 != 0 ) return {};
    for( int i = 0; i + 1 < compact.size(); i += 2 ) {
        bool ok = false;
        const uint v = compact.mid( i, 2 ).toUInt( &ok, 16 );
        if( !ok || v > 0xFF ) return {};
        out.push_back( (quint8) v );
    }
    return out;
}

quint8 clamp7( int v ) { return (quint8) qBound( 0, v, 127 ); }

// The field form -> bytes. Returns empty for an unknown kind.
QVector<quint8> buildMessage( const QString &kind, int channel, int key,
                              int cc, int value )
{
    const quint8 ch = (quint8) qBound( 0, channel, 15 );
    if( kind == QLatin1String( "noteon" ) )
        return { (quint8) ( 0x90 | ch ), clamp7( key ), clamp7( value ) };
    if( kind == QLatin1String( "noteoff" ) )
        return { (quint8) ( 0x80 | ch ), clamp7( key ), clamp7( value ) };
    if( kind == QLatin1String( "polypressure" ) )
        return { (quint8) ( 0xA0 | ch ), clamp7( key ), clamp7( value ) };
    if( kind == QLatin1String( "cc" ) )
        return { (quint8) ( 0xB0 | ch ), clamp7( cc >= 0 ? cc : key ),
                 clamp7( value ) };
    if( kind == QLatin1String( "programchange" ) )
        return { (quint8) ( 0xC0 | ch ), clamp7( value ) };
    if( kind == QLatin1String( "channelpressure" ) )
        return { (quint8) ( 0xD0 | ch ), clamp7( value ) };
    if( kind == QLatin1String( "pitchbend" ) ) {
        const int v = qBound( 0, value, 16383 );
        return { (quint8) ( 0xE0 | ch ), (quint8) ( v & 0x7F ),
                 (quint8) ( ( v >> 7 ) & 0x7F ) };
    }
    return {};
}

// One scheduled message of a replay.
struct Timed {
    qint64          offsetNs = 0;    // from the start of the performance
    QVector<quint8> bytes;
};

bool loadTextLog( const QString &path, QVector<Timed> &out, QString &err )
{
    QFile f( path );
    if( !f.open( QIODevice::ReadOnly | QIODevice::Text ) ) {
        err = QString( "cannot open '%1'" ).arg( path );
        return false;
    }
    QTextStream in( &f );
    int lineNo = 0;
    while( !in.atEnd() ) {
        ++lineNo;
        const QString line = in.readLine().trimmed();
        if( line.isEmpty() || line.startsWith( QLatin1Char( '#' ) ) ) continue;
        const QStringList parts =
            line.split( QRegularExpression( "\\s+" ), Qt::SkipEmptyParts );
        if( parts.size() < 2 ) {
            err = QString( "line %1: expected '<ms> <hex bytes>'" ).arg( lineNo );
            return false;
        }
        bool ok = false;
        const double ms = parts[0].toDouble( &ok );
        if( !ok ) {
            err = QString( "line %1: '%2' is not a millisecond offset" )
                      .arg( lineNo ).arg( parts[0] );
            return false;
        }
        QVector<quint8> bytes;
        for( int i = 1; i < parts.size(); ++i ) {
            const QVector<quint8> b = parseHexBytes( parts[i] );
            if( b.isEmpty() ) {
                err = QString( "line %1: '%2' is not a hex byte" )
                          .arg( lineNo ).arg( parts[i] );
                return false;
            }
            bytes += b;
        }
        out.push_back( { (qint64) ( ms * 1e6 ), bytes } );
    }
    return true;
}

// A .mid -> timed messages, using the FILE's own PPQ and tempo metas.
//
// A sequence stores a note WITH its length (there is no note-off in any table
// — CLAUDE.md, events inv. 8-9), so the note-offs a device would have sent are
// synthesised here from `duration`. That is the same thing a `collect` does for
// the render path; a replay is just the wire version of it.
bool loadSmf( const QString &path, int trackIndex, QVector<Timed> &out,
              QString &err )
{
    twSmfFile file;
    std::string serr;
    if( !twSmf::readFile( path.toStdString(), file, &serr ) ) {
        err = QString( "cannot read '%1': %2" ).arg( path )
                  .arg( QString::fromStdString( serr ) );
        return false;
    }
    if( file.tracks.empty() ) { err = "the file holds no track"; return false; }
    if( file.ppq <= 0 ) { err = "the file has a non-positive PPQ"; return false; }

    // Tempo changes, from every track (a type 1 file puts them on track 0, but
    // nothing forbids otherwise), as {tick, µs per quarter}.
    QVector<QPair<qint64, double>> tempo;
    for( const twSmfTrack &t : file.tracks ) {
        if( !t.events ) continue;
        for( const twEvent &e : t.events->events() )
            if( e.kind == twEventKind::Tempo && e.value > 0.0 )
                tempo.push_back( { e.time, e.value } );
    }
    std::sort( tempo.begin(), tempo.end(),
               []( const QPair<qint64, double> &a,
                   const QPair<qint64, double> &b ) { return a.first < b.first; } );

    // Tick -> nanoseconds, integrating the tempo map. O(events x tempi), which
    // for a test fixture is nothing.
    const double ppq = (double) file.ppq;
    auto tickToNs = [&]( qint64 tick ) {
        double ns = 0.0;
        double usPerQuarter = 500000.0;          // 120 BPM until told otherwise
        qint64 last = 0;
        for( const auto &tp : tempo ) {
            if( tp.first >= tick ) break;
            ns += ( (double) ( tp.first - last ) / ppq ) * usPerQuarter * 1000.0;
            usPerQuarter = tp.second;
            last = tp.first;
        }
        ns += ( (double) ( tick - last ) / ppq ) * usPerQuarter * 1000.0;
        return (qint64) ns;
    };

    for( int ti = 0; ti < (int) file.tracks.size(); ++ti ) {
        if( trackIndex >= 0 && ti != trackIndex ) continue;
        const twSmfTrack &t = file.tracks[(size_t) ti];
        if( !t.events ) continue;
        for( const twEvent &e : t.events->events() ) {
            const int ch = e.channel >= 0 ? e.channel : 0;
            switch( e.kind ) {
            case twEventKind::NoteOn: {
                out.push_back( { tickToNs( e.time ),
                                 buildMessage( "noteon", ch, e.key, -1,
                                               (int) ( e.value * 127.0 + 0.5 ) ) } );
                if( e.duration > 0 )
                    out.push_back( { tickToNs( e.time + e.duration ),
                                     buildMessage( "noteoff", ch, e.key, -1, 0 ) } );
                break;
            }
            case twEventKind::NoteOff:
                out.push_back( { tickToNs( e.time ),
                                 buildMessage( "noteoff", ch, e.key, -1, 0 ) } );
                break;
            case twEventKind::ControlChange:
                out.push_back( { tickToNs( e.time ),
                                 buildMessage( "cc", ch, -1, (int) e.paramId,
                                               (int) ( e.value * 127.0 + 0.5 ) ) } );
                break;
            case twEventKind::PitchBend:
                out.push_back( { tickToNs( e.time ),
                                 buildMessage( "pitchbend", ch, -1, -1,
                                               (int) ( ( e.value + 1.0 ) * 8192.0 ) ) } );
                break;
            case twEventKind::ProgramChange:
                out.push_back( { tickToNs( e.time ),
                                 buildMessage( "programchange", ch, -1, -1,
                                               (int) e.value ) } );
                break;
            default:
                break;              // metadata: nothing goes on the wire
            }
        }
    }

    std::stable_sort( out.begin(), out.end(),
                      []( const Timed &a, const Timed &b ) {
                          return a.offsetNs < b.offsetNs;
                      } );
    return true;
}

// The shared "hold until the playhead is there" step. Rejects rather than
// ignores when the transport is stopped: a project FRAME is a position on a
// moving playhead, and there is no such thing while it is not moving (design
// D2). Silently injecting now would make a case pass while measuring nothing.
bool holdForFrame( qint64 frame, int timeoutMs, QString &why )
{
    SApplication &app = SApplication::app();
    if( !app.isPlaying() ) {
        why = "atFrame/startFrame is only meaningful while the transport is "
              "PLAYING - a project frame is a position on a moving playhead. "
              "Start playback first, or drop the attribute to inject now.";
        return false;
    }
    if( !pumpUntil( [&app, frame] {
                        return (qint64) app.getGlobalLocatorPos() >= frame;
                    },
                    timeoutMs ) ) {
        why = QString( "the playhead did not reach frame %1 within %2 ms "
                       "(it is at %3)" )
                  .arg( frame ).arg( timeoutMs )
                  .arg( (qulonglong) app.getGlobalLocatorPos() );
        return false;
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------- midi-in-event

QStringList SMidiInEventAction::knownAttributes() const
{
    return { QStringLiteral( "kind" ),      QStringLiteral( "key" ),
             QStringLiteral( "velocity" ),  QStringLiteral( "channel" ),
             QStringLiteral( "cc" ),        QStringLiteral( "bytes" ),
             QStringLiteral( "atFrame" ),   QStringLiteral( "timeoutMs" ) };
}

SApplyResult SMidiInEventAction::apply( SProject * )
{
    QString why;
    audio::CaptureMidiInput *port = injectionPort( why );
    if( !port ) {
        qWarning() << "midi-in-event: REJECTED -" << why;
        return { false, nullptr };
    }

    QVector<quint8> msg;
    if( !bytes_.isEmpty() ) {
        msg = parseHexBytes( bytes_ );
        if( msg.isEmpty() ) {
            qWarning() << "midi-in-event: bytes='" << bytes_
                       << "' is not a hex byte string";
            return { false, nullptr };
        }
    } else {
        msg = buildMessage( kind_.trimmed().toLower(), channel_, key_, cc_,
                            velocity_ );
        if( msg.isEmpty() ) {
            qWarning() << "midi-in-event: unknown kind" << kind_;
            return { false, nullptr };
        }
    }

    if( hasAtFrame_ && !holdForFrame( atFrame_, timeoutMs_, why ) ) {
        qWarning() << "midi-in-event:" << why;
        return { false, nullptr };
    }

    const qint64 at = audio::MidiOutScheduler::hostNowNs();
    port->inject( msg.constData(), (std::size_t) msg.size(), at );
    // Recorded so assert-audio-onset can measure the lag from HERE, through
    // the AUDIO capture backend block log rather than through the live lane.
    smidiin::setLastInjectedHostNs( at );
    return { true, nullptr };
}

void SMidiInEventAction::writeXml( QDomElement &elem ) const
{
    if( !bytes_.isEmpty() ) {
        elem.setAttribute( "bytes", bytes_ );
    } else {
        elem.setAttribute( "kind", kind_ );
        elem.setAttribute( "key", QString::number( key_ ) );
        elem.setAttribute( "velocity", QString::number( velocity_ ) );
        elem.setAttribute( "channel", QString::number( channel_ ) );
        if( cc_ >= 0 ) elem.setAttribute( "cc", QString::number( cc_ ) );
    }
    if( hasAtFrame_ ) elem.setAttribute( "atFrame", QString::number( atFrame_ ) );
    elem.setAttribute( "timeoutMs", QString::number( timeoutMs_ ) );
}

bool SMidiInEventAction::readXml( const QDomElement &elem, int )
{
    kind_      = elem.attribute( "kind", "noteon" );
    key_       = elem.attribute( "key", "60" ).toInt();
    velocity_  = elem.attribute( "velocity", "100" ).toInt();
    channel_   = elem.attribute( "channel", "0" ).toInt();
    cc_        = elem.attribute( "cc", "-1" ).toInt();
    bytes_     = elem.attribute( "bytes", "" );
    hasAtFrame_ = elem.hasAttribute( "atFrame" );
    atFrame_   = elem.attribute( "atFrame", "0" ).toLongLong();
    timeoutMs_ = elem.attribute( "timeoutMs", "10000" ).toInt();
    return true;
}

// --------------------------------------------------------------- midi-in-replay

QStringList SMidiInReplayAction::knownAttributes() const
{
    return { QStringLiteral( "filePath" ),  QStringLiteral( "track" ),
             QStringLiteral( "channel" ),   QStringLiteral( "startFrame" ),
             QStringLiteral( "timeoutMs" ) };
}

SApplyResult SMidiInReplayAction::apply( SProject * )
{
    QString why;
    audio::CaptureMidiInput *port = injectionPort( why );
    if( !port ) {
        qWarning() << "midi-in-replay: REJECTED -" << why;
        return { false, nullptr };
    }
    if( filePath_.isEmpty() ) {
        qWarning() << "midi-in-replay: filePath is required";
        return { false, nullptr };
    }

    QVector<Timed> perf;
    const QString suffix = QFileInfo( filePath_ ).suffix().toLower();
    const bool isSmf = ( suffix == QLatin1String( "mid" ) ||
                         suffix == QLatin1String( "midi" ) );
    const bool loaded = isSmf ? loadSmf( filePath_, track_, perf, why )
                              : loadTextLog( filePath_, perf, why );
    if( !loaded ) {
        qWarning() << "midi-in-replay:" << why;
        return { false, nullptr };
    }
    if( perf.isEmpty() ) {
        qWarning() << "midi-in-replay:" << filePath_ << "holds no message";
        return { false, nullptr };
    }

    if( channel_ >= 0 ) {
        for( Timed &t : perf )
            if( !t.bytes.isEmpty() && ( t.bytes[0] & 0xF0 ) != 0xF0 )
                t.bytes[0] = (quint8) ( ( t.bytes[0] & 0xF0 ) |
                                        ( channel_ & 0x0F ) );
    }

    if( hasStartFrame_ && !holdForFrame( startFrame_, timeoutMs_, why ) ) {
        qWarning() << "midi-in-replay:" << why;
        return { false, nullptr };
    }

    // REAL TIME, on the one steady clock. The offsets are relative to the
    // instant the performance starts, so a late message does not drag the ones
    // behind it (the deadlines are absolute, not cumulative).
    const qint64 t0 = audio::MidiOutScheduler::hostNowNs();
    for( const Timed &t : perf ) {
        waitUntilNs( t0 + t.offsetNs );
        const qint64 at = audio::MidiOutScheduler::hostNowNs();
        port->inject( t.bytes.constData(), (std::size_t) t.bytes.size(), at );
        smidiin::setLastInjectedHostNs( at );
    }
    return { true, nullptr };
}

void SMidiInReplayAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "filePath", filePath_ );
    elem.setAttribute( "track", QString::number( track_ ) );
    elem.setAttribute( "channel", QString::number( channel_ ) );
    if( hasStartFrame_ )
        elem.setAttribute( "startFrame", QString::number( startFrame_ ) );
    elem.setAttribute( "timeoutMs", QString::number( timeoutMs_ ) );
}

bool SMidiInReplayAction::readXml( const QDomElement &elem, int )
{
    filePath_ = elem.attribute( "filePath", "" );
    track_    = elem.attribute( "track", "-1" ).toInt();
    channel_  = elem.attribute( "channel", "-1" ).toInt();
    hasStartFrame_ = elem.hasAttribute( "startFrame" );
    startFrame_ = elem.attribute( "startFrame", "0" ).toLongLong();
    timeoutMs_  = elem.attribute( "timeoutMs", "10000" ).toInt();
    return true;
}

static const bool s_reg_midi_in_verbs = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "midi-in-event" ),
        []{ return new SMidiInEventAction; } ),
    SActionRegistry::instance().registerType(
        QStringLiteral( "midi-in-replay" ),
        []{ return new SMidiInReplayAction; } ),
    true );
