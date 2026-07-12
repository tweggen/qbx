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
inline constexpr const char *AudioDeviceId  = "audio/deviceId";

// Default value for a key (invalid QVariant if unknown). Scroll-first defaults.
QVariant def( const QString &key );

// Human-readable label for a wheel action (for combo boxes).
QString wheelActionLabel( WheelAction a );

}  // namespace SOpt

#endif // SOPTIONS_H
