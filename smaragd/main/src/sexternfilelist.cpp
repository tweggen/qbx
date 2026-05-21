
#include <QDebug>

#include "sobject.h"
#include "sexternfile.h"
#include "sexternfilelist.h"

void SExternFileList::externFileAdded( const SExternFile &ef )
{
    QString fileName = ef.getFileName();
    SExternFileItem *oldEFI = itemDict_.value( fileName );
    if( oldEFI ) {
        qWarning() << QString("SExternFileList::externFileAdded(): Invoked although file was "
                  "present: \"%1\".\n").arg(fileName);
        return;
    }    
    SExternFileItem *fi = new SExternFileItem( this, ef );
    QObject::connect( &ef, SIGNAL( nRefsChanged() ), 
                      this, SLOT( externFileRefChanged() ) );
    itemDict_.insert( fileName, fi );
}

void SExternFileList::externFileRemoved( const QString fileName )
{
    SExternFileItem *oldEFI = itemDict_.take( fileName );
    if( !oldEFI ) {
        qWarning() << QString( "SExternFileList::externFileAdded(): Invoked although file was "
                  "not found. \"%1\".\n" ).arg( fileName );
        return;
    }    
    QObject::disconnect( &(oldEFI->getExternFile()), SIGNAL( nRefsChanged() ), 
                      this, SLOT( externFileRefChanged() ) );
    delete oldEFI;
}

void SExternFileList::externFileRefChanged()
{
    SExternFile *snd = (SExternFile *) sender();
    QString fileName = snd->getFileName();
    SExternFileItem *oldEFI = itemDict_.value( fileName );
    if( !oldEFI ) {
        qWarning() << QString( "SExternFileList::externFileAdded(): Invoked although file was "
                  "not found. \"%1\".\n" ).arg( fileName );
        return;
    }    
    {
        QString s;
        s.setNum( snd->getNReferences(), 10 );
        oldEFI->setText( 2, s );
    }
}

SExternFileItem::SExternFileItem( SExternFileList *parent, const SExternFile &ef ) 
    : QTreeWidgetItem( parent ),
      externFile_( ef )
{
    qWarning( "Create new item.\n" );
    setText( 0, externFile_.getFileName() );
    setText( 1, externFile_.metaObject()->className() );
    {
        QString s;
        s.setNum( externFile_.getNReferences(), 10 );
        setText( 2, s );
    }
}

SExternFileList::~SExternFileList()
{
}

SExternFileList::SExternFileList( QWidget *parent, SProject &pro )
    : QTreeWidget( parent ),
      project_( pro )
{
    setColumnCount(3);
    setMinimumWidth(200);
    setMinimumHeight(100);
    QStringList headers = { "File name", "Type", "Refs" };
    setHeaderLabels(headers);
    // FIXME: Add a destroyed slot for the destroyed signal of the project.
    // FIXME: Add initially used extern files.
    QObject::connect( &project_, SIGNAL( externFileAdded( const SExternFile & ) ),
                      this, SLOT( externFileAdded( const SExternFile & ) ) );
    QObject::connect( &project_, SIGNAL( externFileRemoved( const QString ) ),
                      this, SLOT( externFileRemoved( const QString ) ) );
}
