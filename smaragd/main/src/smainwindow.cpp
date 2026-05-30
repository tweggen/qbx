
#include <stdlib.h>

#include <QDebug>
#include <qmessagebox.h>
#include <qaction.h>
#include <qtoolbar.h>
#include <QDockWidget>
#include <qfile.h>
#include <qfiledialog.h>

#include <iostream>

#include "sapplication.h"
#include "smainwindow.h"
#include "sobject.h"
#include "sproject.h"
#include "sexternfilelist.h"
#include "slink.h"
#include "sprojectloader.h"

#include "sstdmixer.h"

#include "twspeaker.h"

#include "pix/playoff.xpm"
#include "pix/playon.xpm"
#include "pix/stopoff.xpm"

using namespace std;

void SMainWindow::nyi()
{
    QMessageBox::information( NULL, "Smaragd warning", 
                              "This feature is not yet implemented.",
                              "OK" );
}

void SMainWindow::destroyDocksToolbars()
{
    // No more extern files here.
    delete qDockExternFileList_;
    qDockExternFileList_ = NULL;
}

void SMainWindow::createDocksToolbars()
{
    // Create the extern file list.
    qDockExternFileList_ = new QDockWidget( "Extern file list" );
    SExternFileList *efl = new SExternFileList( qDockExternFileList_, *currentProject_ );
    qDockExternFileList_->setWidget(efl);
    addDockWidget( Qt::LeftDockWidgetArea, qDockExternFileList_ );
}

void SMainWindow::fileSave()
{
    QFile f("project.qxp");
    if ( f.open(QIODevice::WriteOnly) ) {    // file opened successfully
        {
            QTextStream t( &f );        // use a text stream
            currentProject_->serialize( t );
        }
        f.close();
    }    
}

void SMainWindow::fileNew()
{
    // FIXME: Delete the old component
    closeProject();

    currentProject_ = new SProject();

    createDocksToolbars();    
    // (void) efl;
    // qTBExternFileList_->show();

    // Create default main component for mixing. Could instantiate a wave view later here.
    currentProject_->setRootComponent( new SStdMixer( currentProject_ ) );    
    // Find out the main widget.
    // We do have a root component here as we assigned it before.
    projectRootWidget_ = currentProject_->getRootComponent()->getDetailEditWidget( this );

    setCentralWidget( projectRootWidget_ );
    projectRootWidget_->show();
    SApplication::app().setCurrentProject( currentProject_ );

}

void SMainWindow::fileOpen()
{
    // FIXME: Delete the old component
    closeProject();
//    newProject();
    QString fileName( 
        QFileDialog::getOpenFileName( 
            this,
            "Open Project",
            QDir::currentPath(),
            "qbx Projects (*.qxp *.QXP)" ) ); 
    if( fileName.isNull() ) {
        qWarning( "Nothing selected in file requester.\n" );
        return;
    }

    // Now, as the reading proceeded, create an empty project to fill in the data.
    currentProject_ = new SProject();    
    SApplication::app().setCurrentProject( currentProject_ );

    // OK, try to load the document.
    SProjectLoader loader( *currentProject_, fileName );
    if( !loader.wasLoaded() ) {
        QMessageBox::information( NULL, "Smaragd warning", 
                                  "Unable to open specified project file.",
                                  "OK" );
        return;
    }

    // Now, that we got all data, build up the objects.

    createDocksToolbars();

    int err = loader.createObjects( *currentProject_ );
    if( err ) {
        closeProject();
    }    
    
    // Find out the main widget.
    // We do have a root component here as we assigned it before.
    projectRootWidget_ = currentProject_->getRootComponent()->getDetailEditWidget( this );

    setCentralWidget( projectRootWidget_ );
    projectRootWidget_->show();
    SApplication::app().setCurrentProject( currentProject_ );
    

}

void SMainWindow::fileExit()
{
    ::exit( 0 );
}

void SMainWindow::newProject()
{
#if 0
    currentProject_ = new SProject();
    projectRootWidget_ = currentProject_->getRootComponent()->getDetailEditWidget( this );
    setCentralWidget( projectRootWidget_ );
    SApplication::app().setCurrentProject( currentProject_ );
    qTBExternFileList = new QToolBar( "Extern file list", this, Left );
    SExternFileList *efl = new SExternFileList( qTBExternFileList, *currentProject_ );
    qTBExternFileList->show();
#endif
}

