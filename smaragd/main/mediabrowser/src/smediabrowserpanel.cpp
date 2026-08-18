#include "app/mediabrowser/smediabrowserpanel.h"

#include "app/media/slocalmediasource.h"
#include "app/media/smediaref.h"
#include "app/media/smediaregistry.h"
#include "app/media/smediasource.h"
#include "app/media/smediatypes.h"
#include "app/servicesui/soptions.h"
#include "app/shell/ssettings.h"

#include "tw/core/twlog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDrag>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

// Item data roles. The entry is carried ON the item so describe(), the drag
// and the lazy expand all read ONE copy of it — a parallel row vector would be
// a second model to keep in step with the tree the user actually sees.
constexpr int kRolePath   = Qt::UserRole + 1;   // source-relative path
constexpr int kRoleIsDir  = Qt::UserRole + 2;
constexpr int kRoleSize   = Qt::UserRole + 3;
constexpr int kRoleLoaded = Qt::UserRole + 4;   // a dir whose children arrived

// The debounce the design names (§B.4). 250 ms is long enough that typing
// "kick" issues one walk rather than four, and short enough to feel live.
constexpr int kSearchDebounceMs = 250;

QString humanSize( qint64 bytes )
{
    if( bytes < 0 ) return QString();
    return QLocale().formattedDataSize( bytes );
}

}   // namespace

// --------------------------------------------------------------------------
// SMediaBrowserTree
// --------------------------------------------------------------------------

SMediaBrowserTree::SMediaBrowserTree( SMediaBrowserPanel *owner, QWidget *parent )
    : QTreeWidget( parent ), owner_( owner )
{
}

void SMediaBrowserTree::startDrag( Qt::DropActions /*supportedActions*/ )
{
    // ONE payload spelling, and it is the resources dock's (§A.1): that is the
    // whole reason gate 2 needs no timeline change at all.
    const QTreeWidgetItem *item = currentItem();
    if( !item || !owner_ ) return;
    QMimeData *mime = owner_->mimeForItem( item );
    if( !mime ) return;    // a directory row is not draggable (§B.5)

    QDrag *drag = new QDrag( this );
    drag->setMimeData( mime );

    // The ghost pixmap, lifted from SExternFileList::startDrag so the two docks
    // look the same in the hand.
    const QString  label = item->text( 0 );
    QFontMetrics   fm( font() );
    const int w = fm.horizontalAdvance( label ) + 16;
    const int h = fm.height() + 8;
    QPixmap pm( w, h );
    pm.fill( QColor( 60, 60, 90, 220 ) );
    {
        QPainter pp( &pm );
        pp.setPen( Qt::white );
        pp.drawRect( 0, 0, w - 1, h - 1 );
        pp.drawText( pm.rect(), Qt::AlignCenter, label );
    }
    drag->setPixmap( pm );
    drag->setHotSpot( QPoint( w / 2, h / 2 ) );

    drag->exec( Qt::CopyAction );
}

// --------------------------------------------------------------------------
// construction
// --------------------------------------------------------------------------

SMediaBrowserPanel::SMediaBrowserPanel( QWidget *parent )
    : QWidget( parent )
{
    buildUi();
    registerBuiltinSources();
    reloadSourceCombo();
    restoreState();
    updateFooter();
}

SMediaBrowserPanel::~SMediaBrowserPanel()
{
    cancelAll();
}

