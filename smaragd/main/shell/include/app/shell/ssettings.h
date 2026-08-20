#ifndef _SSETTINGS_H_
#define _SSETTINGS_H_

#include <QObject>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QRect>
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
    // Whether `key` has ANY stored value -- distinct from value() returning a
    // default, and what proposal 38 GATE 5b's plaintext-password migration
    // (§B.8 "Migration") needs: a legacy `media/nextcloud/<id>/password` key
    // must be told apart from "never written".
    bool     contains( const QString &key ) const;
    // Erases `key` outright (a no-op, not an error, when absent). Emits
    // changed() -- unlike setValue(), even though there is no new value,
    // because "this key used to mean something and no longer does" is
    // exactly what the migration case needs a listener to see.
    void     remove( const QString &key );

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

    // Per-DEVICE recording offset, in milliseconds, signed (proposal 21 L0,
    // design §6). The device-reported latencies above are what the driver
    // CLAIMS; this is the user's correction for what it actually does — the
    // number a "record a click, look at where it landed, type the difference"
    // calibration produces, and the last term in the placement conversion
    // (design D6). Keyed by the device NAME so it survives an id change, like
    // midiPortId(). POSITIVE moves a recorded clip EARLIER, matching
    // midiOutOffsetMs's sign convention.
    double recordingOffsetMs( const QString &deviceName ) const;
    void   setRecordingOffsetMs( const QString &deviceName, double ms );

    // The same correction for a MIDI INPUT port, keyed by port name. A
    // controller with its own buffering, or a MIDI-over-USB hub, arrives late
    // by an amount only measurement can find.
    double midiInputOffsetMs( const QString &port ) const;
    void   setMidiInputOffsetMs( const QString &port, double ms );

    // MIDI ports (proposal 37 P7b). A project stores a PORTABLE port NAME
    // (STrack::midiOutPort); the machine-local id the backend's open() wants -
    // a WinMM device index, a CoreMIDI uniqueID, an ALSA "client:port" - lives
    // HERE, keyed by that name. Empty means "not mapped on this machine", and
    // the pump then falls back to matching the backend's own port list.
    QString midiPortId( const QString &portName ) const;
    void    setMidiPortId( const QString &portName, const QString &deviceId );

    // The same mapping for an INPUT port (proposal 21 L2, design section 6).
    // Deliberately a SEPARATE key from midiPortId(): one physical interface
    // usually offers an input and an output with the SAME name, and their
    // machine-local ids are different numbers on every backend - a WinMM input
    // index and a WinMM output index share nothing but their spelling.
    QString midiInputPortId( const QString &portName ) const;
    void    setMidiInputPortId( const QString &portName, const QString &deviceId );

    // Input ports a future session will listen on (proposal 37 P7b persists
    // them; P8 opens them). Machine-local ids, like the audio input device.
    QStringList midiInputPortIds() const;
    void        setMidiInputPortIds( const QStringList &ids );

    // --- native plugin editor geometry (proposal 33 D2) --------------------
    //
    // The window's position and size, per PLUGIN, per user. Deliberately NOT in
    // the project: monitor layout is machine-local, and a window at x=2400
    // restored on a laptop is a window nobody can reach. WHETHER the editor was
    // open does travel, in `<SPluginSlot editorOpen='true'>` — the same split,
    // and the same reason, as the portable midiOutPort NAME against the
    // machine-local midi/portId/<name> above.
    //
    // The key is `<format>:<uid>`, so one plugin's window is the same size
    // wherever it is inserted. That is a DECISION, not an oversight: a user who
    // has sized Dexed to suit their screen means it for Dexed, not for one slot
    // on one track, and per-slot geometry would also have to answer what
    // happens when the slot is removed and re-added.
    //
    // An empty/invalid QRect means "never stored"; the caller then uses the
    // plugin's own preferred size. A STORED rect is still not trusted blindly —
    // see SPluginNativeEditor, which clamps onto a live QScreen before showing.
    QRect pluginEditorGeometry( const QString &pluginKey ) const;
    void  setPluginEditorGeometry( const QString &pluginKey, const QRect &r );

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

    // Nextcloud/WebDAV accounts (proposal 38 GATE 5b, §B.8). ONLY the
    // non-secret half lives here -- url and user, plus the ordered id list
    // the accounts page and the source combo iterate. The password itself
    // never touches this class: `SMediaAccountManager` (main/shell) stores it
    // through `SSecretStore` under the SAME key prefix
    // ("media/nextcloud/<accountId>"), which is what makes the on-disk shape
    //   media/nextcloud/accounts        = "home,band-share,..."
    //   media/nextcloud/<id>/url
    //   media/nextcloud/<id>/user
    //   media/nextcloud/<id>/passwordScheme   (SSecretStore, dpapi-only below)
    //   media/nextcloud/<id>/passwordEnc
    // read as one coherent record per account without this class knowing
    // SSecretStore exists.
    QStringList mediaNextcloudAccountIds() const;
    void        setMediaNextcloudAccountIds( const QStringList &ids );
    QString     mediaNextcloudUrl( const QString &accountId ) const;
    void        setMediaNextcloudUrl( const QString &accountId, const QString &url );
    QString     mediaNextcloudUser( const QString &accountId ) const;
    void        setMediaNextcloudUser( const QString &accountId, const QString &user );
    // Clears url/user for `accountId` (AC 11's "every one of its keys" --
    // the secret half is SSecretStore::remove(), called separately by the
    // caller, exactly as it stores the secret half separately).
    void        removeMediaNextcloudAccountFields( const QString &accountId );

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
