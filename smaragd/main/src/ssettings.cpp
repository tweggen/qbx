#include "ssettings.h"

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

QString SSettings::audioDeviceId() const
{
    return settings_.value( "audio/deviceId" ).toString();
}

void SSettings::setAudioDeviceId( const QString &id )
{
    settings_.setValue( "audio/deviceId", id );
}

QString SSettings::lastDir( const QString &context, const QString &fallback ) const
{
    return settings_.value( "paths/" + context, fallback ).toString();
}

void SSettings::setLastDir( const QString &context, const QString &dir )
{
    settings_.setValue( "paths/" + context, dir );
}
