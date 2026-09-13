#ifndef SUIFONTS_H
#define SUIFONTS_H

#include <QApplication>
#include <QFont>

/**
 * THE TWO NAMED UI FONTS (QBX-102). Every widget that is meant to share one of
 * these asks for it here, so the "same size as" requirement is one spelling
 * rather than a set of point sizes that happen to agree.
 *
 * SMALL FONT: the font the arranger TRACK HEAD draws a track's name in -- the
 * application font at 9 pt. It was a local `QFont smallFont` in
 * SSMVMixerControl's ctor, and the requirement ("the mixer uses the same size
 * as the track name in the track head") is only honest if the head and every
 * other mount read the same function. Used by: the track head's name and
 * volume labels, the mixer strip, and the insert/instrument list
 * (SPluginEffectStrip) in both the Track Detail dock and the mixer.
 *
 * TREE FONT: the font a DOCK TITLE is drawn in -- the "Extern file list"
 * title the request names. That is `QApplication::font("QDockWidgetTitle")`,
 * which is exactly what QDockWidget itself uses (QDockWidgetPrivate::init sets
 * its title font from that class key, and paintEvent draws the title with it
 * while the dock's own font is the default). It comes from the PLATFORM THEME:
 * on macOS it is the smaller dock-title font, while Windows and Linux themes
 * set none, so there it equals the application font. Used by: the Extern file
 * list and the media browser's tree.
 *
 * WHERE THIS LIVES. app/model is the lowest app layer and the only one
 * app/timeline, app/mixerui, app/pluginui and app/mediabrowser can all see --
 * the sdefaultreset.h / sclipcolors.h argument. It reaches nothing in the
 * model.
 */
namespace suifonts {

/// Point size of the small font. Named so a gate can assert the number, not
/// merely that two widgets agree with each other.
inline constexpr int SMALL_POINT_SIZE = 9;

inline QFont smallFont()
{
    QFont f = QApplication::font();
    f.setPointSize( SMALL_POINT_SIZE );
    return f;
}

inline QFont treeFont()
{
    return QApplication::font( "QDockWidgetTitle" );
}

}  // namespace suifonts

#endif // SUIFONTS_H
