#include "app/testkit/smidiouttestactions.h"

#include <cstdlib>

#include <QCoreApplication>
#include <QDebug>
#include <QDomElement>
#include <QElapsedTimer>
#include <QFile>
#include <QStringList>
#include <QTextStream>
#include <QThread>

#include "app/actions/sactionregistry.h"
#include "app/servicesui/soptionsdialog.h"
#include "app/shell/sapplication.h"
#include "app/shell/smidioutpump.h"
#include "app/shell/ssettings.h"

#include "tw/devices/capture_backend.h"
#include "tw/devices/capture_midi.h"
#include "tw/playback/twspeaker.h"

namespace {

// One captured message, decoded far enough to assert on.
struct Msg {
    int    index   = 0;      // position in the recording
    QString port;
    qint64 hostNs  = 0;
    qint64 frame   = 0;      // project frames since playback started
    int    status  = 0;
    int    channel = -1;
    int    d1      = -1;
    int    d2      = -1;
    QString kind;
    QString bytesHex;
};

QString kindOfStatus( int status )
{
    switch( status & 0xF0 ) {
    case 0x80: return QStringLiteral( "noteoff" );
    case 0x90: return QStringLiteral( "noteon" );
    case 0xA0: return QStringLiteral( "polypressure" );
    case 0xB0: return QStringLiteral( "cc" );
    case 0xC0: return QStringLiteral( "programchange" );
    case 0xD0: return QStringLiteral( "channelpressure" );
    case 0xE0: return QStringLiteral( "pitchbend" );
    default:   return QStringLiteral( "other" );
    }
}

// The MIDI backend this process is actually using. Read from the environment
// rather than from a MidiOutput instance ON PURPOSE: constructing a throwaway
// CaptureMidiOutput to ask it would register itself as the live capture port
// and unregister the real one when it died. main.cpp sets the variable for
// every --test-case run, so this is exact rather than a guess.
QString midiBackendName()
{
    const char *env = std::getenv( "SMARAGD_MIDI_BACKEND" );
    QString v = env ? QString::fromLatin1( env ).trimmed().toLower() : QString();
    return v.isEmpty() ? QStringLiteral( "default" ) : v;
}

audio::CaptureBackend *audioCapture()
{
    std::shared_ptr<twSpeaker> speaker = SApplication::app().getSpeaker();
    audio::AudioBackend *backend = speaker ? speaker->getBackend() : nullptr;
    return dynamic_cast<audio::CaptureBackend *>( backend );
}

// Collect the recording, mapped onto the audio clock. `ok` is false when the
// setup cannot support the measurement at all (which is a REJECT, not a fail).
bool collectMessages( QVector<Msg> &out, QString &why )
{
    if( midiBackendName() != QStringLiteral( "capture" ) ) {
        why = QString( "the MIDI backend is '%1', not 'capture' - there is "
                       "nothing recorded to assert about. A --test-case run "
                       "defaults to capture; something set SMARAGD_MIDI_BACKEND."
                     ).arg( midiBackendName() );
        return false;
    }
    audio::CaptureBackend *cap = audioCapture();
    if( !cap ) {
        why = QStringLiteral(
            "the AUDIO backend is not the capture backend, so there is no "
            "block log to map MIDI host times through. Set "
            "SMARAGD_AUDIO_BACKEND=capture (a --test-case run does)." );
        return false;
    }

    // The device latency correction: the pump aims a message at the instant
    // the audio at that position is HEARD, so mapping the send time back
    // through the DELIVERY log lands one output latency late. Subtracting it
    // is a fact about the device, not about the pump.
    qint64 latency = 0;
    if( audio::AudioBackend *b = cap ) latency = (qint64) b->getLatencyFrames();

    out.clear();
    audio::CaptureMidiOutput *midi = audio::CaptureMidiOutput::active();
    if( !midi ) return true;    // capture backend, no port ever opened: empty

    const std::vector<audio::CaptureMidiOutput::Event> events = midi->captured();
    int i = 0;
    for( const audio::CaptureMidiOutput::Event &e : events ) {
        Msg m;
        m.index  = i++;
        m.port   = QString::fromStdString( e.port );
        m.hostNs = e.hostTimeNs;
        m.frame  = cap->frameAtHostTime( e.hostTimeNs ) - latency;
        if( !e.bytes.empty() ) {
            m.status  = e.bytes[0];
            m.channel = ( m.status & 0xF0 ) == 0xF0 ? -1 : ( m.status & 0x0F );
            m.kind    = kindOfStatus( m.status );
        }
        if( e.bytes.size() > 1 ) m.d1 = e.bytes[1];
        if( e.bytes.size() > 2 ) m.d2 = e.bytes[2];
        QStringList hex;
        for( std::uint8_t b : e.bytes )
            hex << QString( "%1" ).arg( (int) b, 2, 16, QChar( '0' ) );
        m.bytesHex = hex.join( ' ' );
        out.append( m );
    }
    return true;
}

// The LAST data byte, which is what "value" means for every kind that has one.
int valueOf( const Msg &m )
{
    return ( m.d2 >= 0 ) ? m.d2 : m.d1;
}

}  // namespace

