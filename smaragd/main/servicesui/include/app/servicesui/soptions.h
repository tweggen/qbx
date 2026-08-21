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
// How far ONE wheel notch travels, as a PERCENTAGE (100 = the shipped feel),
// scaling all four wheel gestures together. One number rather than four because
// the thing being compensated is the DEVICE — a jumpy trackpad or a stiff wheel
// is wrong in every gesture at once, and tuning them independently is a fiddle
// nobody wants. Stored in percent so the INI is readable and the spin box needs
// no conversion; the arranger divides by 100 and clamps.
inline constexpr const char *WheelSensitivityPct = "mouse/wheelSensitivityPercent";
// Keep the playhead on screen: re-page the timeline when the cursor advances
// (under playback/recording) toward the edge of the view. Default on.
inline constexpr const char *FollowPlayhead = "view/followPlayhead";
inline constexpr const char *AudioDeviceId  = "audio/deviceId";

// EVENT EDITOR (piano roll) mouse wheel. Its own namespace, independent of the
// arranger's WheelPlain/WheelShift/WheelCtrl/WheelCtrlShift/ZoomToCursor/
// InvertZoom/WheelSensitivityPct above, so a user who wants a different feel
// in the two views can set one -- but DEFAULTING to the same four-gesture
// mapping and the same sensitivity/zoom-to-cursor/invert-zoom values, so
// nobody's existing install changes feel before they open Options (same
// reasoning as WheelSensitivityPct's 100 %). `WheelAction` above is reused
// verbatim: "scroll vertical" means the note-grid's key rows, "scroll
// horizontal" and "zoom horizontal" mean the shared SEventTimeAxis, and "zoom
// vertical" means the piano roll's own key-row height (SPianoRollView::
// keyHeight_). See main/eventui/CONTRACT.md and SPianoRollView::wheelEvent().
inline constexpr const char *EventWheelPlain     = "eventEditor/mouse/wheelPlain";
inline constexpr const char *EventWheelShift     = "eventEditor/mouse/wheelShift";
inline constexpr const char *EventWheelCtrl      = "eventEditor/mouse/wheelCtrl";
inline constexpr const char *EventWheelCtrlShift = "eventEditor/mouse/wheelCtrlShift";
inline constexpr const char *EventZoomToCursor   = "eventEditor/mouse/zoomToCursor";
inline constexpr const char *EventInvertZoom     = "eventEditor/mouse/invertZoom";
inline constexpr const char *EventWheelSensitivityPct =
    "eventEditor/mouse/wheelSensitivityPercent";

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

// Widget style / theme. The NAMES STHeme::available() understands ("brownpro",
// "system"); an unknown value falls back rather than leaving the app unstyled.
// There is no options-page control yet -- like the shortcut above, this is a
// DEFAULT that is rebindable by editing smaragd.ini, and the environment
// variable SMARAGD_UI_THEME overrides it for one run.
//
// The default is EMPTY, not "brownpro", and that is deliberate: STheme::resolve()
// owns the default because it is the only place that knows whether this is a
// --test-case run (where the default is "system", so no committed pixel gate
// moves). A default spelled here as well would be a second authority that could
// disagree with it.
inline constexpr const char *UiTheme = "ui/theme";

// The Options dialog's last-shown page (AC-b1, 2026-08-21). Stored by NAME
// (the tree item's own label, e.g. "Audio"), never by top-level INDEX: the
// dialog ctor's own comment already warns that the tree and the stack are
// mapped by index and nothing else, so an index surviving in the INI across
// a version that inserted or removed a page would silently open a DIFFERENT
// page than the one the user actually left it on. A name that no longer
// matches any page (removed, or a stale/corrupt value) falls back to page 0
// exactly as an out-of-range explicit `initialPage` always has.
inline constexpr const char *OptionsLastPage = "ui/optionsLastPage";

// Plugin hosting (proposal 08 M2). PluginSearchPaths is a QStringList of
// directories, scanned recursively; its default is the FIRST platform-dependent
// default in this table and comes from the engine
// (audio::twPluginSearchPaths::defaults), so the standard per-OS locations are
// stated once. PluginScanOnStartup makes the app scan changed modules in the
// background at launch; the cache makes that cheap after the first run.
inline constexpr const char *PluginSearchPaths   = "plugins/searchPaths";
inline constexpr const char *PluginScanOnStartup = "plugins/scanOnStartup";

// MIDI (proposal 37 D6 / P7b). All three are per-user and machine-local, which
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

