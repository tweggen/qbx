#ifndef _SSETTINGS_H_
#define _SSETTINGS_H_

#include <QObject>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QVariant>

// Per-user, per-machine configuration, persisted as an INI file under the
// user's app-config location (e.g. %APPDATA%/Smaragd/smaragd.ini on Windows,
// ~/.config/Smaragd/smaragd.ini on Linux). Home for machine-local settings that
// must NOT live in a portable project file: the selected audio output device,
// last-used file-dialog directories, and (since the options dialog) UI
// preferences such as mouse-wheel navigation.
//
// A QObject singleton so it can emit changed() when a value is written, letting
// live UI (the arranger's wheel handler, etc.) react without polling.
class SSettings : public QObject
{
    Q_OBJECT
public:
    static SSettings &instance();

    // Generic typed access (persisted). setValue() emits changed() on a real
    // change. Prefer SOpt keys + SOpt::def() for the default.
    QVariant value( const QString &key, const QVariant &def = QVariant() ) const;
    void     setValue( const QString &key, const QVariant &val );

    // Selected audio output device id (backend-specific; empty == system
    // default). Matches AudioDeviceInfo::id from the audio backend.
    QString audioDeviceId() const;
    void    setAudioDeviceId( const QString &id );

    // Last-used directory for a file dialog, keyed by a context string
    // ("project", "sample", ...). Returns `fallback` when nothing is stored.
    QString lastDir( const QString &context,
                     const QString &fallback = QString() ) const;
    void    setLastDir( const QString &context, const QString &dir );

    // Most-recently-opened project files, newest first (capped, absolute paths).
    // addRecentProject() de-duplicates and moves the entry to the front;
    // removeRecentProject() drops one (e.g. a file that no longer exists).
    QStringList recentProjects() const;
    void        addRecentProject( const QString &path );
    void        removeRecentProject( const QString &path );

signals:
    void changed( const QString &key );

private:
    SSettings();
    mutable QSettings settings_;
};

#endif