void SMainWindow::closeProject()
{
    if( !currentProject_ ) return;
    SApplication::app().setCurrentProject( NULL );
    delete projectRootWidget_;
    delete currentProject_;    
    destroyDocksToolbars();
    currentProject_ = NULL;
}

void SMainWindow::startPlaying()
{
    qWarning() << "startPlaying(): Called." << Qt::endl;
    if( !currentProject_ ) return;
    if( SApplication::app().isPlaying() ) {
        qWarning() << "startPlaying(): Ought to stop." << Qt::endl;
        SApplication::app().getSpeaker()->stopOutput();        
        actPlay_->setIcon( QIcon( QPixmap( (const char **)playoff_xpm ) ) );
        SApplication::app().setPlaying( false );
    } else {
        qWarning() << "startPlaying(): Ought to start." << Qt::endl;
        // FIXME: Add myselves as listener of the root component.
        SObject *root = currentProject_->getRootComponent();
        if( !root ) return;
        qWarning() << "startPlaying(): Preparing start." << Qt::endl;
        root->seekTo( SApplication::app().getGlobalLocatorPos() );
        SApplication::app().getSpeaker()->startOutput();
        actPlay_->setIcon( QIcon( QPixmap( (const char **)playon_xpm ) ) );
        SApplication::app().setPlaying( true );
    }
} 

void SMainWindow::stopPlaying()
{
    if( !currentProject_ ) return;
    if( SApplication::app().isPlaying() ) {
        SApplication::app().getSpeaker()->stopOutput();
        SApplication::app().setPlaying( false );
    } else {
        SObject *root = currentProject_->getRootComponent();
        if( !root ) return;        
        // FIXME: Jump to left locator here.
        SApplication::app().setGlobalLocatorPos( 0 );
        //  currentProject_->getRootComponent().seekTo( 0 );
    }
    actPlay_->setIcon( QIcon( QPixmap( (const char **)playoff_xpm ) ) );
}

SMainWindow::SMainWindow()
    : QMainWindow(),
      currentProject_( 0 ),
      projectRootWidget_( NULL )
{
    actPlay_ = new QAction( 
        QIcon( QPixmap( (const char **)playoff_xpm )),
        "Start playing",
        this);
    actPlay_->setShortcut(Qt::Key_Space);
    // actPlay_->setMenuText("Start");

    actStop_ = new QAction(
        QIcon( QPixmap( (const char **)stopoff_xpm )),
        "Stop playing",
        this);
    actStop_->setShortcut(Qt::Key_0);
    // actStop_->setMenuText("Stop");
    /*new QAction( "Stop playing", QIcon( QPixmap( "images/player_stop.png" ) ),
                            "Stop", Qt::Key_0, this );*/
    qTBTransport_ = new QToolBar( "Transport" /*, this*/ );
    /*
    actPlay_->addTo( qTBTransport );
    actStop_->addTo( qTBTransport );
    */
    qTBTransport_->addAction( actPlay_ );
    qTBTransport_->addAction( actStop_ );
    addToolBar( Qt::TopToolBarArea, qTBTransport_ );

    QObject::connect( actPlay_, SIGNAL( triggered() ), 
                      this, SLOT( startPlaying() ) );
    QObject::connect( actStop_, SIGNAL( triggered() ), 
                      this, SLOT( stopPlaying() ) );

    qFileMenu_ = new QMenu( "&File", this );
    qFileMenu_->setTearOffEnabled(true);
    qFileMenu_->addAction( "&New...", this, SLOT( fileNew() ), Qt::CTRL | Qt::Key_N );
    qFileMenu_->addAction( "&Open...", this, SLOT( fileOpen() ) );
    qFileMenu_->addAction( "&Save", this, SLOT( fileSave() ), Qt::CTRL | Qt::Key_S );
    qFileMenu_->addAction( "Save &as", this, SLOT( nyi() ) );
    qFileMenu_->addSeparator();
    qFileMenu_->addAction( "&Close",  this, SLOT( nyi() ), Qt::CTRL | Qt::Key_W );  
    qFileMenu_->addSeparator();
    qFileMenu_->addAction( "E&xit", this, SLOT( fileExit() ), Qt::CTRL | Qt::Key_Q );    
    menuBar()->addMenu( qFileMenu_ );
    qDockExternFileList_ = NULL;
}

SMainWindow::~SMainWindow()
{
}