// MIDI RECORDING (proposal 21 L4 = 37 P8b, design D8). Both are GLOBAL and
// per-user, not per track: "how does a recorded pass combine with what is
// already there" and "quantise the input" are properties of how this person
// works, and every reference DAW puts them in one place beside the transport.
// They are read ONCE at each record start (SMidiRecorder), so a change made
// during a take cannot make the commit disagree with the capture.
//
//   MidiRecordMode      new-take | overdub | replace.
//                       new-take is the default and is what "record over that
//                       part again" means everywhere: the pass becomes a new
//                       TAKE on the column, and the previous one is still
//                       there to go back to. Overdub merges into the active
//                       take; replace drops the notes inside the recorded
//                       window first.
//   MidiRecordQuantize  off | 1/4 | 1/8 | 1/16 | 1/8t | 1/16t | ... - the
//                       `quantize-notes` grid spelling, so the input quantise
//                       and the edit verb can never disagree about what "1/8t"
//                       means. OFF by default: quantising a performance the
//                       user did not ask to have quantised is destructive, and
//                       the verb is one keystroke away afterwards.
inline constexpr const char *MidiRecordMode     = "midi/recordMode";
inline constexpr const char *MidiRecordQuantize = "midi/recordQuantize";

// TRANSPORT POLISH (proposal 21 L5). All three are GLOBAL and per-user rather
// than per project, for the reason the MIDI-recording pair are: "how loud do I
// want the click" and "how many bars do I want before a take" are properties of
// how this person works, not of the song. Whether the metronome is ON is a
// PROJECT property (`SProjectProps::Metronome`) and stays one - it travels with
// the arrangement and is on the transport bar.
//
//   MetronomeLevel      linear amplitude of the ACCENTED click, 0..1. An
//                       ordinary beat is 0.7 of it (the "1 kHz square, relative
//                       amplitude 0.7" beside the downbeat's "2 kHz square,
//                       relative amplitude 1.0" - twmetronome.h), so a case can
//                       assert the ladder in closed form.
//   CountInBars         bars of click BEFORE the record position, 0..8.
//                       Default 0: a count-in nobody asked for delays every
//                       take. The transport toolbar's metronome context menu's
//                       "Count in while recording" checkbox is a convenience
//                       on/off toggle over THIS SAME key (0 = unchecked), not
//                       a second flag - the spinbox already treats 0 as "off"
//                       everywhere else it is read.
//   PreRollBars         bars the transport ROLLS before the record position,
//                       0..8, with recording beginning at the locator.
//                       Default 0.
//   ClickWhileRecording whether the beat click sounds WHILE RECORDING
//                       specifically, independent of the metronome switch and
//                       of plain playback. Default ON (unchanged behaviour):
//                       unchecking it via the metronome context menu silences
//                       the click for the take without touching
//                       `SProjectProps::Metronome` (`sliveplanbuilder.h`'s
//                       `metronomeWanted`). A count-in is NOT affected - it is
//                       not "recording" yet (the playhead has not moved) and
//                       overrides both knobs by design.
inline constexpr const char *MetronomeLevel       = "metronome/level";
inline constexpr const char *CountInBars          = "transport/countInBars";
inline constexpr const char *PreRollBars          = "transport/preRollBars";
inline constexpr const char *ClickWhileRecording  = "metronome/clickWhileRecording";

// MEDIA BROWSER (proposal 38 gate 2, design §B.4). Per-user and machine-local
// for the reason every path in this table is: where this person keeps their
// samples is a property of this machine, not of the song, and a project file
// that carried a browse path would point at nothing on the next box.
//
//   MediaLastSourceId    which source the dock had selected. Restored WITHOUT
//                        contacting it -- a source is opened when it is
//                        SELECTED (§B.4), so a dock remembering a dead server
//                        costs a banner at the next click, never a hang at
//                        launch.
//   MediaLastPath        a PREFIX, not a key: the panel reads and writes
//                        "media/lastPath/<sourceId>", because two sources have
//                        two unrelated path spaces and one shared key would
//                        hand a WebDAV path to the local file system.
//   MediaCategoryMask    the media-type checkbox menu, as smedia::Category
//                        bits. Default Audio: the MVP registers no other
//                        category, and an empty mask means "no filter", which
//                        is not what an empty checkbox list should mean.
//   MediaSearchRecursive the recurse checkbox beside the search box.
//   MediaCacheCapMB      (gate 3) the LRU cap, in MB, on the content-addressed
//                        fetch cache under <configDir>/mediacache. 2048 by
//                        default. It is a CACHE policy and the open project is
//                        NOT a cache: SProject::externFiles() is subtracted
//                        before anything is evicted, and a cache that cannot
//                        reach its cap because the project pins too much logs
//                        that and stops (§B.6, trap T17). <= 0 means "no cap".
inline constexpr const char *MediaLastSourceId    = "media/lastSourceId";
inline constexpr const char *MediaLastPath        = "media/lastPath";
inline constexpr const char *MediaCategoryMask    = "media/categoryMask";
inline constexpr const char *MediaSearchRecursive = "media/searchRecursive";
inline constexpr const char *MediaCacheCapMB      = "media/cacheCapMB";

// Default value for a key (invalid QVariant if unknown). Scroll-first defaults.
QVariant def( const QString &key );

// Human-readable label for a wheel action (for combo boxes).
QString wheelActionLabel( WheelAction a );

}  // namespace SOpt

#endif // SOPTIONS_H
