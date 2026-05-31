#ifndef _SSETTINGS_H_
#define _SSETTINGS_H_

#include <QSettings>
#include <QString>

// Per-user, per-machine configuration, persisted as an INI file under the
// user's app-config location (e.g. %APPDATA%/Smaragd/smaragd.ini on Windows,
// ~/.config/Smaragd/smaragd.ini on Linux). This is the home for machine-local
// settings that must NOT live in a portable project file: the selected audio
// output device, and the last-used directories for file dialogs.
class SSettings
{
public:
    static SSettings &instance();

    // Selected audio output device id (backend-specific; empty == system
    // default). Matches AudioDeviceInfo::id from the audio backend.
    QString audioDeviceId() const;
    void    setAudioDeviceId( const QString &id );

    // Last-used directory for a file dialog, keyed by a context string
    // ("project", "sample", ...). Returns `fallback` when nothing is stored.
    QString lastDir( const QString &context,
                     const QString &fallback = QString() ) const;
    void    setLastDir( const QString &context, const QString &dir );

private:
    SSettings();
    mutable QSettings settings_;
};

#endif
