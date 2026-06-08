
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

void SExternFileList::assetAdded( const QString &name, SObject &body )
{
    if( assetItemDict_.contains( name ) ) {
        qWarning() << QString( "SExternFileList::assetAdded(): asset already "
                               "present: \"%1\"." ).arg( name );
        return;
    }
    QTreeWidgetItem *item = new QTreeWidgetItem( this );
    item->setText( 0, name );
    item->setText( 1, "Asset" );
    item->setText( 2, QString::number( body.getNReferences() ) );
    assetItemDict_.insert( name, item );
    assetNameByBody_.insert( &body, name );
    QObject::connect( &body, SIGNAL( nRefsChanged() ),
                      this, SLOT( assetRefChanged() ) );
}

void SExternFileList::assetRemoved( const QString &name )
{
    QTreeWidgetItem *item = assetItemDict_.take( name );
    if( !item ) return;
    // Drop the body->name reverse entry and its ref-count signal connection.
    for( auto it = assetNameByBody_.begin(); it != assetNameByBody_.end(); ++it ) {
        if( it.value() == name ) {
            QObject::disconnect( it.key(), SIGNAL( nRefsChanged() ),
                                 this, SLOT( assetRefChanged() ) );
            assetNameByBody_.erase( it );
            break;
        }
    }
    delete item;
}

void SExternFileList::assetRefChanged()
{
    SObject *body = (SObject *) sender();
    QString name = assetNameByBody_.value( body );
    if( name.isEmpty() ) return;
    QTreeWidgetItem *item = assetItemDict_.value( name );
    if( item ) item->setText( 2, QString::number( body->getNReferences() ) );
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
    QObject::connect( &project_, SIGNAL( assetAdded( const QString &, SObject & ) ),
                      this, SLOT( assetAdded( const QString &, SObject & ) ) );
    QObject::connect( &project_, SIGNAL( assetRemoved( const QString & ) ),
                      this, SLOT( assetRemoved( const QString & ) ) );
}
