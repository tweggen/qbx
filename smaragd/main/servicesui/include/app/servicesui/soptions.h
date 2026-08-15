#ifndef SOPTIONS_H
#define SOPTIONS_H

#include <QString>
#include <QVariant>

// Central registry of per-user option keys, the mouse-wheel action enum, and
// their default values — shared by the options dialog and the arranger so they
// can never disagree. Values live in SSettings (QSettings INI); read them as
//   SSettings::instance().value( SOpt::WheelPlain, SOpt::def( SOpt::WheelPlain ) )
namespace SOpt {

// What a mouse-wheel gesture does in the arranger.
enum WheelAction {
    None = 0,
    ScrollVertical,     // scroll tracks
    ScrollHorizontal,   // scroll the timeline
    ZoomHorizontal,     // zoom time (px/second)
    ZoomVertical        // zoom track height
};

// Option keys (QSettings paths).
inline constexpr const char *WheelPlain     = "mouse/wheelPlain";
inline constexpr const char *WheelShift     = "mouse/wheelShift";
inline constexpr const char *WheelCtrl      = "mouse/wheelCtrl";
inline constexpr const char *WheelCtrlShift = "mouse/wheelCtrlShift";
inline constexpr const char *ZoomToCursor   = "mouse/zoomToCursor";
inline constexpr const char *InvertZoom     = "mouse/invertZoom";
// Keep the playhead on screen: re-page the timeline when the cursor advances
// (under playback/recording) toward the edge of the view. Default on.
inline constexpr const char *FollowPlayhead = "view/followPlayhead";
inline constexpr const char *AudioDeviceId  = "audio/deviceId";

// Logging (proposal 24). LogConsole's default is the BUILD's default —
// SMARAGD_LOG_CONSOLE_DEFAULT, on for Debug and off for Release — so a release
// user opts in and a debug build keeps its console for free. The command line
// (--log-console / --no-log-console / --log-level) and the SMARAGD_LOG_CONSOLE
// / SMARAGD_LOG_LEVEL environment variables override these, in that order.
inline constexpr const char *LogConsole     = "log/console";
inline constexpr const char *LogLevel       = "log/level";      // "error".."trace"
inline constexpr const char *LogCapacity    = "log/capacity";   // ring records
inline constexpr const char *LogToFile      = "log/toFile";

// Keyboard shortcuts (proposal 31). There is no keybinding UI: the sequence is
// read once at startup through SSettings, so the default below is a DEFAULT
// rather than a constant, rebindable by editing smaragd.ini. Parsed with
// QKeySequence::fromString, so an unparseable value simply leaves the action
// unbound rather than crashing.
inline constexpr const char *ShortcutClipProperties = "ui/shortcuts/clipProperties";

// Plugin hosting (proposal 08 M2). PluginSearchPaths is a QStringList of
// directories, scanned recursively; its default is the FIRST platform-dependent
// default in this table and comes from the engine
// (audio::twPluginSearchPaths::defaults), so the standard per-OS locations are
// stated once. PluginScanOnStartup makes the app scan changed modules in the
// background at launch; the cache makes that cheap after the first run.
inline constexpr const char *PluginSearchPaths   = "plugins/searchPaths";
inline constexpr const char *PluginScanOnStartup = "plugins/scanOnStartup";

// MIDI (proposal 36 D6 / P7b). All three are per-user and machine-local, which
// is exactly why they are here and not in the project: a port NAME travels with
// the project, the id it resolves to does not (SSettings::midiPortId).
//
//   MidiOutOffsetMs   the GLOBAL send offset added to every track's own one,
//                     signed, POSITIVE = send EARLIER. Compensates a whole rig
//                     (an interface's MIDI jitter, a converter) rather than one
//                     device.
//   MidiChaseNoteOns  whether a chase at start/locate re-attacks the notes that
//                     were already sounding. Default OFF for MIDI out: pushing
//                     a note-on at a hardware synth on every locate is usually
//                     a surprise. Controllers/program/bend are chased ALWAYS
//                     and are not configurable - without them the receiving
//                     device is simply in the wrong state.
//   MidiInputPortIds  the input ports a future session will listen on. Listed
//                     and persisted here from P7b; nothing reads them until P8
//                     opens the input side.
inline constexpr const char *MidiOutOffsetMs   = "midi/outOffsetMs";
inline constexpr const char *MidiChaseNoteOns  = "midi/chaseNoteOns";
inline constexpr const char *MidiInputPortIds  = "midi/inputPortIds";

// Default value for a key (invalid QVariant if unknown). Scroll-first defaults.
QVariant def( const QString &key );

// Human-readable label for a wheel action (for combo boxes).
QString wheelActionLabel( WheelAction a );

}  // namespace SOpt

#endif // SOPTIONS_H
