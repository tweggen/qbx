#include "app/shell/sviewtabs.h"
#include "app/shell/sapplication.h"
#include "app/model/sproject.h"
#include "tw/core/twlog.h"
#include <QTabBar>
#include <QSize>
#include <QLayout>

SViewTabs::SViewTabs( QWidget *parent )
    : QTabWidget( parent )
{
    setDocumentMode( true );
    setTabsClosable( true );
    setMovable( false );          // tab 0 must stay tab 0; see styleMasterTab()
    connect( this, &QTabWidget::tabCloseRequested,
             this, &SViewTabs::onTabCloseRequested );
    connect( this, &QTabWidget::currentChanged,
             this, &SViewTabs::onCurrentChanged );
}

SViewTabs::~SViewTabs()
{
    // Entries hold QPointers and widgets the QTabWidget owns; nothing to free.
    entries_.clear();
}

int SViewTabs::indexOfRoot( const SObject *root ) const
{
    if( !root ) return -1;
    for( int i = 0; i < entries_.size(); ++i ) {
        if( entries_.at( i ).root.data() == root ) return i;
    }
    return -1;
}

void SViewTabs::styleMasterTab()
{
    // The master is not closeable. Qt has no per-tab "closable" flag, so the
    // close BUTTON is removed instead -- on both sides, because which side
    // carries it is a style decision and a stale button on the other one would
    // still be clickable.
    if( count() == 0 ) return;
    if( QTabBar *bar = tabBar() ) {
        bar->setTabButton( 0, QTabBar::RightSide, nullptr );
        bar->setTabButton( 0, QTabBar::LeftSide, nullptr );
    }
}

void SViewTabs::setMasterEditor( SObject *masterRoot, QWidget *editor,
                                 const QString &label )
{
    clearAll();
    if( !editor ) return;

    Entry e;
    e.root = masterRoot;
    e.editor = editor;
    entries_.append( e );
    addTab( editor, label.isEmpty() ? tr( "Arrangement" ) : label );
    styleMasterTab();
    setCurrentIndex( 0 );

    if( masterRoot ) {
        connect( masterRoot, &QObject::destroyed,
                 this, &SViewTabs::onRootDestroyed, Qt::UniqueConnection );
    }
}

QWidget *SViewTabs::masterEditor() const
{
    return entries_.isEmpty() ? nullptr : entries_.first().editor;
}

QWidget *SViewTabs::activeEditor() const
{
    const int i = currentIndex();
    if( i < 0 || i >= entries_.size() ) return masterEditor();
    return entries_.at( i ).editor;
}

SObject *SViewTabs::activeRoot() const
{
    const int i = currentIndex();
    if( i < 0 || i >= entries_.size() ) return nullptr;
    return entries_.at( i ).root.data();
}

QWidget *SViewTabs::openFor( SObject *root, const QString &label )
{
    if( !root ) return nullptr;

    // Dedup by identity: one object, at most one editor (§2).
    const int existing = indexOfRoot( root );
    if( existing >= 0 ) {
        setCurrentIndex( existing );
        return entries_.at( existing ).editor;
    }

    QWidget *editor = root->getDetailEditWidget( this );
    if( !editor ) {
        TW_LOGW( "ui.shell", "open tab: '%s' vends no detail editor",
                 root->getSName().toUtf8().constData() );
        return nullptr;
    }

    Entry e;
    e.root = root;
    e.editor = editor;
    entries_.append( e );
    const int idx = addTab( editor, label.isEmpty() ? root->getSName() : label );

    // The lifetime wire, from the first commit (§8): a root that dies takes its
    // tab with it, rather than leaving an editor holding a dangling pointer.
    connect( root, &QObject::destroyed,
             this, &SViewTabs::onRootDestroyed, Qt::UniqueConnection );

    setCurrentIndex( idx );

    // GIVE THE NEW PAGE A GEOMETRY NOW, rather than waiting for the layout.
    // A .qxa script runs its actions back to back without returning to the
    // event loop, so a page added mid-script is never resized -- and every
    // arranger gesture is PIXEL-BASED (getXPosOfOffset), so in a zero-sized
    // view a drag computes meaningless coordinates and lands nowhere. The
    // master tab does not show this because it is the central widget and was
    // laid out when the window was.
    QSize want = size();
    if( QWidget *ref = masterEditor() ) {
        if( ref != editor && !ref->size().isEmpty() ) want = ref->size();
    }
    if( want.isEmpty() ) want = QSize( 1200, 800 );
    editor->resize( want );
    // resize() alone only SCHEDULES the layout; the children -- and the
    // arranger's canvas is a child -- are still unsized until it runs, and a
    // .qxa never returns to the event loop for it to run in. activate() does
    // it now. This mirrors what the headless widget gates in SMainWindow
    // already do by hand ("resize() runs the real updateLayout()").
    if( QLayout *l = editor->layout() ) l->activate();

    return editor;
}

void SViewTabs::closeFor( SObject *root )
{
    const int idx = indexOfRoot( root );
    if( idx <= 0 ) return;        // 0 is the master, -1 is not open
    onTabCloseRequested( idx );
}

void SViewTabs::onTabCloseRequested( int index )
{
    if( index <= 0 || index >= entries_.size() ) return;   // never the master

    QWidget *editor = entries_.at( index ).editor;
    entries_.removeAt( index );
    removeTab( index );
    // Closing a tab destroys the VIEW and nothing else: the object it edited is
    // still referenced by the model and must outlive this.
    if( editor ) editor->deleteLater();
}

void SViewTabs::onCurrentChanged( int /*index*/ )
{
    // The active tab PUBLISHES itself as the selection context (proposal 09
    // §3). Every reader of SApplication's selection -- the menus, the delete
    // key, the transport -- then sees this tab's clips and no other tab's.
    SObject *root = activeRoot();
    QString name;
    if( root ) {
        if( SProject *proj = root->getProjectSafe() )
            name = proj->arrangementNameOf( root );
    }
    SApplication::app().setActiveSelectionRoot( name );

    emit activeRootChanged( root );
}

void SViewTabs::onRootDestroyed( QObject *obj )
{
    // QPointer has already nulled the entry, so match on the raw pointer value
    // -- obj is mid-destruction and must not be dereferenced.
    for( int i = entries_.size() - 1; i >= 1; --i ) {
        if( entries_.at( i ).root.isNull()
            || entries_.at( i ).root.data() == static_cast<SObject *>( obj ) ) {
            QWidget *editor = entries_.at( i ).editor;
            entries_.removeAt( i );
            removeTab( i );
            if( editor ) editor->deleteLater();
        }
    }
}

QStringList SViewTabs::tabNames() const
{
    QStringList out;
    for( int i = 0; i < count(); ++i ) out << tabText( i );
    return out;
}

QString SViewTabs::activeTabName() const
{
    const int i = currentIndex();
    return ( i >= 0 && i < count() ) ? tabText( i ) : QString();
}

void SViewTabs::clearAll()
{
    // Detach rather than delete: SMainWindow's teardown deliberately hands the
    // project's widgets back to the project's own QObject tree (see the comment
    // at its closeProject), and deleting them here would tear that tree up from
    // underneath the project destructor.
    while( count() > 0 ) removeTab( 0 );
    entries_.clear();
}
