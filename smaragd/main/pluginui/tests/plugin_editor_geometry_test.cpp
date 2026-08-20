// Gate for SPluginNativeEditor::clampOntoAScreen() — proposal 33 D2.
//
// The failure this prevents is the classic multi-monitor one: a plugin editor
// restored at the position it had on a second screen that has since been
// unplugged, so it comes back off screen, unreachable, and — on Windows — with
// nothing at all reported. It cannot be reproduced by hand without physically
// unplugging a monitor, which is exactly why the clamp is a pure function of
// (rect, current screens) and exactly why it is public and static.
//
// Needs a QGuiApplication because it asks QScreen for the available geometry;
// runs under QT_QPA_PLATFORM=offscreen, which supplies one screen of a fixed
// size. Every expectation below is derived FROM that screen at runtime rather
// than hard-coded, so the test says the same thing whatever size the offscreen
// platform picks.

#include "app/pluginui/spluginnativeeditor.h"

#include <QGuiApplication>
#include <QRect>
#include <QScreen>

#include <iostream>

namespace {

int g_failures = 0;

void check( bool ok, const QString &what )
{
    if( ok ) {
        std::cout << "  ok   " << what.toStdString() << "\n";
        return;
    }
    std::cout << "  FAIL " << what.toStdString() << "\n";
    ++g_failures;
}

QString show( const QRect &r )
{
    return QStringLiteral( "%1,%2 %3x%4" )
        .arg( r.x() ).arg( r.y() ).arg( r.width() ).arg( r.height() );
}

}  // namespace

int main( int argc, char **argv )
{
    QGuiApplication app( argc, argv );

    QScreen *scr = QGuiApplication::primaryScreen();
    if( !scr ) {
        std::cout << "no screen; cannot run\n";
        return 1;
    }
    const QRect avail = scr->availableGeometry();
    std::cout << "=== clampOntoAScreen (available " << show( avail ).toStdString()
              << ") ===\n";

    // 1. A rect already comfortably inside the screen is returned UNCHANGED.
    //    Anything else would move a window the user had put where they wanted.
    {
        const QRect want( avail.x() + 40, avail.y() + 40, 200, 150 );
        const QRect got = SPluginNativeEditor::clampOntoAScreen( want );
        check( got == want, "a rect inside the screen is untouched: " + show( got ) );
    }

    // 2. THE UNPLUGGED-MONITOR CASE. Far off to the right of every screen: the
    //    SIZE survives and the window is pulled back until its right edge sits
    //    on the screen's.
    {
        const QRect want( avail.right() + 2000, avail.y() + 60, 300, 200 );
        const QRect got = SPluginNativeEditor::clampOntoAScreen( want );
        check( got.size() == want.size(), "off-screen right: the size is kept" );
        check( avail.contains( got ),
               "...and the whole window is back on the screen: " + show( got ) );
        check( got.right() == avail.right(),
               "...moved exactly far enough, not centred or reset" );
    }

    // 3. Negative coordinates — the same failure to the left and above, which is
    //    what a monitor arranged to the LEFT of the primary produces.
    {
        const QRect want( avail.x() - 1500, avail.y() - 900, 320, 240 );
        const QRect got = SPluginNativeEditor::clampOntoAScreen( want );
        check( got.size() == want.size(), "off-screen top-left: the size is kept" );
        check( got.topLeft() == avail.topLeft(),
               "...and it lands on the screen's own origin: " + show( got ) );
    }

    // 4. A rect LARGER than the screen is shrunk to fit, not left overflowing.
    //    A plugin that asked for 1600x1200 on a smaller display is better
    //    clipped by us — where the user can still resize — than by the window
    //    manager.
    {
        const QRect want( avail.x(), avail.y(),
                          avail.width() + 800, avail.height() + 600 );
        const QRect got = SPluginNativeEditor::clampOntoAScreen( want );
        check( got.width() == avail.width() && got.height() == avail.height(),
               "oversize is shrunk to the available screen: " + show( got ) );
        check( avail.contains( got ), "...and still fully inside it" );
    }

    // 5. An empty or invalid rect stays invalid. It must NOT come back as a
    //    plausible-looking zero-size rectangle at the screen origin: the caller
    //    tells "never stored" from "stored" by validity, and a valid 0x0 would
    //    resize a window to nothing.
    {
        check( !SPluginNativeEditor::clampOntoAScreen( QRect() ).isValid(),
               "a default-constructed rect stays invalid" );
        check( !SPluginNativeEditor::clampOntoAScreen( QRect( 10, 10, 0, 0 ) ).isValid(),
               "a zero-size rect stays invalid" );
        check( !SPluginNativeEditor::clampOntoAScreen( QRect( 10, 10, -5, 40 ) ).isValid(),
               "a negative-width rect stays invalid" );
    }

    if( g_failures ) {
        std::cout << "=== " << g_failures << " check(s) failed ===\n";
        return 1;
    }
    std::cout << "=== All tests passed ===\n";
    return 0;
}
