
#include <qapplication.h>
#include <QFontDatabase>
#include <QFont>
#include "sapplication.h"
#include "smainwindow.h"

int main( int argc, char *argv[] )
{
    SApplication app( argc, argv );

    // Default UI font: the bundled FreeSans (embedded via resources/fonts.qrc),
    // rendered antialiased. If the font was not bundled into this build the
    // family stays the platform default; antialiasing is requested either way.
    {
        int fid = QFontDatabase::addApplicationFont(
            QStringLiteral( ":/fonts/FreeSans.ttf" ) );
        QFont uiFont = QApplication::font();
        if( fid != -1 ) {
            const QStringList fams = QFontDatabase::applicationFontFamilies( fid );
            if( !fams.isEmpty() ) uiFont.setFamily( fams.first() );
        }
        uiFont.setStyleStrategy( QFont::PreferAntialias );
        QApplication::setFont( uiFont );
    }

    SMainWindow *win = new SMainWindow();
    // Restored (un-maximized) geometry, then start maximized to fill the screen.
    win->move( 100,100 );
    win->resize( 800, 600 );
    win->showMaximized();
    app.exec();
    return 0;
}
