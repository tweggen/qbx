#ifndef _SWINDOWGEOMETRY_H_
#define _SWINDOWGEOMETRY_H_

// Never relaunch into full screen (QBX-294).
//
// `ui/windowGeometry` holds what QWidget::saveGeometry() returns, and that blob
// carries the window's full-screen flag. Restored with restoreGeometry() and
// then show()n, it put Smaragd back into full screen at every start. On macOS
// the window had been flagged full screen without being a native AppKit
// full-screen Space, so the hover-to-reveal menu bar never came and the user
// had no way out short of deleting the key from the INI by hand.
//
// The rule: full screen is a state of THIS session, never of the next one. A
// window that was full screen comes back MAXIMIZED, which is what the
// first-run path shows. Two halves, each enforced separately:
//
//   save    -- withoutFullScreen() rewrites the blob before it is stored, so
//              no new INI ever carries the flag;
//   restore -- relaunchState() maps the state restoreGeometry() produced, so
//              an INI written before this fix (or by hand) is harmless too.
//
// Both are pure functions so that a unit test can gate them with no mapped
// window; SMainWindow::saveWindowLayout()/restoreWindowLayout() are their only
// product callers, and the `save-window-layout` / `restore-window-layout`
// testkit verbs gate those two call sites.

#include <QByteArray>
#include <QtGlobal>

namespace swindowgeometry {

// The state a relaunch must use for a window that restoreGeometry() left in
// `restored`: full screen becomes maximized, every other state is untouched.
inline Qt::WindowStates relaunchState( Qt::WindowStates restored )
{
    if( !( restored & Qt::WindowFullScreen ) ) return restored;
    return ( restored & ~Qt::WindowStates( Qt::WindowFullScreen ) )
           | Qt::WindowMaximized;
}

// The layout of QWidget::saveGeometry()'s blob, big-endian (QDataStream):
//
//   offset  0  quint32  magic 0x01D9D0CB
//           4  quint16  major version (1, 2, or 3 since Qt 6.3)
//           6  quint16  minor version
//           8  QRect    frameGeometry   (4 x qint32: left, top, right, bottom)
//          24  QRect    normalGeometry
//          40  qint32   screen number
//          44  quint8   windowState() & Qt::WindowMaximized   (0 or 0x02)
//          45  quint8   windowState() & Qt::WindowFullScreen  (0 or 0x04)
//          46  qint32   screen width          (version 2 and later)
//          50  QRect    geometry()            (version 3 and later)
//
// Both state bytes are present from version 1 on. window_geometry_test checks
// these offsets against a blob Qt itself writes, so a Qt that moved them fails
// the test rather than this function silently patching the wrong byte.
constexpr int kMagicOffset      = 0;
constexpr int kMajorOffset      = 4;
constexpr int kMaximizedOffset  = 44;
constexpr int kFullScreenOffset = 45;
constexpr quint32 kMagic        = 0x01D9D0CB;

inline quint32 readBE32( const QByteArray &b, int at )
{
    return ( quint32( quint8( b[at] ) ) << 24 ) | ( quint32( quint8( b[at + 1] ) ) << 16 )
         | ( quint32( quint8( b[at + 2] ) ) << 8 ) | quint32( quint8( b[at + 3] ) );
}

// True when `geo` is a geometry blob this code understands and its full-screen
// byte is set.
inline bool carriesFullScreen( const QByteArray &geo )
{
    if( geo.size() <= kFullScreenOffset ) return false;
    if( readBE32( geo, kMagicOffset ) != kMagic ) return false;
    const quint16 major = quint16( ( quint8( geo[kMajorOffset] ) << 8 )
                                   | quint8( geo[kMajorOffset + 1] ) );
    if( major < 1 || major > 3 ) return false;
    return geo[kFullScreenOffset] != 0;
}

// `geo` with the full-screen flag replaced by the maximized one, so that
// restoreGeometry() of the result asks for a maximized window. Anything else --
// a blob without the flag, an unknown version, garbage -- is returned
// unchanged, byte for byte.
inline QByteArray withoutFullScreen( const QByteArray &geo )
{
    if( !carriesFullScreen( geo ) ) return geo;
    QByteArray out = geo;
    out[kFullScreenOffset] = char( 0 );
    out[kMaximizedOffset]  = char( quint8( Qt::WindowMaximized ) );
    return out;
}

} // namespace swindowgeometry

#endif // _SWINDOWGEOMETRY_H_
