#include "ssettings.h"

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
