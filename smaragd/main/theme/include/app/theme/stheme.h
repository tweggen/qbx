#ifndef STHEME_H
#define STHEME_H

#include <QString>
#include <QStringList>

class QApplication;

// Choosing and installing the application's widget style.
//
// This is deliberately a LEAF: it depends on no other app module, not even on
// SSettings or SOpt. The caller reads the persisted preference and hands it in,
// which is what keeps `theme` free of the app's one big dependency cycle and
// what lets a unit test resolve a name without an INI, an SApplication or a
// window.
namespace STheme {

// The theme names resolve() and apply() understand.
//   "brownpro" — SBrownProStyle, the shipped look.
//   "system"   — install nothing; keep whatever style Qt picked for the
//                platform. The escape hatch for a user on a platform theme or
//                an accessibility setting we have not accounted for.
QStringList available();

// Resolve which theme to install, LAST WINS:
//
//     compile default  ->  `configured` (the persisted option)  ->  environment
//
// `configured` may be empty (nothing persisted). The environment variable is
// SMARAGD_UI_THEME, which follows the house pattern of the other runtime
// backend knobs (SMARAGD_AUDIO_BACKEND, SMARAGD_MIDI_BACKEND,
// SMARAGD_SECRET_BACKEND) — read once, ahead of the platform choice.
//
// `testCase` flips the COMPILE DEFAULT to "system", and that is load-bearing
// rather than tidy. A qxa case that grabs pixels (assert-track-head,
// assert-lane-alignment, meter_levels' head PNGs, folder_sum_preview's canvas
// gate) is asserting against widget chrome this style repaints, so making the
// theme the headless default would move a large number of committed pixel
// expectations in one commit — with no theme gate to say which of those moves
// were intended. A future theme case sets SMARAGD_UI_THEME=brownpro explicitly,
// which still wins here.
//
// An unknown name resolves to the default rather than to nothing, so a typo in
// the INI cannot leave the app unstyled with no explanation.
QString resolve( const QString &configured, bool testCase );

// Install `name` on `app`. Returns the name actually installed, which differs
// from `name` when it is unknown.
//
// The palette travels WITH the style (SBrownProStyle::polish(QPalette&)), so
// there is no second QApplication::setPalette() call to keep in step and no
// palette left behind when the style is swapped back out.
QString apply( QApplication &app, const QString &name );

} // namespace STheme

#endif // STHEME_H
