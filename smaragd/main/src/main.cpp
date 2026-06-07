
#include <qapplication.h>
#include "sapplication.h"
#include "smainwindow.h"

int main( int argc, char *argv[] )
{
    SApplication app( argc, argv );
    SMainWindow *win = new SMainWindow();
    // Restored (un-maximized) geometry, then start maximized to fill the screen.
    win->move( 100,100 );
    win->resize( 800, 600 );
    win->showMaximized();
    app.exec();
    return 0;
}
