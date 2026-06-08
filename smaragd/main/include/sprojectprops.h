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
inline constexpr char RulerMode[]   = "rulerMode";    // string: "bars" or "time" display format

// Values a brand-new project starts with.
inline QVariantMap defaults()
{
    QVariantMap m;
    m[SnapToGrid]  = true;
    m[GridVisible] = true;
    m[Metronome]   = false;
    m[Cycle]       = false;
    m[RulerMode]   = QString("bars");
    return m;
}

} // namespace SProjectProps

#endif // SPROJECTPROPS_H
