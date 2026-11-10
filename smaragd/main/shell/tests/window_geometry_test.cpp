// Never relaunch into full screen (QBX-294).
//
// `ui/windowGeometry` stores QWidget::saveGeometry() and that blob carries the
// full-screen flag; restored and shown, it put Smaragd back into full screen at
// every start, on macOS with no menu bar and no way out. This gates the two
// pure functions of app/shell/swindowgeometry.h:
//
//   * that the byte offsets withoutFullScreen() patches are the ones Qt writes
//     (asked of Qt itself, on a blob saveGeometry() produced);
//   * that a patched blob restores MAXIMIZED and not full screen, through Qt's
//     own restoreGeometry();
//   * that every other blob passes through byte for byte;
//   * that the blob in the field report (full-screen byte 0x04, maximized byte
//     0, frame 0,26 to 1279,827) restores full screen as it stands and
//     maximized once patched;
//   * relaunchState()'s mapping.
//
// Nothing is shown: a widget that was never mapped still reports the state
// setWindowState() and restoreGeometry() gave it, and the state is the claim.
// The CALL SITES in SMainWindow are gated by window_layout_fullscreen.qxa.

#include "app/shell/swindowgeometry.h"

#include <QApplication>
#include <QScreen>
#include <QByteArray>
#include <QDataStream>
#include <QIODevice>
#include <QRect>
#include <QString>
#include <QWidget>

#include <iostream>

static int gFailures = 0;

static bool check( bool ok, const QString &what )
{
    std::cout << ( ok ? "  ok   " : "  FAIL " ) << what.toStdString() << std::endl;
    if( !ok ) ++gFailures;
    return ok;
}

// The state a fresh, never-shown widget is in after restoreGeometry( blob ).
static Qt::WindowStates restoredState( const QByteArray &blob, bool *ok = nullptr )
{
    QWidget w;
    const bool r = w.restoreGeometry( blob );
    if( ok ) *ok = r;
    return w.windowState();
}

static bool isMaximizedOnly( Qt::WindowStates s )
{
    return ( s & Qt::WindowMaximized ) && !( s & Qt::WindowFullScreen );
}

// The field report's blob, reconstructed in the version-3 layout Qt 6 writes:
// frame (and normal and client) geometry 0,26 to 1279,827 -- the screen's
// available area below the menu bar -- on screen 0, maximized byte 0 and
// full-screen byte 0x04. The screen-width field is a parameter: the report's
// machine had a 1280-wide screen, and Qt's restoreGeometry() REFUSES a whole
// blob whose stored screen width differs from the current screen's by more
// than 0.8..1.25 (its high-DPI sanity check), so on a box with another screen
// the verbatim blob never reaches the state code at all.
static QByteArray ticketBlob( qint32 screenWidth )
{
    QByteArray b;
    QDataStream s( &b, QIODevice::WriteOnly );
    s.setVersion( QDataStream::Qt_4_0 );
    const QRect frame( QPoint( 0, 26 ), QPoint( 1279, 827 ) );
    s << quint32( 0x01D9D0CB ) << quint16( 3 ) << quint16( 0 )
      << frame << frame << qint32( 0 )
      << quint8( 0 ) << quint8( 0x04 )
      << screenWidth << frame;
    return b;
}