// ------------------------------------------------------------ assert-midi-out

QStringList SAssertMidiOutAction::knownAttributes() const
{
    return { QStringLiteral( "port" ),     QStringLiteral( "kind" ),
             QStringLiteral( "channel" ),  QStringLiteral( "key" ),
             QStringLiteral( "cc" ),       QStringLiteral( "value" ),
             QStringLiteral( "at" ),       QStringLiteral( "tolerance" ),
             QStringLiteral( "count" ),    QStringLiteral( "minCount" ),
             QStringLiteral( "maxCount" ), QStringLiteral( "index" ) };
}

SApplyResult SAssertMidiOutAction::apply( SProject * )
{
    QVector<Msg> msgs;
    QString why;
    if( !collectMessages( msgs, why ) ) {
        qWarning() << "assert-midi-out: REJECTED -" << why;
        return { false, nullptr };
    }

    QVector<Msg> hits;
    for( const Msg &m : msgs ) {
        if( !port_.isEmpty() && m.port != port_ ) continue;
        if( !kind_.isEmpty() && kind_ != QStringLiteral( "any" )
            && m.kind != kind_ ) continue;
        if( channel_ >= 0 && m.channel != channel_ ) continue;
        if( key_ >= 0 && m.d1 != key_ ) continue;
        if( cc_ >= 0 && m.d1 != cc_ ) continue;
        if( value_ >= 0 && valueOf( m ) != value_ ) continue;
        if( hasAt_ && qAbs( m.frame - at_ ) > tolerance_ ) continue;
        hits.append( m );
    }

    auto dump = [&]() {
        QStringList lines;
        for( const Msg &m : msgs )
            lines << QString( "  #%1 %2 ch=%3 d1=%4 d2=%5 frame=%6 port=%7 [%8]" )
                         .arg( m.index ).arg( m.kind ).arg( m.channel )
                         .arg( m.d1 ).arg( m.d2 ).arg( m.frame )
                         .arg( m.port ).arg( m.bytesHex );
        if( lines.isEmpty() ) lines << QStringLiteral( "  (nothing captured)" );
        return lines.join( "\n" );
    };

    const int n = hits.size();
    bool ok = true;
    if( count_    >= 0 && n != count_ )    ok = false;
    if( minCount_ >= 0 && n <  minCount_ ) ok = false;
    if( maxCount_ >= 0 && n >  maxCount_ ) ok = false;
    // `at` with no count constraint means "at least one landed there".
    if( hasAt_ && count_ < 0 && minCount_ < 0 && n == 0 ) ok = false;
    if( index_ >= 0 && ( n == 0 || hits.first().index != index_ ) ) ok = false;

    if( !ok ) {
        qWarning().noquote()
            << QString( "assert-midi-out: FAILED. matched %1 message(s)"
                        " (kind='%2' channel=%3 key=%4 cc=%5 value=%6 at=%7"
                        " tolerance=%8 expect count=%9 min=%10 max=%11"
                        " index=%12)\n%13" )
                   .arg( n ).arg( kind_ ).arg( channel_ ).arg( key_ ).arg( cc_ )
                   .arg( value_ ).arg( hasAt_ ? QString::number( at_ ) : "-" )
                   .arg( tolerance_ ).arg( count_ ).arg( minCount_ )
                   .arg( maxCount_ ).arg( index_ ).arg( dump() );
        return { false, nullptr };
    }

    // The measured lateness is the number the gate is actually about, so it is
    // reported on SUCCESS too - a passing assertion with 4000 frames of error
    // is one machine away from a failing one.
    if( hasAt_ && n > 0 ) {
        qint64 worst = 0;
        for( const Msg &m : hits )
            if( qAbs( m.frame - at_ ) > qAbs( worst ) ) worst = m.frame - at_;
        qDebug() << "assert-midi-out: OK -" << n << "match(es), worst offset"
                 << worst << "frames from" << at_ << "(tolerance" << tolerance_ << ")";
    } else {
        qDebug() << "assert-midi-out: OK -" << n << "match(es)";
    }
    return { true, nullptr };
}

