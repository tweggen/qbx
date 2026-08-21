#include <QRect>
#include "app/shell/ssettings.h"
#include "app/servicesui/soptions.h"

#include <QFileInfo>

SSettings &SSettings::instance()
{
    static SSettings s;
    return s;
}

SSettings::SSettings()
    : settings_( QSettings::IniFormat, QSettings::UserScope,
                 "Smaragd", "smaragd" )
{
}

QString SSettings::configDir() const
{
    return QFileInfo( settings_.fileName() ).absolutePath();
}

QVariant SSettings::value( const QString &key, const QVariant &def ) const
{
    return settings_.value( key, def );
}

void SSettings::setValue( const QString &key, const QVariant &val )
{
    if( settings_.value( key ) == val ) return;   // no-op: don't churn/emit
    settings_.setValue( key, val );
    settings_.sync();  // Ensure changes are written to disk immediately
    emit changed( key );
}

bool SSettings::contains( const QString &key ) const
{
    return settings_.contains( key );
}

void SSettings::remove( const QString &key )
{
    if( !settings_.contains( key ) ) return;   // no-op: don't churn/emit
    settings_.remove( key );
    settings_.sync();
    emit changed( key );
}

QString SSettings::audioDeviceId() const
{
    return value( "audio/deviceId" ).toString();
}

void SSettings::setAudioDeviceId( const QString &id )
{
    setValue( "audio/deviceId", id );
}

QString SSettings::audioInputDeviceId() const
{
    return value( "audio/inputDeviceId" ).toString();
}

void SSettings::setAudioInputDeviceId( const QString &id )
{
    setValue( "audio/inputDeviceId", id );
}

double SSettings::recordingOffsetMs( const QString &deviceName ) const
{
    if( deviceName.isEmpty() ) return 0.0;
    return value( "audio/recordingOffsetMs/" + deviceName, 0.0 ).toDouble();
}

void SSettings::setRecordingOffsetMs( const QString &deviceName, double ms )
{
    if( deviceName.isEmpty() ) return;
    setValue( "audio/recordingOffsetMs/" + deviceName, ms );
}

double SSettings::midiInputOffsetMs( const QString &port ) const
{
    if( port.isEmpty() ) return 0.0;
    return value( "midi/inputOffsetMs/" + port, 0.0 ).toDouble();
}

void SSettings::setMidiInputOffsetMs( const QString &port, double ms )
{
    if( port.isEmpty() ) return;
    setValue( "midi/inputOffsetMs/" + port, ms );
}

QString SSettings::midiPortId( const QString &portName ) const
{
    if( portName.isEmpty() ) return QString();
    return value( "midi/portId/" + portName ).toString();
}

void SSettings::setMidiPortId( const QString &portName, const QString &deviceId )
{
    if( portName.isEmpty() ) return;
    setValue( "midi/portId/" + portName, deviceId );
}

QRect SSettings::pluginEditorGeometry( const QString &pluginKey ) const
{
    if( pluginKey.isEmpty() ) return QRect();
    const QVariant v = value( "pluginui/editorGeometry/" + pluginKey );
    if( !v.isValid() ) return QRect();
    const QRect r = v.toRect();
    // A stored rect with no extent is not a geometry, it is a corrupt key. Do
    // not hand it on as one: the caller cannot tell it from "never stored" and
    // would resize a window to nothing.
    return ( r.width() > 0 && r.height() > 0 ) ? r : QRect();
}

void SSettings::setPluginEditorGeometry( const QString &pluginKey, const QRect &r )
{
    if( pluginKey.isEmpty() ) return;
    if( r.width() <= 0 || r.height() <= 0 ) return;
    setValue( "pluginui/editorGeometry/" + pluginKey, r );
}

QString SSettings::midiInputPortId( const QString &portName ) const
{
    if( portName.isEmpty() ) return QString();
    return value( "midi/inputPortId/" + portName ).toString();
}

void SSettings::setMidiInputPortId( const QString &portName, const QString &deviceId )
{
    if( portName.isEmpty() ) return;
    setValue( "midi/inputPortId/" + portName, deviceId );
}

QStringList SSettings::midiInputPortIds() const
{
    return value( "midi/inputPortIds" ).toStringList();
}

void SSettings::setMidiInputPortIds( const QStringList &ids )
{
    setValue( "midi/inputPortIds", ids );
}

QStringList SSettings::mediaNextcloudAccountIds() const
{
    return value( "media/nextcloud/accounts" ).toStringList();
}

void SSettings::setMediaNextcloudAccountIds( const QStringList &ids )
{
    setValue( "media/nextcloud/accounts", ids );
}

QString SSettings::mediaNextcloudUrl( const QString &accountId ) const
{
    if( accountId.isEmpty() ) return QString();
    return value( "media/nextcloud/" + accountId + "/url" ).toString();
}

