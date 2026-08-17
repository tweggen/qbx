#include "app/shell/smidiinputhub.h"

#include "app/shell/ssettings.h"
#include "tw/core/twlog.h"
#include "tw/devices/keyboard_midi.h"

namespace {

/// The one spelling of the computer keyboard's port name.
const QString kKeyboard = QStringLiteral( "keyboard" );

}  // namespace

SMidiInputHub::SMidiInputHub()
{
    // THE PROBE FIRST, AND THE ORDER IS LOAD-BEARING. `CaptureMidiInput`
    // registers the most recently CONSTRUCTED instance as the one
    // `CaptureMidiInput::active()` returns, which is the port `midi-in-event`
    // injects into. Constructing the probe here - before any listening port -
    // means a listening port is always the newer one, so a headless case's
    // injections reach the port the live lane is draining rather than an
    // unopened probe. The same argument SMidiOutPump makes for probeOut_.
    probe_ = audio::createMidiInput();

    // THE COMPUTER KEYBOARD IS OPENED EAGERLY (design D9). It is in-process,
    // so it costs nothing, and the piano-roll dock has to be able to play it
    // whether or not any track has ever been armed - a key press with nothing
    // listening is still a key press, and `KeyboardMidiInput::active()` has to
    // find an OPEN port for it.
    auto kb = std::make_unique<Port>();
    kb->input = audio::createMidiInput( "keyboard" );
    if( kb->input ) {
        kb->fanout = std::make_unique<audio::MidiInFanout>();
        kb->input->setCallback( kb->fanout->callback() );
        kb->input->open( audio::KeyboardMidiInput::kPortId );
        kb->deviceId = kKeyboard;
        ports_[kKeyboard] = std::move( kb );
    }
}

SMidiInputHub::~SMidiInputHub()
{
    // Order: drop the CALLBACK before the fanout, or a device thread already
    // inside onMessage() would touch a destroyed one. close() is what stops
    // the thread; setCallback(nullptr) is the belt to that brace.
    for( auto &kv : ports_ ) {
        Port *p = kv.second.get();
        if( !p || !p->input ) continue;
        p->input->setCallback( nullptr );
        p->input->close();
    }
    ports_.clear();
}

QString SMidiInputHub::resolvePortId( const QString &portName ) const
{
    if( portName.isEmpty() ) return QStringLiteral( "default" );
    if( portName == kKeyboard ) return QString::fromUtf8( audio::KeyboardMidiInput::kPortId );

    const QString mapped = SSettings::instance().midiInputPortId( portName );
    if( !mapped.isEmpty() ) return mapped;

    if( probe_ ) {
        for( const audio::MidiPortInfo &p : probe_->listPorts() ) {
            const QString id   = QString::fromStdString( p.id );
            const QString name = QString::fromStdString( p.name );
            if( name.compare( portName, Qt::CaseInsensitive ) == 0
                || id.compare( portName, Qt::CaseInsensitive ) == 0 )
                return id;
        }
    }
    return portName;
}

SMidiInputHub::Port *SMidiInputHub::portFor( const QString &portName )
{
    const QString key = portName.isEmpty() ? QStringLiteral( "default" ) : portName;
    auto it = ports_.find( key );
    if( it != ports_.end() ) return it->second.get();
    if( failed_.contains( key ) ) return nullptr;

    auto port = std::make_unique<Port>();
    port->input = ( key == kKeyboard ) ? audio::createMidiInput( "keyboard" )
                                       : audio::createMidiInput();
    if( !port->input ) {
        failed_ << key;
        TW_LOGW( "shell", "[LIVE] no MIDI input backend for port '%s'",
                 key.toStdString().c_str() );
        return nullptr;
    }
    port->fanout   = std::make_unique<audio::MidiInFanout>();
    port->deviceId = resolvePortId( key );
    // The callback goes on BEFORE open(), which is the rule MidiInput states:
    // setting it while open races the device thread by definition.
    port->input->setCallback( port->fanout->callback() );
    if( port->input->open( port->deviceId.toStdString() ) != 0 ) {
        failed_ << key;
        TW_LOGW( "shell", "[LIVE] MIDI input port '%s' (id '%s') would not open",
                 key.toStdString().c_str(), port->deviceId.toStdString().c_str() );
        port->input->setCallback( nullptr );
        return nullptr;
    }
    TW_LOGI( "shell", "[LIVE] MIDI input open: backend=%s port='%s' id='%s'",
             port->input->backendName(), key.toStdString().c_str(),
             port->deviceId.toStdString().c_str() );
    Port *raw = port.get();
    ports_[key] = std::move( port );
    return raw;
}

audio::MidiInFanout *SMidiInputHub::fanoutFor( const QString &portName )
{
    Port *p = portFor( portName );
    return p ? p->fanout.get() : nullptr;
}

bool SMidiInputHub::isOpen( const QString &portName ) const
{
    const QString key = portName.isEmpty() ? QStringLiteral( "default" ) : portName;
    auto it = ports_.find( key );
    return it != ports_.end() && it->second->input && it->second->input->isOpen();
}

audio::MidiInFanout::Sink *SMidiInputHub::recorderSink( const QString &portName )
{
    Port *p = portFor( portName );
    if( !p || !p->fanout ) return nullptr;
    if( !p->recorder )
        p->recorder = p->fanout->acquire( 0xFFFF, "midi recorder" );
    return p->recorder;
}

std::vector<audio::MidiPortInfo> SMidiInputHub::listPorts() const
{
    std::vector<audio::MidiPortInfo> out;
    // The keyboard FIRST: it is the port that always exists, and a user
    // scanning the list for something to play should find it without hunting.
    out.push_back( audio::MidiPortInfo{ audio::KeyboardMidiInput::kPortId,
                                        audio::KeyboardMidiInput::kPortName,
                                        false } );
    if( probe_ )
        for( const audio::MidiPortInfo &p : probe_->listPorts() )
            out.push_back( p );
    return out;
}

QString SMidiInputHub::describe() const
{
    QStringList parts;
    for( const auto &kv : ports_ ) {
        const Port *p = kv.second.get();
        if( !p ) continue;
        parts << QStringLiteral( "port=%1 open=%2 msgs=%3 thru=%4" )
                     .arg( kv.first )
                     .arg( ( p->input && p->input->isOpen() ) ? 1 : 0 )
                     .arg( p->fanout ? (qulonglong) p->fanout->messages() : 0ull )
                     .arg( p->fanout ? (qulonglong) p->fanout->thruSent() : 0ull );
    }
    return parts.join( QStringLiteral( " " ) );
}
