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
 * volume labels.
 *
 * TREE FONT: the font a DOCK TITLE is drawn in -- the "Extern file list"
 * title the request names. That is `QApplication::font("QDockWidgetTitle")`,
 * which is exactly what QDockWidget itself uses (QDockWidgetPrivate::init sets
 * its title font from that class key, and paintEvent draws the title with it
 * while the dock's own font is the default). It comes from the PLATFORM THEME:
 * on macOS it is the smaller dock-title font, while Windows and Linux themes
 * set none, so there it equals the application font. Used by: the Extern file
 * list, the media browser's tree, the MIXER STRIP, and the
 * insert/instrument list (SPluginEffectStrip) in both the Track Detail dock
 * and the mixer.
 *
 * QBX-115 MOVED THE STRIP AND THE INSERT LIST FROM SMALL TO TREE, reversing
 * that part of QBX-102. The two requests want different things and both are
 * legitimate: QBX-102 wanted the mixer to AGREE with the track head, QBX-115
 * wants the mixer to be DENSE -- "the very small font as used in e.g. the
 * media browser or the resource list", so that "more channels [can] be
 * displayed on screen at once". Those two lists are the TREE mounts, so the
 * newer request names this function even though it does not name it.
 *
 * ON A PLATFORM THAT SETS NO DOCK-TITLE FONT the two are the same QFont and
 * the move is invisible; it shows on macOS, where QBX-115 was reported. This
 * is why every gate over these fonts also asserts Qt::WA_SetFont: equality
 * alone passes with the setFont() call deleted.
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
