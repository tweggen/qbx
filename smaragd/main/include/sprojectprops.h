#ifndef SPROJECTPROPS_H
#define SPROJECTPROPS_H

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

// Time-range marker in the ruler (the asset-from-range selection). Persisted so
// a selection survives save/load. Start/End are offset_t (sample frames), stored
// as qulonglong; only meaningful when RangeValid is true.
inline constexpr char RangeValid[]  = "rangeValid";   // bool
inline constexpr char RangeStart[]  = "rangeStart";   // qulonglong (offset_t)
inline constexpr char RangeEnd[]    = "rangeEnd";     // qulonglong (offset_t)

// Values a brand-new project starts with.
inline QVariantMap defaults()
{
    QVariantMap m;
    m[SnapToGrid]  = true;
    m[GridVisible] = true;
    m[Metronome]   = false;
    m[Cycle]       = false;
    m[RangeValid]  = false;
    m[RangeStart]  = (qulonglong) 0;
    m[RangeEnd]    = (qulonglong) 0;
    return m;
}

} // namespace SProjectProps

#endif // SPROJECTPROPS_H