void SMediaBrowserPanel::buildUi()
{
    // The sizing policy is SExternFileList's, verbatim and for its reason: a
    // QTreeWidget is Expanding by default, so without a cap QMainWindow hands
    // this dock all the resize space and pins the arranger at its minimum.
    setSizePolicy( QSizePolicy::Preferred, QSizePolicy::Expanding );
    setMinimumWidth( 200 );
    setMaximumWidth( 360 );
    setMinimumHeight( 100 );

    QVBoxLayout *root = new QVBoxLayout( this );
    root->setContentsMargins( 4, 4, 4, 4 );
    root->setSpacing( 3 );

    // Row 1: source + media type.
    QHBoxLayout *row1 = new QHBoxLayout;
    row1->setSpacing( 3 );
    sourceCombo_ = new QComboBox( this );
    sourceCombo_->setToolTip( tr( "Media source" ) );
    row1->addWidget( sourceCombo_, 1 );

    filterButton_ = new QToolButton( this );
    filterButton_->setText( tr( "Media type" ) );
    filterButton_->setPopupMode( QToolButton::InstantPopup );
    filterMenu_ = new QMenu( filterButton_ );
    // One entry per CATEGORY, so the control is multi-category by shape — the
    // requester asked for a checkbox list. The MVP registers only Audio, so
    // the menu shows one entry (§B.3).
    QAction *audio = filterMenu_->addAction( tr( "Audio" ) );
    audio->setCheckable( true );
    audio->setData( (int) smedia::Audio );
    filterButton_->setMenu( filterMenu_ );
    row1->addWidget( filterButton_ );
    root->addLayout( row1 );

    // Row 2: up + path.
    QHBoxLayout *row2 = new QHBoxLayout;
    row2->setSpacing( 3 );
    upButton_ = new QToolButton( this );
    upButton_->setText( QStringLiteral( "Up" ) );
    upButton_->setToolTip( tr( "Up one level" ) );
    row2->addWidget( upButton_ );
    pathEdit_ = new QLineEdit( this );
    pathEdit_->setPlaceholderText( tr( "path" ) );
    row2->addWidget( pathEdit_, 1 );
    root->addLayout( row2 );

    // Row 3: search + recurse.
    QHBoxLayout *row3 = new QHBoxLayout;
    row3->setSpacing( 3 );
    searchEdit_ = new QLineEdit( this );
    searchEdit_->setPlaceholderText( tr( "search..." ) );
    searchEdit_->setClearButtonEnabled( true );
    row3->addWidget( searchEdit_, 1 );
    recursiveBox_ = new QCheckBox( tr( "recurse" ), this );
    row3->addWidget( recursiveBox_ );
    root->addLayout( row3 );

    // The inline banner (§B.4): an unreachable source names its error HERE,
    // with Retry, rather than in a modal nobody asked for.
    banner_ = new QFrame( this );
    banner_->setFrameShape( QFrame::StyledPanel );
    QHBoxLayout *bl = new QHBoxLayout( banner_ );
    bl->setContentsMargins( 4, 2, 4, 2 );
    bannerLabel_ = new QLabel( banner_ );
    bannerLabel_->setWordWrap( true );
    bl->addWidget( bannerLabel_, 1 );
    bannerRetry_ = new QPushButton( tr( "Retry" ), banner_ );
    bl->addWidget( bannerRetry_ );
    banner_->hide();
    root->addWidget( banner_ );

    tree_ = new SMediaBrowserTree( this, this );
    tree_->setColumnCount( 3 );
    tree_->setHeaderLabels( { tr( "Name" ), tr( "Size" ), tr( "Modified" ) } );
    tree_->setDragEnabled( true );
    tree_->setDragDropMode( QAbstractItemView::DragOnly );
    tree_->setSelectionMode( QAbstractItemView::SingleSelection );
    tree_->setUniformRowHeights( true );
    root->addWidget( tree_, 1 );

    footer_ = new QLabel( this );
    footer_->setTextInteractionFlags( Qt::TextSelectableByMouse );
    root->addWidget( footer_ );

    debounce_ = new QTimer( this );
    debounce_->setSingleShot( true );
    debounce_->setInterval( kSearchDebounceMs );

    connect( sourceCombo_, QOverload<int>::of( &QComboBox::activated ),
             this, &SMediaBrowserPanel::onSourceComboActivated );
    connect( &SMediaRegistry::instance(), &SMediaRegistry::sourcesChanged,
             this, &SMediaBrowserPanel::onRegistryChanged );
    connect( pathEdit_, &QLineEdit::returnPressed,
             this, &SMediaBrowserPanel::onPathEdited );
    connect( upButton_, &QToolButton::clicked,
             this, &SMediaBrowserPanel::onUpClicked );
    connect( searchEdit_, &QLineEdit::textChanged,
             this, &SMediaBrowserPanel::onSearchTextChanged );
    connect( debounce_, &QTimer::timeout,
             this, &SMediaBrowserPanel::onDebounceFired );
    connect( recursiveBox_, &QCheckBox::toggled,
             this, &SMediaBrowserPanel::onRecursiveToggled );
    connect( filterMenu_, &QMenu::triggered,
             this, [this]( QAction * ) { onFilterMenuChanged(); } );
    connect( tree_, &QTreeWidget::itemExpanded,
             this, &SMediaBrowserPanel::onItemExpanded );
    connect( bannerRetry_, &QPushButton::clicked,
             this, &SMediaBrowserPanel::onRetryClicked );
}

