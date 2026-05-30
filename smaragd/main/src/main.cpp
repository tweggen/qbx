
#include <qapplication.h>
#include "sapplication.h"
#include "smainwindow.h"

int main( int argc, char *argv[] )
{
    SApplication app( argc, argv );
    SMainWindow *win = new SMainWindow();
    win->move( 100,100 );
    win->resize( 800, 600 );
    win->show();
    app.exec();
    return 0;
}
