#ifndef SDEFAULTRESET_H
#define SDEFAULTRESET_H

#include <functional>

class QWidget;

/**
 * "Double-click a control to put it back to its default."
 *
 * The gesture every DAW has for a fader, applied here to every value control
 * the two detail panes carry: the Track Detail fader, the generic plugin
 * editor's parameter sliders, and the Clip Detail pane's value fields (volume,
 * pan, pitch, stretch, formant shift, transpose, velocity scale).
 *
 * WHY A `std::function` RATHER THAN A VALUE. Each of those three call sites
 * commits differently and the difference is not cosmetic: the Track Detail
 * fader commits from QSlider::valueChanged, a plugin slider quantises through
 * its own tick helpers and may be inside an automation write pass, and the
 * Clip Detail spin boxes commit on editingFinished only (their standing rule:
 * valueChanged/textChanged fire while the user is still typing, so a
 * programmatic write must never be able to reach a commit through them). A
 * helper that "just set the value" would silently do nothing on the third of
 * those, which is the one a user is most likely to try. So each call site
 * hands over the restore it already knows how to perform, and this file owns
 * only the GESTURE.
 *
 * WHERE THIS LIVES. app/model is the lowest app layer and the only one both
 * app/timeline and app/pluginui can see (they are peers in app_ui and may not
 * include each other in that direction) — the same argument sclipcolors.h and
 * sclipwindowgeometry.h are here for. It reaches nothing in the model; it is a
 * Qt event filter and nothing more.
 */
namespace sdefaultreset {

/**
 * Run `restore` when `w` is double-clicked with the left button.
 *
 * The filter is parented to `w`, so it dies with the widget and a rebuilt
 * panel cannot accumulate them.
 *
 * TWO THINGS THIS DOES ON PURPOSE:
 *
 *  - it SWALLOWS the double-click. On a QSlider the second press would
 *    otherwise start another drag (or jump the handle to the click position,
 *    depending on the style), immediately moving the control off the default
 *    it was just asked for. SSMVMixerControl::eventFilter has swallowed it on
 *    the arranger's track-head fader for exactly this reason since long before
 *    this helper existed.
 *
 *  - it also watches a spin box's EMBEDDED LINE EDIT. QAbstractSpinBox does
 *    not see a double-click over its text at all — the child QLineEdit
 *    consumes it — so filtering the spin box alone leaves the gesture working
 *    on the arrows and dead over the number, which is where a user clicks. The
 *    cost is that double-click-to-select-a-word inside these fields becomes
 *    reset-to-default instead; that is the requested behaviour, and the text
 *    is still selectable by dragging, by Ctrl-A, and by tabbing into the field
 *    (QAbstractSpinBox selects all on focus).
 */
void onDoubleClick( QWidget *w, std::function<void()> restore );

}  // namespace sdefaultreset

#endif