void SMediaBrowserPanel::registerBuiltinSources()
{
    // Registering is NOT contacting: SLocalMediaSource's constructor does no
    // I/O, and nothing is listed until a source is SELECTED (§B.4). The panel
    // OWNS this one; the registry deliberately owns none of its sources.
    if( SMediaRegistry::instance().source( QStringLiteral( "local" ) ) ) return;
    localSource_ = new SLocalMediaSource( this );
    SMediaRegistry::instance().registerSource( localSource_ );
}

void SMediaBrowserPanel::reloadSourceCombo()
{
    const QString want = sourceId_.isEmpty()
        ? SSettings::instance()
              .value( SOpt::MediaLastSourceId,
                      SOpt::def( SOpt::MediaLastSourceId ) ).toString()
        : sourceId_;

    QSignalBlocker block( sourceCombo_ );
    sourceCombo_->clear();
    for( const QString &id : SMediaRegistry::instance().sourceIds() ) {
        SMediaSource *s = SMediaRegistry::instance().source( id );
        sourceCombo_->addItem( s ? s->displayName() : id, id );
    }
    const int idx = sourceCombo_->findData( want );
    if( idx >= 0 ) sourceCombo_->setCurrentIndex( idx );
}

void SMediaBrowserPanel::restoreState()
{
    SSettings &st = SSettings::instance();
    sourceId_ = st.value( SOpt::MediaLastSourceId,
                          SOpt::def( SOpt::MediaLastSourceId ) ).toString();
    categoryMask_ = st.value( SOpt::MediaCategoryMask,
                              SOpt::def( SOpt::MediaCategoryMask ) ).toInt();
    recursive_ = st.value( SOpt::MediaSearchRecursive,
                           SOpt::def( SOpt::MediaSearchRecursive ) ).toBool();
    path_ = st.value( QString( SOpt::MediaLastPath ) + '/' + sourceId_,
                      QString() ).toString();
    if( path_.isEmpty() ) path_ = defaultPathForSource( sourceId_ );

    QSignalBlocker b1( recursiveBox_ );
    recursiveBox_->setChecked( recursive_ );
    pathEdit_->setText( path_ );
    for( QAction *a : filterMenu_->actions() )
        a->setChecked( ( categoryMask_ & a->data().toInt() ) != 0 );

    const int idx = sourceCombo_->findData( sourceId_ );
    if( idx >= 0 ) {
        QSignalBlocker b2( sourceCombo_ );
        sourceCombo_->setCurrentIndex( idx );
    }
}

QString SMediaBrowserPanel::defaultPathForSource( const QString &sourceId ) const
{
    if( sourceId != QStringLiteral( "local" ) ) return QString();
    // §B.4's first-run rule: the last Insert-sample directory is the one place
    // the app already remembers where THIS user keeps audio, so it beats a
    // generic Music folder; Music beats home; home always exists.
    const QString last = SSettings::instance().lastDir( QStringLiteral( "sample" ) );
    if( !last.isEmpty() && QFileInfo( last ).isDir() )
        return QDir::fromNativeSeparators( last );
    const QString music =
        QStandardPaths::writableLocation( QStandardPaths::MusicLocation );
    if( !music.isEmpty() && QFileInfo( music ).isDir() )
        return QDir::fromNativeSeparators( music );
    return QDir::fromNativeSeparators( QDir::homePath() );
}

void SMediaBrowserPanel::showEvent( QShowEvent *event )
{
    QWidget::showEvent( event );
    // FIRST SHOW is the first legal moment to contact anything. A headless run
    // never shows the dock, which is what keeps a qxa case from opening a
    // source no verb asked for.
    if( !opened_ ) openCurrentSource();
}

