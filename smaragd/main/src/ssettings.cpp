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

QVariant SSettings::value( const QString &key, const QVariant &def ) const
{
    return settings_.value( key, def );
}

void SSettings::setValue( const QString &key, const QVariant &val )
{
    if( settings_.value( key ) == val ) return;   // no-op: don't churn/emit
    settings_.setValue( key, val );
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

QString SSettings::lastDir( const QString &context, const QString &fallback ) const
{
    return value( "paths/" + context, fallback ).toString();
}

void SSettings::setLastDir( const QString &context, const QString &dir )
{
    setValue( "paths/" + context, dir );
}
