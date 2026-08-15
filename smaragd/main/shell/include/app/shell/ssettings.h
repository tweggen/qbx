#ifndef _SSETTINGS_H_
#define _SSETTINGS_H_

#include <QObject>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QByteArray>

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

    // Selected audio input device id (backend-specific; empty == system
    // default). Used for recording input.
    QString audioInputDeviceId() const;
    void    setAudioInputDeviceId( const QString &id );

    // Cached audio device latencies (in frames). Measured at startup and
    // persisted to avoid repeated device opens. Returns 0 if not yet measured.
    uint32_t audioOutputLatencyFrames( const QString &deviceId ) const;
    void     setAudioOutputLatencyFrames( const QString &deviceId, uint32_t frames );
    uint32_t audioInputLatencyFrames( const QString &deviceId ) const;
    void     setAudioInputLatencyFrames( const QString &deviceId, uint32_t frames );

    // MIDI ports (proposal 37 P7b). A project stores a PORTABLE port NAME
    // (STrack::midiOutPort); the machine-local id the backend's open() wants -
    // a WinMM device index, a CoreMIDI uniqueID, an ALSA "client:port" - lives
    // HERE, keyed by that name. Empty means "not mapped on this machine", and
    // the pump then falls back to matching the backend's own port list.
    QString midiPortId( const QString &portName ) const;
    void    setMidiPortId( const QString &portName, const QString &deviceId );

    // Input ports a future session will listen on (proposal 37 P7b persists
    // them; P8 opens them). Machine-local ids, like the audio input device.
    QStringList midiInputPortIds() const;
    void        setMidiInputPortIds( const QStringList &ids );

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

    // Directories scanned for plugin modules, recursively (proposal 08 M2).
    // Falls back to SOpt::def( SOpt::PluginSearchPaths ), i.e. the engine's
    // per-platform defaults, so a user who never opened the options dialog
    // still gets the standard locations.
    QStringList pluginSearchPaths() const;
    void        setPluginSearchPaths( const QStringList &dirs );
    bool        pluginScanOnStartup() const;
    void        setPluginScanOnStartup( bool on );

    // Main window geometry (size and position, binary format).
    QByteArray windowGeometry() const;
    void       setWindowGeometry( const QByteArray &geometry );

    // Main window state (toolbar/dock layout and positions, binary format).
    QByteArray windowState() const;
    void       setWindowState( const QByteArray &state );

    // Directory holding smaragd.ini. Also where the rotating log file goes, so
    // a user asked for "your config folder" finds both in one place.
    QString configDir() const;

signals:
    void changed( const QString &key );

private:
    SSettings();
    mutable QSettings settings_;
};

#endif