// --------------------------------------------------------------------------
// source / request plumbing
// --------------------------------------------------------------------------

void SMediaBrowserPanel::openCurrentSource()
{
    cancelAll();
    if( source_ ) {
        disconnect( source_, nullptr, this, nullptr );
        source_ = nullptr;
    }

    source_ = SMediaRegistry::instance().source( sourceId_ );
    if( !source_ ) {
        opened_ = false;
        tree_->clear();
        showBanner( tr( "Source \"%1\" is not available." ).arg( sourceId_ ) );
        updateFooter();
        return;
    }
    opened_ = true;

    connect( source_, &SMediaSource::entriesReady,
             this, &SMediaBrowserPanel::onEntriesReady, Qt::UniqueConnection );
    connect( source_, &SMediaSource::requestFailed,
             this, &SMediaBrowserPanel::onRequestFailed, Qt::UniqueConnection );
    connect( source_, &QObject::destroyed, this, [this]( QObject * ) {
        source_ = nullptr;
        opened_ = false;
    } );

    source_->setSuffixFilter( smedia::suffixesFor( categoryMask_ ) );
    if( path_.isEmpty() ) path_ = defaultPathForSource( sourceId_ );
    pathEdit_->setText( path_ );
    refreshRoot();
}

void SMediaBrowserPanel::cancelAll()
{
    if( source_ ) {
        if( rootRequest_ ) source_->cancel( rootRequest_ );
        for( auto it = expandRequests_.constBegin();
             it != expandRequests_.constEnd(); ++it )
            source_->cancel( it.key() );
    }
    rootRequest_ = 0;
    expandRequests_.clear();
}

void SMediaBrowserPanel::refreshRoot()
{
    if( !source_ ) return;

    // SUPERSEDE, do not race: the old request's id stops being the one we are
    // displaying the instant the new one is issued, so a batch still in flight
    // for it is dropped by onEntriesReady whatever the timing. The cancel is
    // a courtesy that saves the walk, not the correctness property.
    cancelAll();
    tree_->clear();
    truncated_ = 0;
    error_.clear();
    hideBanner();

    if( mode_ == Search && !needle_.isEmpty() ) {
        // The search carries its OWN suffix list: a request is a snapshot of
        // the whole query, so a filter change mid-walk cannot make the result
        // disagree with the request that asked for it (app/media CONTRACT).
        rootRequest_ = source_->search( path_, needle_, recursive_,
                                        smedia::suffixesFor( categoryMask_ ) );
    } else {
        mode_ = Browse;
        rootRequest_ = source_->listDirectory( path_ );
    }
    updateFooter();
}

void SMediaBrowserPanel::onEntriesReady( int requestId,
                                         const QVector<SMediaEntry> &batch,
                                         bool final, int truncatedCount )
{
    if( requestId == rootRequest_ ) {
        appendEntries( nullptr, batch );
        if( final ) {
            rootRequest_ = 0;
            truncated_   = truncatedCount;
            if( truncatedCount > 0 )
                TW_LOGW( "media", "browser: listing truncated, at least %d "
                                  "entries not shown", truncatedCount );
        }
        updateFooter();
        return;
    }

    auto it = expandRequests_.find( requestId );
    if( it != expandRequests_.end() ) {
        QTreeWidgetItem *parent = it.value();
        if( parent ) {
            appendEntries( parent, batch );
            if( final ) parent->setData( 0, kRoleLoaded, true );
        }
        if( final ) expandRequests_.erase( it );
        updateFooter();
        return;
    }

    // Not one of ours: a batch from a SUPERSEDED request. Dropped by ID
    // (inv. 2) — this is the line that stops a stale walk repainting over a
    // newer one, and it must never become a "cancelled?" flag test.
}