void SAssertMidiOutAction::writeXml( QDomElement &elem ) const
{
    if( !port_.isEmpty() ) elem.setAttribute( "port", port_ );
    if( !kind_.isEmpty() ) elem.setAttribute( "kind", kind_ );
    elem.setAttribute( "channel", QString::number( channel_ ) );
    elem.setAttribute( "key", QString::number( key_ ) );
    elem.setAttribute( "cc", QString::number( cc_ ) );
    elem.setAttribute( "value", QString::number( value_ ) );
    if( hasAt_ ) elem.setAttribute( "at", QString::number( at_ ) );
    elem.setAttribute( "tolerance", QString::number( tolerance_ ) );
    elem.setAttribute( "count", QString::number( count_ ) );
    elem.setAttribute( "minCount", QString::number( minCount_ ) );
    elem.setAttribute( "maxCount", QString::number( maxCount_ ) );
    elem.setAttribute( "index", QString::number( index_ ) );
}

bool SAssertMidiOutAction::readXml( const QDomElement &elem, int )
{
    port_      = elem.attribute( "port", "" );
    kind_      = elem.attribute( "kind", "" );
    channel_   = elem.attribute( "channel", "-1" ).toInt();
    key_       = elem.attribute( "key", "-1" ).toInt();
    cc_        = elem.attribute( "cc", "-1" ).toInt();
    value_     = elem.attribute( "value", "-1" ).toInt();
    hasAt_     = elem.hasAttribute( "at" );
    at_        = elem.attribute( "at", "0" ).toLongLong();
    tolerance_ = elem.attribute( "tolerance", "4096" ).toLongLong();
    count_     = elem.attribute( "count", "-1" ).toInt();
    minCount_  = elem.attribute( "minCount", "-1" ).toInt();
    maxCount_  = elem.attribute( "maxCount", "-1" ).toInt();
    index_     = elem.attribute( "index", "-1" ).toInt();
    return true;
}

// ---------------------------------------------------------- dump-midi-capture

QStringList SDumpMidiCaptureAction::knownAttributes() const
{
    return { QStringLiteral( "filename" ), QStringLiteral( "minCount" ) };
}

SApplyResult SDumpMidiCaptureAction::apply( SProject * )
{
    if( filename_.isEmpty() ) {
        qWarning() << "dump-midi-capture: no filename given";
        return { false, nullptr };
    }
    // Same sanitisation as <screenshot> and <dump-playback-capture>: the verb
    // writes into the test output directory and nowhere else.
    if( filename_.contains( '/' ) || filename_.contains( '\\' )
        || filename_.contains( ".." ) ) {
        qWarning() << "dump-midi-capture: filename contains path separators:"
                   << filename_;
        return { false, nullptr };
    }

    SApplication &app = SApplication::app();
    const QString outputDir = app.testOutputDir();
    if( outputDir.isEmpty() ) {
        qWarning() << "dump-midi-capture: no test output directory configured";
        return { false, nullptr };
    }
    if( !app.ensureOutputDirExists() ) {
        qWarning() << "dump-midi-capture: failed to create output directory:"
                   << outputDir;
        return { false, nullptr };
    }

    QVector<Msg> msgs;
    QString why;
    if( !collectMessages( msgs, why ) ) {
        qWarning() << "dump-midi-capture: REJECTED -" << why;
        return { false, nullptr };
    }
    if( msgs.size() < minCount_ ) {
        qWarning() << "dump-midi-capture: captured only" << msgs.size()
                   << "message(s), expected at least" << minCount_;
        return { false, nullptr };
    }

    const QString fullPath = outputDir + "/" + filename_;
    QFile f( fullPath );
    if( !f.open( QIODevice::WriteOnly | QIODevice::Text ) ) {
        qWarning() << "dump-midi-capture: cannot write" << fullPath;
        return { false, nullptr };
    }
    QTextStream out( &f );
    out << "# index  hostTimeNs  frame(project, since playback start)  port"
           "  bytes  decoded\n";
    for( const Msg &m : msgs ) {
        out << m.index << "\t" << m.hostNs << "\t" << m.frame << "\t"
            << m.port << "\t" << m.bytesHex << "\t"
            << m.kind << " ch=" << m.channel << " d1=" << m.d1
            << " d2=" << m.d2 << "\n";
    }
    f.close();

    qDebug() << "dump-midi-capture: wrote" << msgs.size() << "message(s) to"
             << fullPath;
    return { true, nullptr };   // Not undoable: it produces an artifact.
}

void SDumpMidiCaptureAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "filename", filename_ );
    if( minCount_ > 0 ) elem.setAttribute( "minCount", QString::number( minCount_ ) );
}

bool SDumpMidiCaptureAction::readXml( const QDomElement &elem, int )
{
    filename_ = elem.attribute( "filename", "" );
    minCount_ = elem.attribute( "minCount", "0" ).toInt();
    return true;
}

// -------------------------------------------------------- assert-midi-options

