
#include <QDebug>
#include <QDrag>
#include <QMimeData>
#include <QPixmap>
#include <QPainter>
#include <QFontMetrics>

#include "app/model/sobject.h"
#include "app/model/sexternfile.h"
#include "app/model/sexternfilelist.h"
#include "app/model/sproject.h"

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

void SExternFileList::startDrag(Qt::DropActions /*supportedActions*/)
{
    QTreeWidgetItem *item = currentItem();
    if (!item) return;

    QString mimePayload;

    // Check if this is an asset item.
    if (assetItemDict_.values().contains(item)) {
        // Find the asset name by searching assetItemDict_.
        for (auto it = assetItemDict_.begin(); it != assetItemDict_.end(); ++it) {
            if (it.value() == item) {
                mimePayload = QStringLiteral("asset:") + it.key();
                break;
            }
        }
    } else if (itemDict_.values().contains(dynamic_cast<SExternFileItem*>(item))) {
        // It's a file item.
        SExternFileItem *fileItem = dynamic_cast<SExternFileItem*>(item);
        if (fileItem) {
            mimePayload = QStringLiteral("file:") + fileItem->getExternFile().getFileName();
        }
    }

    if (mimePayload.isEmpty()) return;

    QMimeData *mimeData = new QMimeData;
    mimeData->setData(QStringLiteral("application/x-smaragd-resource"), mimePayload.toUtf8());

    QDrag *drag = new QDrag(this);
    drag->setMimeData(mimeData);

    // Give the drag a visible ghost so something follows the cursor. (Qt's
    // default item-view drag renders the row, but we build our own QDrag.)
    {
        QString label = item->text(0);
        QFontMetrics fm(font());
        int w = fm.horizontalAdvance(label) + 16;
        int h = fm.height() + 8;
        QPixmap pm(w, h);
        pm.fill(QColor(60, 60, 90, 220));
        QPainter pp(&pm);
        pp.setPen(Qt::white);
        pp.drawRect(0, 0, w - 1, h - 1);
        pp.drawText(pm.rect(), Qt::AlignCenter, label);
        pp.end();
        drag->setPixmap(pm);
        drag->setHotSpot(QPoint(w / 2, h / 2));
    }

    drag->exec(Qt::CopyAction);
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
    disconnectSignals();
}

void SExternFileList::disconnectSignals()
{
    if( project_ ) {
        QObject::disconnect( project_, nullptr, this, nullptr );
    }
}

void SExternFileList::populate()
{
    if( !project_ ) return;
    // Back-fill rows for files/assets that already exist on the project. During a
    // project load the files are added (and externFileAdded/assetAdded emitted)
    // BEFORE this list is attached and connectSignals() runs, so those signals are
    // lost. Here we reuse the very same row-building slots for the already-present
    // entries, keeping the item dicts and per-file nRefsChanged connections
    // consistent. Called from setProject() before connectSignals(), so there is no
    // double-add. Any files added later still arrive via the live signal.
    const QHash<QString,SExternFile*> &files = project_->externFiles();
    for( auto it = files.constBegin(); it != files.constEnd(); ++it ) {
        if( it.value() ) externFileAdded( *it.value() );
    }
    const QHash<QString,SObject*> &assets = project_->assets();
    for( auto it = assets.constBegin(); it != assets.constEnd(); ++it ) {
        if( it.value() ) assetAdded( it.key(), *it.value() );
    }
}

void SExternFileList::connectSignals()
{
    if( !project_ ) return;

    QObject::connect( project_, SIGNAL( externFileAdded( const SExternFile & ) ),
                      this, SLOT( externFileAdded( const SExternFile & ) ) );
    QObject::connect( project_, SIGNAL( externFileRemoved( const QString ) ),
                      this, SLOT( externFileRemoved( const QString ) ) );
    QObject::connect( project_, SIGNAL( assetAdded( const QString &, SObject & ) ),
                      this, SLOT( assetAdded( const QString &, SObject & ) ) );
    QObject::connect( project_, SIGNAL( assetRemoved( const QString & ) ),
                      this, SLOT( assetRemoved( const QString & ) ) );
}

void SExternFileList::setProject( SProject *project )
{
    disconnectSignals();
    clear();
    itemDict_.clear();
    assetItemDict_.clear();
    assetNameByBody_.clear();

    project_ = project;
    if( project_ ) {
        populate();
        connectSignals();
    }
}

SExternFileList::SExternFileList( QWidget *parent, SProject *project )
    : QTreeWidget( parent ),
      project_( nullptr )
{
    setColumnCount(3);
    // The extern-file list is a bounded side panel, not the primary content: cap its
    // width and give it a non-greedy horizontal policy so it cannot starve the
    // Expanding central mixer view. Without this, QMainWindow hands all resize space
    // to this dock (a QTreeWidget is Expanding by default) and the mixer stays pinned
    // at its minimum; a bad, oversized dock width persisted from a previous session is
    // also clamped here. Keeps a usable min so it never collapses either.
    setSizePolicy( QSizePolicy::Preferred, QSizePolicy::Expanding );
    setMinimumWidth(200);
    setMaximumWidth(360);
    setMinimumHeight(100);
    QStringList headers = { "File name", "Type", "Refs" };
    setHeaderLabels(headers);
    setDragEnabled(true);
    setDragDropMode(QAbstractItemView::DragOnly);
    setSelectionMode(QAbstractItemView::SingleSelection);

    if( project ) {
        setProject( project );
    }
}