void SMediaBrowserPanel::onRequestFailed( int requestId, const QString &message )
{
    if( requestId == rootRequest_ ) {
        rootRequest_ = 0;
        error_       = message;
        showBanner( message );
        updateFooter();
        return;
    }
    auto it = expandRequests_.find( requestId );
    if( it != expandRequests_.end() ) {
        if( it.value() ) {
            it.value()->setData( 0, kRoleLoaded, true );
            // Drop the "expander" placeholder: a dir that cannot be listed must
            // not keep offering to be expanded forever.
            while( it.value()->childCount() > 0 )
                delete it.value()->takeChild( 0 );
        }
        expandRequests_.erase( it );
        error_ = message;
        showBanner( message );
        updateFooter();
    }
}

// --------------------------------------------------------------------------
// rows
// --------------------------------------------------------------------------

QTreeWidgetItem *SMediaBrowserPanel::makeItem( const SMediaEntry &entry ) const
{
    QTreeWidgetItem *item = new QTreeWidgetItem;
    item->setText( 0, entry.name );
    item->setText( 1, entry.isDir ? QString() : humanSize( entry.sizeBytes ) );
    item->setText( 2, entry.modified.isValid()
                          ? entry.modified.toString( Qt::ISODate )
                          : QString() );
    item->setData( 0, kRolePath, entry.ref.path );
    item->setData( 0, kRoleIsDir, entry.isDir );
    item->setData( 0, kRoleSize, (qlonglong) entry.sizeBytes );
    item->setData( 0, kRoleLoaded, false );
    if( entry.isDir ) {
        // A placeholder child so the expander appears; the real children arrive
        // on the first expand (lazy listDirectory, §B.4).
        item->addChild( new QTreeWidgetItem( QStringList{ QStringLiteral( "..." ) } ) );
        item->setFlags( item->flags() & ~Qt::ItemIsDragEnabled );
    }
    return item;
}

void SMediaBrowserPanel::appendEntries( QTreeWidgetItem *parent,
                                        const QVector<SMediaEntry> &batch )
{
    if( batch.isEmpty() ) return;
    if( parent ) {
        // Drop the placeholder on the first real child.
        if( parent->childCount() == 1
            && !parent->child( 0 )->data( 0, kRolePath ).isValid() )
            delete parent->takeChild( 0 );
        for( const SMediaEntry &e : batch ) parent->addChild( makeItem( e ) );
    } else {
        for( const SMediaEntry &e : batch )
            tree_->addTopLevelItem( makeItem( e ) );
    }
}

void SMediaBrowserPanel::collectRows( QTreeWidgetItem *parent,
                                      QVector<QTreeWidgetItem *> &out ) const
{
    const int n = parent ? parent->childCount() : tree_->topLevelItemCount();
    for( int i = 0; i < n; ++i ) {
        QTreeWidgetItem *child =
            parent ? parent->child( i ) : tree_->topLevelItem( i );
        if( !child->data( 0, kRolePath ).isValid() ) continue;   // placeholder
        out.append( child );
        collectRows( child, out );
    }
}

QVector<QTreeWidgetItem *> SMediaBrowserPanel::visibleRows() const
{
    QVector<QTreeWidgetItem *> rows;
    collectRows( nullptr, rows );
    return rows;
}

// --------------------------------------------------------------------------
// the drag payload
// --------------------------------------------------------------------------

QMimeData *SMediaBrowserPanel::mimeForItem( const QTreeWidgetItem *item ) const
{
    if( !item ) return nullptr;
    if( item->data( 0, kRoleIsDir ).toBool() ) return nullptr;
    const QString path = item->data( 0, kRolePath ).toString();
    if( path.isEmpty() ) return nullptr;

    // A LOCAL row emits "file:<absolute path>" — the payload
    // SMVActualView::dropEvent has always accepted, which is why this gate
    // needs no timeline change at all (§A.1). A remote row's "media:" payload
    // is gate 3.
    if( sourceId_ != QStringLiteral( "local" ) ) {
        TW_LOGW( "media", "browser: source '%s' has no drag payload yet "
                          "(the media: payload is gate 3)",
                 sourceId_.toUtf8().constData() );
        return nullptr;
    }

    QMimeData *mime = new QMimeData;
    mime->setData( QStringLiteral( "application/x-smaragd-resource" ),
                   ( QStringLiteral( "file:" ) + path ).toUtf8() );
    return mime;
}