QStringList SAssertMidiOptionsAction::knownAttributes() const
{
    return { QStringLiteral( "contains" ), QStringLiteral( "absent" ),
             QStringLiteral( "outputPorts" ), QStringLiteral( "inputPorts" ) };
}

SApplyResult SAssertMidiOptionsAction::apply( SProject * )
{
    // The REAL dialog, built off screen and never shown (a qxa run on Windows
    // uses the real platform plugin, so a shown dialog would appear on the
    // developer's screen and could steal focus). Its constructor runs
    // loadMidiPage(), which is the code under test.
    SOptionsDialog dialog( nullptr );
    const QString desc = dialog.describeMidiPage();

    bool ok = true;
    QStringList problems;
    if( !contains_.isEmpty() && !desc.contains( contains_ ) ) {
        ok = false;
        problems << QString( "expected to contain '%1'" ).arg( contains_ );
    }
    if( !absent_.isEmpty() && desc.contains( absent_ ) ) {
        ok = false;
        problems << QString( "expected NOT to contain '%1'" ).arg( absent_ );
    }
    if( outputPorts_ >= 0
        && !desc.contains( QString( "outputs=%1\n" ).arg( outputPorts_ ) ) ) {
        ok = false;
        problems << QString( "expected outputs=%1" ).arg( outputPorts_ );
    }
    if( inputPorts_ >= 0
        && !desc.contains( QString( "inputs=%1\n" ).arg( inputPorts_ ) ) ) {
        ok = false;
        problems << QString( "expected inputs=%1" ).arg( inputPorts_ );
    }

    if( !ok ) {
        qWarning().noquote() << "assert-midi-options: FAILED -"
                             << problems.join( "; " ) << "\n" << desc;
        return { false, nullptr };
    }
    qDebug().noquote() << "assert-midi-options: OK\n" << desc;
    return { true, nullptr };
}

void SAssertMidiOptionsAction::writeXml( QDomElement &elem ) const
{
    if( !contains_.isEmpty() ) elem.setAttribute( "contains", contains_ );
    if( !absent_.isEmpty() )   elem.setAttribute( "absent", absent_ );
    elem.setAttribute( "outputPorts", QString::number( outputPorts_ ) );
    elem.setAttribute( "inputPorts", QString::number( inputPorts_ ) );
}

bool SAssertMidiOptionsAction::readXml( const QDomElement &elem, int )
{
    contains_    = elem.attribute( "contains", "" );
    absent_      = elem.attribute( "absent", "" );
    outputPorts_ = elem.attribute( "outputPorts", "-1" ).toInt();
    inputPorts_  = elem.attribute( "inputPorts", "-1" ).toInt();
    return true;
}

// ------------------------------------------------------------------ set-option

SApplyResult SSetOptionAction::apply( SProject * )
{
    if( key_.isEmpty() ) {
        qWarning() << "set-option: no key given";
        return { false, nullptr };
    }
    SSettings::instance().setValue( key_, value_ );
    return { true, nullptr };   // Not undoable: a per-user preference.
}

void SSetOptionAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "key", key_ );
    elem.setAttribute( "value", value_ );
}

bool SSetOptionAction::readXml( const QDomElement &elem, int )
{
    key_   = elem.attribute( "key", "" );
    value_ = elem.attribute( "value", "" );
    return true;
}

// --------------------------------------------------------------------- wait-ms

SApplyResult SWaitMsAction::apply( SProject * )
{
    if( ms_ <= 0 ) return { true, nullptr };
    QElapsedTimer timer;
    timer.start();
    // Pumping the event loop is the POINT: every main-thread timer the
    // behaviour under test rides on - the locator poll, the meters, the
    // MIDI-out pump - only runs while events are processed.
    while( timer.elapsed() < ms_ ) {
        QCoreApplication::processEvents();
        QThread::msleep( 2 );
    }
    return { true, nullptr };
}

void SWaitMsAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "ms", QString::number( ms_ ) );
}

bool SWaitMsAction::readXml( const QDomElement &elem, int )
{
    ms_ = elem.attribute( "ms", "0" ).toInt();
    return true;
}

static const bool s_reg_midi_out_verbs = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "assert-midi-out" ),
        []{ return new SAssertMidiOutAction; } ),
    SActionRegistry::instance().registerType(
        QStringLiteral( "dump-midi-capture" ),
        []{ return new SDumpMidiCaptureAction; } ),
    SActionRegistry::instance().registerType(
        QStringLiteral( "assert-midi-options" ),
        []{ return new SAssertMidiOptionsAction; } ),
    SActionRegistry::instance().registerType(
        QStringLiteral( "set-option" ),
        []{ return new SSetOptionAction; } ),
    SActionRegistry::instance().registerType(
        QStringLiteral( "wait-ms" ),
        []{ return new SWaitMsAction; } ),
    true );
