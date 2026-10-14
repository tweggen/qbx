#ifndef SPROJECTPROPS_H
#define SPROJECTPROPS_H

#include <QString>
#include <QVariantMap>

// Central registry of well-known per-project property keys and their defaults.
//
// Properties live in a generic QVariantMap on SProject (see prop()/setProp()),
// so new options can be added without touching SProject's interface. These
// constants keep the well-known keys discoverable and typo-proof; a fresh
// project is seeded from defaults().
namespace SProjectProps {

inline constexpr char SnapToGrid[]  = "snapToGrid";   // bool: clip times snap to grid
inline constexpr char GridVisible[] = "gridVisible";  // bool: draw the time grid
inline constexpr char Metronome[]   = "metronome";    // bool: metronome on (stub)
inline constexpr char Cycle[]       = "cycle";        // bool: cycle/loop on (stub)
inline constexpr char RulerMode[]   = "rulerMode";    // string: "bars" or "time" display format

// Time-range marker in the ruler (the asset-from-range selection). Persisted so
// a selection survives save/load. Start/End are offset_t (sample frames), stored
// as qulonglong; only meaningful when RangeValid is true.
inline constexpr char RangeValid[]  = "rangeValid";   // bool
inline constexpr char RangeStart[]  = "rangeStart";   // qulonglong (offset_t)
inline constexpr char RangeEnd[]    = "rangeEnd";     // qulonglong (offset_t)

// Timeline view state (the arranger canvas), fix/track-list-polish (m).
// Persisted so re-opening a project restores the zoom/pan the user left it
// at instead of resetting to the hard-coded defaults every load. Read/written
// by SMVActualView::loadViewStateFromProject()/saveViewStateToProject().
inline constexpr char TimelineZoomSecondWidth[] = "timelineZoomSecondWidth";
    // double: horizontal zoom, pixels per second (SMVActualView::secondWidth_)
inline constexpr char TimelineScrollX[] = "timelineScrollX";
    // qulonglong (offset_t): horizontal scroll position, project frames
    // (SMVActualView::upperLeftOffset_)

// Track Detail dock SECTIONS (QBX-102). One flag per section, shared by every
// track in the project and saved with it: collapsing the Feel Flow section on
// one track collapses it for all of them. Deliberately NOT seeded by defaults()
// below -- a key that was never set reads its default through
// trackDetailSectionCollapsedDefault(), so a project that nobody collapsed
// anything in writes no new keys and serializes exactly as before. View state:
// not an action, not undoable, and it does not mark the project dirty (the
// TimelineZoomSecondWidth precedent above).
inline constexpr char TrackDetailPluginsCollapsed[]  = "trackDetailPluginsCollapsed";   // bool, default false
inline constexpr char TrackDetailFeelFlowCollapsed[] = "trackDetailFeelFlowCollapsed";  // bool, default TRUE
inline constexpr char TrackDetailSlidersCollapsed[]  = "trackDetailSlidersCollapsed";   // bool, default false

// How a recording lands on a lane that already has material (plan 53, QBX-156).
// One of "takes" | "takes-split" | "overdub" | "replace". Resolved per lane:
// the track's own attribute, else the nearest ancestor folder track's, else
// this.
//
// DELIBERATELY NOT SEEDED BY defaults() BELOW, following the
// TrackDetail*Collapsed keys above and against plan 53 §5.2's wording. The
// reason is this project's own rule that a new serialized feature writes
// NOTHING when unused: `properties_` is initialised from defaults() and
// serialized WHOLESALE (QJsonObject::fromVariantMap), so a key added there
// appears in every project anyone saves from now on. Read the default through
// recordModeDefault() instead and a project nobody set a mode on writes no new
// key and re-serializes exactly as before.
//
// It also makes plan 53 §5.2's fileOpen hazard structurally impossible rather
// than merely avoided: there is no default to be merged over, so a loaded file
// without the key reads "takes" whatever the user's INI says.
inline constexpr char RecordMode[] = "recordMode";   // string

/// What a project that has never had a record mode set means: new-take comping.
inline const char *recordModeDefault() { return "takes"; }

/// Is this one of the four modes? Anything else is refused rather than
/// silently treated as the default -- a typo in a .qxp or a verb would
/// otherwise be invisible.
inline bool isRecordMode( const QString &m )
{
    return m == QLatin1String( "takes" )      || m == QLatin1String( "takes-split" )
        || m == QLatin1String( "overdub" )    || m == QLatin1String( "replace" );
}

// PLAYBACK AUTO-FADE at clip edges and comp boundaries, in MILLISECONDS
// (plan 53 Q9/P5, decision D3a (a)).
//
// DELIBERATELY NOT SEEDED BY defaults() BELOW, for a reason stronger than the
// RecordMode key's above: this one is AUDIBLE. `properties_` is initialised
// from defaults() and a load MERGES the file's keys over it
// (SProject::readPreChildrenAttributes), so a default of 3 ms here would be
// what EVERY PROJECT EVER SAVED reads -- no file written before plan 53 has
// the key -- and every one of them would render differently than it does
// today. That is D3a option (b), which the plan rejected precisely because it
// moves every byte-identity golden and every per-second RMS gate in the suite.
//
// So: the READ default is autoFadeMsDefault(), which is ZERO, and the 3 ms the
// feature promises is autoFadeMsSeed(), written once into a project at the
// moment it is CREATED (sautofade::seedInto, called only from the fresh-project
// paths -- never after a load, the srecordmodeseed.h rule). A new project
// fades; an old project is untouched; and nothing has to remember which,
// because the file says.
inline constexpr char AutoFadeMs[] = "autoFadeMs";   // double, milliseconds

/// What a project that has never had an auto-fade set means: NO auto-fade.
/// Every project file written before plan 53 is one of these.
inline double autoFadeMsDefault() { return 0.0; }

/// What a BRAND-NEW project is created with (D3a (a)): 3 ms, long enough to
/// kill the click of a hard edge and short enough to be inaudible as a fade.
inline double autoFadeMsSeed() { return 3.0; }

inline const char *trackDetailSectionKey( const QString &id )
{
    if( id == QLatin1String( "plugins" ) )  return TrackDetailPluginsCollapsed;
    if( id == QLatin1String( "feelflow" ) ) return TrackDetailFeelFlowCollapsed;
    if( id == QLatin1String( "sliders" ) )  return TrackDetailSlidersCollapsed;
    return nullptr;
}

/// The request's defaults: the whole Feel Flow section collapsed, the plugin
/// chain and the sliders expanded.
inline bool trackDetailSectionCollapsedDefault( const QString &id )
{
    return id == QLatin1String( "feelflow" );
}

// Values a brand-new project starts with.
inline QVariantMap defaults()
{
    QVariantMap m;
    m[SnapToGrid]  = true;
    m[GridVisible] = true;
    m[Metronome]   = false;
    m[Cycle]       = false;
    m[RulerMode]   = QString("bars");
    m[RangeValid]  = false;
    m[RangeStart]  = (qulonglong) 0;
    m[RangeEnd]    = (qulonglong) 0;
    m[TimelineZoomSecondWidth] = 30.0;
    m[TimelineScrollX]         = (qulonglong) 0;
    return m;
}

} // namespace SProjectProps

#endif // SPROJECTPROPS_H