QMimeData *SMediaBrowserPanel::createDragMime( int row, const QString &name ) const
{
    const QVector<QTreeWidgetItem *> rows = visibleRows();
    const QTreeWidgetItem *item = nullptr;
    if( !name.isEmpty() ) {
        for( QTreeWidgetItem *r : rows )
            if( r->text( 0 ) == name ) { item = r; break; }
    } else if( row >= 0 && row < rows.size() ) {
        item = rows.at( row );
    }
    return mimeForItem( item );
}

// --------------------------------------------------------------------------
// the widget's own signals
// --------------------------------------------------------------------------

void SMediaBrowserPanel::onSourceComboActivated( int index )
{
    const QString id = sourceCombo_->itemData( index ).toString();
    if( id.isEmpty() ) return;
    selectSource( id );
}

void SMediaBrowserPanel::onRegistryChanged()
{
    reloadSourceCombo();
}

void SMediaBrowserPanel::onPathEdited()
{
    setBrowsePath( pathEdit_->text() );
}

void SMediaBrowserPanel::onUpClicked()
{
    if( path_.isEmpty() ) return;
    QDir dir( path_ );
    if( !dir.cdUp() ) return;
    setBrowsePath( dir.absolutePath() );
}

void SMediaBrowserPanel::onSearchTextChanged()
{
    debounce_->start();
}

void SMediaBrowserPanel::onDebounceFired()
{
    needle_ = searchEdit_->text().trimmed();
    mode_   = needle_.isEmpty() ? Browse : Search;
    refreshRoot();
}

void SMediaBrowserPanel::onRecursiveToggled( bool on )
{
    recursive_ = on;
    persist( SOpt::MediaSearchRecursive, on );
    if( mode_ == Search ) refreshRoot();
}

void SMediaBrowserPanel::onFilterMenuChanged()
{
    int mask = 0;
    for( QAction *a : filterMenu_->actions() )
        if( a->isChecked() ) mask |= a->data().toInt();
    categoryMask_ = mask;
    persist( SOpt::MediaCategoryMask, mask );
    if( source_ ) source_->setSuffixFilter( smedia::suffixesFor( mask ) );
    if( opened_ ) refreshRoot();
    updateFooter();
}

void SMediaBrowserPanel::onItemExpanded( QTreeWidgetItem *item )
{
    if( !item || !source_ ) return;
    if( !item->data( 0, kRoleIsDir ).toBool() ) return;
    if( item->data( 0, kRoleLoaded ).toBool() ) return;
    // Already asked for?
    for( auto it = expandRequests_.constBegin();
         it != expandRequests_.constEnd(); ++it )
        if( it.value() == item ) return;

    const int id = source_->listDirectory( item->data( 0, kRolePath ).toString() );
    expandRequests_.insert( id, item );
    updateFooter();
}

void SMediaBrowserPanel::onRetryClicked()
{
    hideBanner();
    if( !source_ ) openCurrentSource();
    else refreshRoot();
}

// --------------------------------------------------------------------------
// the explicit entry points
// --------------------------------------------------------------------------

bool SMediaBrowserPanel::selectSource( const QString &id )
{
    sourceId_ = id;
    persist( SOpt::MediaLastSourceId, id );
    const int idx = sourceCombo_->findData( id );
    if( idx >= 0 ) {
        QSignalBlocker b( sourceCombo_ );
        sourceCombo_->setCurrentIndex( idx );
    }
    path_ = SSettings::instance()
                .value( QString( SOpt::MediaLastPath ) + '/' + id, QString() )
                .toString();
    if( path_.isEmpty() ) path_ = defaultPathForSource( id );
    mode_   = Browse;
    needle_.clear();
    {
        QSignalBlocker b( searchEdit_ );
        searchEdit_->clear();
    }
    openCurrentSource();
    return source_ != nullptr;
}

bool SMediaBrowserPanel::setBrowsePath( const QString &path )
{
    const QString clean = QDir::fromNativeSeparators( QDir::cleanPath( path ) );
    if( clean.isEmpty() ) return false;
    path_ = clean;
    pathEdit_->setText( clean );
    persist( QString( SOpt::MediaLastPath ) + '/' + sourceId_, clean );
    mode_ = Browse;
    needle_.clear();
    {
        QSignalBlocker b( searchEdit_ );
        searchEdit_->clear();
    }
    debounce_->stop();
    if( !source_ ) openCurrentSource();
    else refreshRoot();
    return source_ != nullptr;
}