void SSettings::setMediaNextcloudUrl( const QString &accountId, const QString &url )
{
    if( accountId.isEmpty() ) return;
    setValue( "media/nextcloud/" + accountId + "/url", url );
}

QString SSettings::mediaNextcloudUser( const QString &accountId ) const
{
    if( accountId.isEmpty() ) return QString();
    return value( "media/nextcloud/" + accountId + "/user" ).toString();
}

void SSettings::setMediaNextcloudUser( const QString &accountId, const QString &user )
{
    if( accountId.isEmpty() ) return;
    setValue( "media/nextcloud/" + accountId + "/user", user );
}

void SSettings::removeMediaNextcloudAccountFields( const QString &accountId )
{
    if( accountId.isEmpty() ) return;
    remove( "media/nextcloud/" + accountId + "/url" );
    remove( "media/nextcloud/" + accountId + "/user" );
}

uint32_t SSettings::audioOutputLatencyFrames( const QString &deviceId ) const
{
    return value( "audio/outputLatency/" + deviceId, 0u ).toUInt();
}

void SSettings::setAudioOutputLatencyFrames( const QString &deviceId, uint32_t frames )
{
    setValue( "audio/outputLatency/" + deviceId, frames );
}

uint32_t SSettings::audioInputLatencyFrames( const QString &deviceId ) const
{
    return value( "audio/inputLatency/" + deviceId, 0u ).toUInt();
}

void SSettings::setAudioInputLatencyFrames( const QString &deviceId, uint32_t frames )
{
    setValue( "audio/inputLatency/" + deviceId, frames );
}

uint32_t SSettings::audioInputChannelCount( const QString &deviceId ) const
{
    return value( "audio/inputDeviceChannels/" + deviceId, 0u ).toUInt();
}

void SSettings::setAudioInputChannelCount( const QString &deviceId, uint32_t channels )
{
    setValue( "audio/inputDeviceChannels/" + deviceId, channels );
}

QString SSettings::lastDir( const QString &context, const QString &fallback ) const
{
    return value( "paths/" + context, fallback ).toString();
}

void SSettings::setLastDir( const QString &context, const QString &dir )
{
    setValue( "paths/" + context, dir );
}

static const char *kRecentKey = "recent/projects";
static const int   kRecentMax = 5;

QStringList SSettings::recentProjects() const
{
    return value( kRecentKey ).toStringList();
}

void SSettings::addRecentProject( const QString &path )
{
    if( path.isEmpty() ) return;
    const QString abs = QFileInfo( path ).absoluteFilePath();

    QStringList list = recentProjects();
    // Drop any existing entry for the same file (case-insensitive: Windows paths).
    for( int i = list.size() - 1; i >= 0; --i ) {
        if( QString::compare( QFileInfo( list.at( i ) ).absoluteFilePath(),
                              abs, Qt::CaseInsensitive ) == 0 ) {
            list.removeAt( i );
        }
    }
    list.prepend( abs );
    while( list.size() > kRecentMax ) list.removeLast();

    setValue( kRecentKey, list );
}

void SSettings::removeRecentProject( const QString &path )
{
    const QString abs = QFileInfo( path ).absoluteFilePath();
    QStringList list = recentProjects();
    bool changed = false;
    for( int i = list.size() - 1; i >= 0; --i ) {
        if( QString::compare( QFileInfo( list.at( i ) ).absoluteFilePath(),
                              abs, Qt::CaseInsensitive ) == 0 ) {
            list.removeAt( i );
            changed = true;
        }
    }
    if( changed ) setValue( kRecentKey, list );
}

QStringList SSettings::pluginSearchPaths() const
{
    // A user who removes every entry means "search nowhere"; that is stored as
    // an empty (but present) list, which QSettings round-trips distinctly from
    // "never configured".
    if( !settings_.contains( SOpt::PluginSearchPaths ) )
        return SOpt::def( SOpt::PluginSearchPaths ).toStringList();
    return value( SOpt::PluginSearchPaths ).toStringList();
}

void SSettings::setPluginSearchPaths( const QStringList &dirs )
{
    setValue( SOpt::PluginSearchPaths, dirs );
}

bool SSettings::pluginScanOnStartup() const
{
    return value( SOpt::PluginScanOnStartup,
                  SOpt::def( SOpt::PluginScanOnStartup ) ).toBool();
}

void SSettings::setPluginScanOnStartup( bool on )
{
    setValue( SOpt::PluginScanOnStartup, on );
}

QByteArray SSettings::windowGeometry() const
{
    return value( "ui/windowGeometry" ).toByteArray();
}

void SSettings::setWindowGeometry( const QByteArray &geometry )
{
    setValue( "ui/windowGeometry", geometry );
}

QByteArray SSettings::windowState() const
{
    return value( "ui/windowState" ).toByteArray();
}

void SSettings::setWindowState( const QByteArray &state )
{
    setValue( "ui/windowState", state );
}