int main( int argc, char **argv )
{
    QApplication app( argc, argv );
    using namespace swindowgeometry;

    // ---- 1: THE OFFSETS, ASKED OF QT ---------------------------------------
    std::cout << "=== the blob layout Qt writes ===" << std::endl;
    QByteArray normalBlob, maxBlob, fullBlob;
    {
        QWidget w;
        w.setGeometry( 100, 120, 640, 480 );
        normalBlob = w.saveGeometry();
        w.setWindowState( Qt::WindowMaximized );
        maxBlob = w.saveGeometry();
        w.setWindowState( Qt::WindowFullScreen );
        fullBlob = w.saveGeometry();
    }
    check( fullBlob.size() > kFullScreenOffset, "the blob reaches the full-screen byte" );
    check( readBE32( fullBlob, kMagicOffset ) == kMagic, "magic at offset 0" );
    check( quint8( fullBlob[kMajorOffset] ) == 0 && quint8( fullBlob[kMajorOffset + 1] ) == 3,
           "format version 3 at offset 4" );
    check( quint8( fullBlob[kFullScreenOffset] ) == 0x04, "full screen: byte 45 is 0x04" );
    check( quint8( normalBlob[kFullScreenOffset] ) == 0, "windowed: byte 45 is 0" );
    check( quint8( normalBlob[kMaximizedOffset] ) == 0, "windowed: byte 44 is 0" );
    check( quint8( maxBlob[kMaximizedOffset] ) == 0x02, "maximized: byte 44 is 0x02" );
    check( quint8( maxBlob[kFullScreenOffset] ) == 0, "maximized: byte 45 is 0" );
    check( carriesFullScreen( fullBlob ), "carriesFullScreen(full-screen blob)" );
    check( !carriesFullScreen( normalBlob ) && !carriesFullScreen( maxBlob ),
           "!carriesFullScreen(windowed and maximized blobs)" );

    // ---- 2: THE PATCH, JUDGED BY QT'S OWN RESTORE --------------------------
    std::cout << "=== a patched full-screen blob restores maximized ===" << std::endl;
    {
        check( ( restoredState( fullBlob ) & Qt::WindowFullScreen ) != 0,
               "unpatched, Qt restores full screen (the defect)" );
        const QByteArray patched = withoutFullScreen( fullBlob );
        check( !carriesFullScreen( patched ), "the patched blob carries no full-screen flag" );
        check( patched.size() == fullBlob.size(), "...and keeps its length" );
        bool ok = false;
        const Qt::WindowStates s = restoredState( patched, &ok );
        check( ok, "Qt accepts the patched blob" );
        check( isMaximizedOnly( s ), "the patched blob restores maximized, not full screen" );
        // Only the two state bytes moved.
        int diffs = 0;
        for( int i = 0; i < patched.size(); ++i )
            if( patched[i] != fullBlob[i]
                && i != kFullScreenOffset && i != kMaximizedOffset ) ++diffs;
        check( diffs == 0, "no byte but 44 and 45 changed" );
    }

    // ---- 3: EVERYTHING ELSE PASSES THROUGH ---------------------------------
    std::cout << "=== other blobs are untouched ===" << std::endl;
    {
        check( withoutFullScreen( normalBlob ) == normalBlob, "windowed blob unchanged" );
        check( withoutFullScreen( maxBlob ) == maxBlob, "maximized blob unchanged" );
        check( withoutFullScreen( QByteArray() ).isEmpty(), "empty blob unchanged" );
        QByteArray wrongMagic = fullBlob;
        wrongMagic[0] = char( 0x7F );
        check( withoutFullScreen( wrongMagic ) == wrongMagic, "a blob with another magic unchanged" );
        QByteArray wrongVersion = fullBlob;
        wrongVersion[kMajorOffset + 1] = char( 9 );
        check( withoutFullScreen( wrongVersion ) == wrongVersion, "an unknown version unchanged" );
        check( withoutFullScreen( fullBlob.left( kFullScreenOffset ) )
                   == fullBlob.left( kFullScreenOffset ),
               "a truncated blob unchanged" );
        check( ( restoredState( normalBlob ) & ( Qt::WindowMaximized | Qt::WindowFullScreen ) ) == 0,
               "a windowed blob still restores windowed" );
        check( isMaximizedOnly( restoredState( maxBlob ) ), "a maximized blob still restores maximized" );
    }

    // ---- 4: THE FIELD REPORT'S BLOB ----------------------------------------
    std::cout << "=== the blob from the report ===" << std::endl;
    {
        // Verbatim, on whatever screen this runs: Qt either refuses it (the
        // screen-width check; restoreWindowLayout() then reports nothing
        // restored and main.cpp takes the maximized first-run path) or
        // restores it full screen, which the patch must turn into maximized.
        {
            const QByteArray v = ticketBlob( 1280 );
            bool vok = false;
            const Qt::WindowStates vs = restoredState( v, &vok );
            std::cout << "  (verbatim blob: Qt " << ( vok ? "accepts" : "refuses" )
                      << " it on this " << QGuiApplication::primaryScreen()->geometry().width()
                      << "-wide screen)" << std::endl;
            if( vok ) check( ( vs & Qt::WindowFullScreen ) != 0,
                             "verbatim, it restores full screen" );
            bool pok = false;
            const Qt::WindowStates ps = restoredState( withoutFullScreen( v ), &pok );
            check( pok == vok, "patching does not change whether Qt accepts it" );
            if( pok ) check( isMaximizedOnly( ps ), "verbatim and patched, it restores maximized" );
        }
        // ...and with this screen's width in the width field, so Qt's sanity
        // check passes and the state bytes are what decides.
        const QByteArray t = ticketBlob( QGuiApplication::primaryScreen()->geometry().width() );
        check( t.size() == 66, "the reconstructed blob is Qt 6's 66 bytes" );
        check( t.size() == fullBlob.size(), "...the same length Qt writes here" );
        check( carriesFullScreen( t ), "it carries the full-screen flag" );
        bool ok = false;
        const Qt::WindowStates before = restoredState( t, &ok );
        check( ok, "Qt accepts it" );
        check( ( before & Qt::WindowFullScreen ) != 0, "as stored, it restores full screen" );
        check( isMaximizedOnly( restoredState( withoutFullScreen( t ) ) ),
               "patched, it restores maximized" );
        check( isMaximizedOnly( relaunchState( before ) ),
               "relaunchState() of what it restored is maximized" );
    }

    // ---- 5: relaunchState() -------------------------------------------------
    std::cout << "=== relaunchState ===" << std::endl;
    {
        check( relaunchState( Qt::WindowFullScreen ) == Qt::WindowMaximized,
               "full screen -> maximized" );
        check( relaunchState( Qt::WindowFullScreen | Qt::WindowMaximized ) == Qt::WindowMaximized,
               "full screen + maximized -> maximized" );
        check( relaunchState( Qt::WindowNoState ) == Qt::WindowNoState, "no state unchanged" );
        check( relaunchState( Qt::WindowMaximized ) == Qt::WindowMaximized, "maximized unchanged" );
        check( relaunchState( Qt::WindowMinimized ) == Qt::WindowMinimized, "minimized unchanged" );
    }

    std::cout << ( gFailures ? "FAILED: " : "PASSED" );
    if( gFailures ) std::cout << gFailures << " check(s)";
    std::cout << std::endl;
    return gFailures ? 1 : 0;
}