bool SMediaBrowserPanel::expandRowNamed( const QString &name )
{
    for( QTreeWidgetItem *r : visibleRows() ) {
        if( r->text( 0 ) != name ) continue;
        if( !r->data( 0, kRoleIsDir ).toBool() ) return false;
        // The REAL expand: setExpanded emits itemExpanded, which is the same
        // slot a user's click on the triangle reaches.
        r->setExpanded( true );
        // A row expanded a second time emits nothing, so ask directly.
        onItemExpanded( r );
        return true;
    }
    return false;
}

bool SMediaBrowserPanel::setSearch( const QString &needle, bool recursive,
                                    bool viaDebounce )
{
    if( recursiveBox_->isChecked() != recursive ) {
        recursiveBox_->setChecked( recursive );   // emits toggled -> persists
    } else {
        recursive_ = recursive;
    }

    searchEdit_->setText( needle );   // starts the debounce, as typing does
    if( !viaDebounce ) {
        debounce_->stop();
        onDebounceFired();
    }
    return true;
}

bool SMediaBrowserPanel::setCategories( const QString &spelling )
{
    const int mask = smedia::categoryMaskFromString( spelling );
    for( QAction *a : filterMenu_->actions() )
        a->setChecked( ( mask & a->data().toInt() ) != 0 );
    onFilterMenuChanged();
    return true;
}

bool SMediaBrowserPanel::isBusy() const
{
    return rootRequest_ != 0 || !expandRequests_.isEmpty()
           || debounce_->isActive();
}

// --------------------------------------------------------------------------
// describe / footer / banner
// --------------------------------------------------------------------------

QString SMediaBrowserPanel::describe() const
{
    const QVector<QTreeWidgetItem *> rows = visibleRows();
    QString out;
    out += QStringLiteral( "mode=%1|source=%2|path=%3|filter=%4|recursive=%5"
                           "|rows=%6|truncated=%7|busy=%8|banner=%9|error=%10" )
               .arg( mode_ == Search ? QStringLiteral( "search" )
                                     : QStringLiteral( "browse" ) )
               .arg( sourceId_ )
               .arg( path_ )
               .arg( smedia::categoryMaskToString( categoryMask_ ) )
               .arg( recursive_ ? 1 : 0 )
               .arg( rows.size() )
               .arg( truncated_ )
               .arg( isBusy() ? 1 : 0 )
               .arg( ( banner_ && !banner_->isHidden() ) ? 1 : 0 )
               .arg( error_ );
    for( QTreeWidgetItem *r : rows ) {
        out += '\n';
        out += QStringLiteral( "%1,%2,%3" )
                   .arg( r->text( 0 ) )
                   .arg( r->data( 0, kRoleSize ).toLongLong() )
                   .arg( r->data( 0, kRoleIsDir ).toBool() ? 1 : 0 );
    }
    return out;
}

void SMediaBrowserPanel::updateFooter()
{
    if( !footer_ ) return;
    const int n = visibleRows().size();
    QString text;
    if( isBusy() ) {
        text = tr( "%1 items, working..." ).arg( n );
    } else if( truncated_ > 0 ) {
        // A bound is ANNOUNCED, never silent (app/media inv. 4).
        text = tr( "%1 items shown; at least %2 more were not (truncated)" )
                   .arg( n ).arg( truncated_ );
    } else {
        text = tr( "%1 items" ).arg( n );
    }
    footer_->setText( text );
}

void SMediaBrowserPanel::showBanner( const QString &message )
{
    error_ = message;
    if( !banner_ ) return;
    bannerLabel_->setText( message );
    banner_->show();
}

void SMediaBrowserPanel::hideBanner()
{
    error_.clear();
    if( banner_ ) banner_->hide();
}

void SMediaBrowserPanel::persist( const QString &key, const QVariant &value )
{
    SSettings::instance().setValue( key, value );
}
