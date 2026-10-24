// Gate for SPluginNativeEditor's pure geometry statics — clampOntoAScreen()
// (proposal 33 D2), the plugin-units <-> Qt-units conversion pair
// hostSizeFor()/nativeSizeFor() (the Retina half-size fix, 2026-09-21), and
// the two reopen rules restoredGeometry()/sizeWithoutAnAnswer() (QBX-219).
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
#include <QSize>

#include <iostream>
#include <limits>

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

    // ---------------------------------------------------------------------
    // hostSizeFor() / nativeSizeFor(): the plugin's geometry units to Qt's.
    //
    // WHAT THIS PREVENTS: the bug found 2026-09-21, where a 944x712 iPlug2
    // editor came up in a 472x356 window on a 2x Retina Mac with its right and
    // bottom cropped off. twEditorSize used to be declared PHYSICAL PIXELS
    // ALWAYS, so this conversion divided by the dpr on every platform — but
    // VST3 and CLAP both state cocoa geometry in LOGICAL units and no backend
    // ever scaled it up, so macOS got divided by 2 for nothing.
    //
    // The expectation below is platform-split because the CONVENTION is: it
    // asserts the identity on macOS and the division everywhere else. Written
    // against the old code it fails on macOS, which is the point.
    std::cout << "=== hostSizeFor / nativeSizeFor ===\n";
    {
        const audio::twEditorSize plug{ 944, 712 };

        // dpr 1: every platform agrees, because there is nothing to scale.
        check( SPluginNativeEditor::hostSizeFor( plug, 1.0 ) == QSize( 944, 712 ),
               "at dpr 1 the plugin's size is the window's size" );

#if defined( Q_OS_MACOS )
        check( SPluginNativeEditor::hostSizeFor( plug, 2.0 ) == QSize( 944, 712 ),
               "macOS at dpr 2: IDENTITY — cocoa geometry is already logical" );
        check( SPluginNativeEditor::hostSizeFor( plug, 1.5 ) == QSize( 944, 712 ),
               "...and at any other dpr too; the ratio never enters into it" );
#else
        check( SPluginNativeEditor::hostSizeFor( plug, 2.0 ) == QSize( 472, 356 ),
               "Windows/X11 at dpr 2: physical pixels divide down to logical" );
        check( SPluginNativeEditor::hostSizeFor( plug, 1.5 ) == QSize( 629, 475 ),
               "...and a fractional ratio rounds rather than truncating" );
#endif

        // A widget with no window yet reports dpr 0. It is not a scale, and
        // dividing by it would be a window of infinite size.
        check( SPluginNativeEditor::hostSizeFor( plug, 0.0 ) == QSize( 944, 712 ),
               "a dpr of 0 (no window yet) is treated as 1, not divided by" );

        // THE ROUND TRIP, which is what keeps resizeEvent() from arguing with
        // the plugin one poll at a time: the two conversions have to be
        // inverses. EXACTLY so at an integral ratio (and at every ratio on
        // macOS, where both are the identity); within one unit at a fractional
        // one, because an odd extent divided by 1.5 and rounded cannot come
        // back to itself — 712/1.5 rounds to 475 and 475*1.5 rounds to 713.
        // That residue is pre-existing, is Windows/X11 only, and is why the
        // tolerance is stated rather than the test written around it.
        for( const qreal dpr : { 1.0, 2.0 } ) {
            const QSize host = SPluginNativeEditor::hostSizeFor( plug, dpr );
            const audio::twEditorSize back =
                SPluginNativeEditor::nativeSizeFor( host, dpr );
            check( back.width == plug.width && back.height == plug.height,
                   QStringLiteral( "round trip at dpr %1 lands back on 944x712" )
                       .arg( dpr ) );
        }
        {
            const QSize host = SPluginNativeEditor::hostSizeFor( plug, 1.5 );
            const audio::twEditorSize back =
                SPluginNativeEditor::nativeSizeFor( host, 1.5 );
            check( qAbs( back.width - plug.width ) <= 1 &&
                       qAbs( back.height - plug.height ) <= 1,
                   QStringLiteral( "round trip at dpr 1.5 lands within 1 of "
                                   "944x712 (%1x%2)" )
                       .arg( back.width ).arg( back.height ) );
        }
    }

    // ---------------------------------------------------------------------
    // isSendableScale(): whether we have a monitor scale worth telling a plugin.
    //
    // WHAT THIS PREVENTS: QBX-119. attachPlugin() reads devicePixelRatioF()
    // BEFORE the dialog has ever been mapped, and a widget with no window and no
    // screen behind it reports 0. An iPlug2/Skia editor handed a content scale of
    // 0 sets a CAMetalLayer contentsScale of 0, so its drawable comes out 0x0
    // however big the view is, nextDrawable answers nil, and the plugin
    // dereferences the null SkCanvas that follows -- an EXC_BAD_ACCESS in
    // SkCanvas::restoreToCount, inside the plugin, where the host cannot catch
    // it. The only safe host behaviour is not to make the call.
    std::cout << "=== isSendableScale ===\n";
    {
        check( SPluginNativeEditor::isSendableScale( 1.0 ),
               "1.0 is a scale" );
        check( SPluginNativeEditor::isSendableScale( 2.0 ),
               "2.0 is a scale" );
        check( SPluginNativeEditor::isSendableScale( 1.5 ),
               "a fractional ratio is a scale" );
        check( SPluginNativeEditor::isSendableScale( 0.5 ),
               "a ratio below 1 is still a scale: QT_SCALE_FACTOR may be < 1" );

        // THE ONE THAT SHIPPED.
        check( !SPluginNativeEditor::isSendableScale( 0.0 ),
               "0 is NOT a scale -- it is a widget with no window yet" );

        check( !SPluginNativeEditor::isSendableScale( -2.0 ),
               "a negative ratio is not a scale" );

        // NaN is the reason this is a named function rather than `scale > 0` at
        // the call site. Every comparison against NaN is false, so a guard
        // written as the negation of a rejection -- `if( !(scale <= 0) ) send` --
        // passes it straight through to the plugin.
        check( !SPluginNativeEditor::isSendableScale(
                   std::numeric_limits<qreal>::quiet_NaN() ),
               "NaN is not a scale, however the guard is spelled" );
        check( !SPluginNativeEditor::isSendableScale(
                   std::numeric_limits<qreal>::infinity() ),
               "infinity is not a scale" );
        check( !SPluginNativeEditor::isSendableScale(
                   -std::numeric_limits<qreal>::infinity() ),
               "negative infinity is not a scale" );
    }

    // ---------------------------------------------------------------------
    // restoredGeometry(): the CLIENT rectangle a reopened window is given
    // (QBX-219).
    //
    // WHAT THIS PREVENTS: a fixed-size editor walking down the screen one title
    // bar per reopen. saveGeometryToSettings() stores geometry(), the client
    // rectangle; the restore used move(), which places the FRAME, so every
    // reopen came back 31 px lower and every close saved the lower spot. The
    // drift itself needs a mapped window to exist at all -- a window the suite
    // never maps has no frame -- so what is gated here is the rule, and that
    // the result goes through setGeometry() is restoreGeometryFromSettings()'s
    // one line. What the rule must say: the stored POSITION, untouched, for a
    // window that is already on screen; the stored size only if the plugin can
    // be resized.
    std::cout << "=== restoredGeometry ===\n";
    {
        const QRect stored( avail.x() + 50, avail.y() + 70, 300, 200 );
        const QSize pluginSize( 240, 160 );

        const QRect fixed =
            SPluginNativeEditor::restoredGeometry( stored, false, pluginSize );
        check( fixed.topLeft() == stored.topLeft(),
               "fixed-size: the stored client position is kept exactly: " + show( fixed ) );
        check( fixed.size() == pluginSize,
               "...at the size the plugin asked for, not the stored one" );

        const QRect resizable =
            SPluginNativeEditor::restoredGeometry( stored, true, pluginSize );
        check( resizable == stored,
               "resizable: the stored client rectangle is kept whole: " + show( resizable ) );

        // The clamp sees the size the window will really have. A fixed-size
        // window stored near the right edge at a SMALL size, now asked for at
        // a bigger one, must be pulled back far enough for the bigger one.
        const QRect nearEdge( avail.right() - 120, avail.y() + 20, 100, 100 );
        const QRect pulled =
            SPluginNativeEditor::restoredGeometry( nearEdge, false, QSize( 300, 200 ) );
        check( avail.contains( pulled ) && pulled.size() == QSize( 300, 200 ),
               "fixed-size near the edge: clamped at its REAL size: " + show( pulled ) );

        check( !SPluginNativeEditor::restoredGeometry( QRect(), false, pluginSize ).isValid(),
               "nothing stored: nothing restored" );
    }

    // ---------------------------------------------------------------------
    // sizeWithoutAnAnswer(): what a window gets when its plugin reports an
    // invalid size on the window's first sizing (QBX-219). Every iPlug2 plugin
    // on the dev box answers 0x0 from its second attach on; the window used to
    // be shown at exactly that.
    std::cout << "=== sizeWithoutAnAnswer ===\n";
    {
        const QSize lastGood( 944, 714 ), stored( 800, 600 );
        check( SPluginNativeEditor::sizeWithoutAnAnswer( lastGood, stored ) == lastGood,
               "the plugin's own last good size comes first" );
        check( SPluginNativeEditor::sizeWithoutAnAnswer( QSize(), stored ) == stored,
               "...then the remembered window size" );
        check( SPluginNativeEditor::sizeWithoutAnAnswer( QSize( 0, 0 ), QSize( 0, 0 ) )
                   == QSize( 640, 480 ),
               "...and with neither, a default -- never an empty window" );
        const QSize none = SPluginNativeEditor::sizeWithoutAnAnswer( QSize(), QSize() );
        check( none.width() > 0 && none.height() > 0,
               "the answer is never empty" );
    }

    if( g_failures ) {
        std::cout << "=== " << g_failures << " check(s) failed ===\n";
        return 1;
    }
    std::cout << "=== All tests passed ===\n";
    return 0;
}
