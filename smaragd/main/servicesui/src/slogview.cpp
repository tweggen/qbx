#include "app/servicesui/slogview.h"
#include "app/servicesui/slogmodel.h"
#include "app/shell/ssettings.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QModelIndexList>
#include <QSaveFile>
#include <QScrollBar>
#include <QTableView>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

using tw::LogLevel;

SLogView::SLogView( QWidget *parent )
    : QWidget( parent )
{
    model_ = new SLogModel( this );

    // ---- toolbar --------------------------------------------------------
    QToolBar *bar = new QToolBar( this );
    bar->setIconSize( QSize( 16, 16 ) );

    levelBox_ = new QComboBox( bar );
    levelBox_->addItem( tr( "Errors" ),   (int)LogLevel::Error );
    levelBox_->addItem( tr( "Warnings" ), (int)LogLevel::Warn );
    levelBox_->addItem( tr( "Info" ),     (int)LogLevel::Info );
    levelBox_->addItem( tr( "Debug" ),    (int)LogLevel::Debug );
    levelBox_->addItem( tr( "Trace" ),    (int)LogLevel::Trace );
    levelBox_->setCurrentIndex( 4 );
    levelBox_->setToolTip( tr( "Show this level and everything more severe" ) );
    connect( levelBox_, QOverload<int>::of( &QComboBox::currentIndexChanged ),
             this, &SLogView::onLevelChanged );
    bar->addWidget( new QLabel( tr( "  Level: " ), bar ) );
    bar->addWidget( levelBox_ );

    categoryBtn_ = new QToolButton( bar );
    categoryBtn_->setText( tr( "Categories" ) );
    categoryBtn_->setPopupMode( QToolButton::InstantPopup );
    categoryMenu_ = new QMenu( categoryBtn_ );
    categoryBtn_->setMenu( categoryMenu_ );
    bar->addWidget( categoryBtn_ );

    filterEdit_ = new QLineEdit( bar );
    filterEdit_->setPlaceholderText( tr( "Filter…" ) );
    filterEdit_->setClearButtonEnabled( true );
    filterEdit_->setMaximumWidth( 260 );
    connect( filterEdit_, &QLineEdit::textChanged, this, &SLogView::onTextFilterEdited );
    bar->addWidget( filterEdit_ );

    bar->addSeparator();
    pauseAct_ = bar->addAction( tr( "Pause" ) );
    pauseAct_->setCheckable( true );
    pauseAct_->setToolTip( tr( "Stop updating the view. The log keeps recording; "
                               "resuming replays from the buffer." ) );
    connect( pauseAct_, &QAction::toggled, this, &SLogView::togglePause );

    bar->addAction( tr( "Clear" ), this, &SLogView::clearView )
       ->setToolTip( tr( "Clear the view only — the log file keeps its history" ) );
    bar->addAction( tr( "Copy" ),  this, &SLogView::copySelection );
    bar->addAction( tr( "Save…" ), this, &SLogView::saveAs );
    bar->addAction( tr( "Log folder" ), this, &SLogView::openLogFolder );

    statusLabel_ = new QLabel( bar );
    statusLabel_->setContentsMargins( 8, 0, 8, 0 );
    QWidget *spacer = new QWidget( bar );
    spacer->setSizePolicy( QSizePolicy::Expanding, QSizePolicy::Preferred );
    bar->addWidget( spacer );
    bar->addWidget( statusLabel_ );

    // ---- table ----------------------------------------------------------
    table_ = new QTableView( this );
    table_->setModel( model_ );
    table_->setSelectionBehavior( QAbstractItemView::SelectRows );
    table_->setSelectionMode( QAbstractItemView::ExtendedSelection );
    table_->setEditTriggers( QAbstractItemView::NoEditTriggers );
    table_->setShowGrid( false );
    table_->setWordWrap( false );
    table_->setTextElideMode( Qt::ElideRight );
    table_->setAlternatingRowColors( true );
    table_->setHorizontalScrollMode( QAbstractItemView::ScrollPerPixel );
    table_->setVerticalScrollMode( QAbstractItemView::ScrollPerPixel );
    table_->setFont( QFontDatabase::systemFont( QFontDatabase::FixedFont ) );

    // THE performance-critical bit: a fixed row height makes the view's
    // scrollbar geometry pure arithmetic and its painting O(visible rows), so
    // 500 000 rows cost the same to scroll as 500.
    QHeaderView *vh = table_->verticalHeader();
    vh->setSectionResizeMode( QHeaderView::Fixed );
    vh->setDefaultSectionSize( table_->fontMetrics().height() + 4 );
    vh->setVisible( false );

    // Size the narrow columns ONCE from representative text, then pin them.
    // QHeaderView::ResizeToContents would re-measure every row on every insert.
    QHeaderView *hh = table_->horizontalHeader();
    const QFontMetrics fm = table_->fontMetrics();
    hh->resizeSection( SLogModel::ColTime,     fm.horizontalAdvance( "00:00:00.000" ) + 12 );
    hh->resizeSection( SLogModel::ColLevel,    fm.horizontalAdvance( "WRN" ) + 12 );
    hh->resizeSection( SLogModel::ColCategory, fm.horizontalAdvance( "ui.timeline" ) + 12 );
    hh->resizeSection( SLogModel::ColThread,   fm.horizontalAdvance( "log-writer" ) + 12 );
    hh->setSectionResizeMode( SLogModel::ColTime,     QHeaderView::Interactive );
    hh->setSectionResizeMode( SLogModel::ColLevel,    QHeaderView::Interactive );
    hh->setSectionResizeMode( SLogModel::ColCategory, QHeaderView::Interactive );
    hh->setSectionResizeMode( SLogModel::ColThread,   QHeaderView::Interactive );
    hh->setSectionResizeMode( SLogModel::ColMessage,  QHeaderView::Stretch );

    QVBoxLayout *lay = new QVBoxLayout( this );
    lay->setContentsMargins( 0, 0, 0, 0 );
    lay->setSpacing( 0 );
    lay->addWidget( bar );
    lay->addWidget( table_ );

    connect( model_, &SLogModel::rowsAppended, this, &SLogView::onRowsAppended );

    filterDebounce_ = new QTimer( this );
    filterDebounce_->setSingleShot( true );
    filterDebounce_->setInterval( 200 );
    connect( filterDebounce_, &QTimer::timeout, this, &SLogView::applyTextFilter );

    statusTimer_ = new QTimer( this );
    statusTimer_->setInterval( 500 );
    connect( statusTimer_, &QTimer::timeout, this, &SLogView::refreshStatus );

    categoryTimer_ = new QTimer( this );
    categoryTimer_->setInterval( 2000 );
    connect( categoryTimer_, &QTimer::timeout, this, &SLogView::rebuildCategoryMenu );

    rebuildCategoryMenu();
    refreshStatus();
}

void SLogView::setLive( bool live )
{
    wantLive_ = live;
    const bool run = live && !paused_;
    model_->setLive( run );
    if( live ) { statusTimer_->start(); categoryTimer_->start(); }
    else       { statusTimer_->stop();  categoryTimer_->stop(); }
}

int SLogView::backlog() const       { return model_->backlog(); }
int SLogView::displayedRows() const { return model_->rowCount(); }
qint64 SLogView::worstDrainMs() const  { return model_->worstDrainMs(); }
void SLogView::resetDrainStats()       { model_->resetDrainStats(); }

bool SLogView::atBottom() const
{
    const QScrollBar *sb = table_->verticalScrollBar();
    return sb->value() >= sb->maximum() - 2;
}

void SLogView::scrollToBottom()
{
    QScrollBar *sb = table_->verticalScrollBar();
    sb->setValue( sb->maximum() );
}

void SLogView::onRowsAppended()
{
    // Follow the tail only when already parked at the bottom, so scrolling up
    // to read something pins the view instead of yanking it away.
    //
    // wasAtBottom_ is a MEMBER, not a function-local static: a static would be
    // shared by every SLogView in the process, so a second one would inherit
    // the first's scroll state. Only one log dock exists today, which is
    // exactly why the bug would sit there unnoticed until it did not.
    if( wasAtBottom_ ) scrollToBottom();
    wasAtBottom_ = atBottom();
}

void SLogView::onTextFilterEdited()
{
    filterDebounce_->start();     // restart; one rescan per pause in typing
}

void SLogView::applyTextFilter()
{
    model_->setTextFilter( filterEdit_->text() );
    scrollToBottom();
}

void SLogView::onLevelChanged( int index )
{
    const int lvl = levelBox_->itemData( index ).toInt();
    model_->setLevelFilter( static_cast<LogLevel>( lvl ) );
    scrollToBottom();
}

void SLogView::rebuildCategoryMenu()
{
    const QStringList cats = model_->knownCategories();

    QStringList existing;
    for( QAction *a : categoryMenu_->actions() )
        if( !a->isSeparator() && a->isCheckable() ) existing << a->text();
    if( existing == cats ) return;               // nothing new appeared

    const QSet<QString> checked = model_->categoryFilter();
    categoryMenu_->clear();

    QAction *all = categoryMenu_->addAction( tr( "Show all" ) );
    connect( all, &QAction::triggered, this, [this] {
        for( QAction *a : categoryMenu_->actions() )
            if( a->isCheckable() ) a->setChecked( false );
        model_->setCategoryFilter( {} );
    } );
    categoryMenu_->addSeparator();

    for( const QString &c : cats ) {
        QAction *a = categoryMenu_->addAction( c );
        a->setCheckable( true );
        a->setChecked( checked.contains( c ) );
        connect( a, &QAction::toggled, this, &SLogView::onCategoryToggled );
    }
}

void SLogView::onCategoryToggled()
{
    QSet<QString> selected;
    for( QAction *a : categoryMenu_->actions() )
        if( a->isCheckable() && a->isChecked() ) selected.insert( a->text() );
    // An empty selection means "everything", which is what a user who has just
    // unticked the last box expects — not an empty log.
    model_->setCategoryFilter( selected );
}

void SLogView::togglePause( bool paused )
{
    paused_ = paused;
    pauseAct_->setText( paused ? tr( "Resume" ) : tr( "Pause" ) );
    model_->setLive( wantLive_ && !paused_ );
    if( !paused_ ) scrollToBottom();
}

void SLogView::clearView()
{
    model_->clearView();
}

void SLogView::copySelection()
{
    const QModelIndexList sel = table_->selectionModel()->selectedRows();
    QString out;
    if( sel.isEmpty() ) {
        out = model_->allText();
    } else {
        QList<int> rows;
        for( const QModelIndex &i : sel ) rows << i.row();
        std::sort( rows.begin(), rows.end() );
        for( int r : rows ) { out += model_->rowText( r ); out += QLatin1Char( '\n' ); }
    }
    QApplication::clipboard()->setText( out );
}

void SLogView::saveAs()
{
    const QString dir = SSettings::instance().lastDir(
        QStringLiteral( "log" ), SSettings::instance().configDir() );
    const QString path = QFileDialog::getSaveFileName(
        this, tr( "Save log" ), dir + QStringLiteral( "/smaragd-log.txt" ),
        tr( "Text files (*.txt);;All files (*)" ) );
    if( path.isEmpty() ) return;

    QSaveFile f( path );
    if( !f.open( QIODevice::WriteOnly | QIODevice::Text ) ) {
        QMessageBox::warning( this, tr( "Save log" ),
                              tr( "Could not write %1" ).arg( path ) );
        return;
    }
    const QByteArray bytes = model_->allText().toUtf8();
    f.write( bytes );
    if( !f.commit() ) {
        QMessageBox::warning( this, tr( "Save log" ),
                              tr( "Could not write %1" ).arg( path ) );
        return;
    }
    SSettings::instance().setLastDir( QStringLiteral( "log" ),
                                      QFileInfo( path ).absolutePath() );
}

void SLogView::openLogFolder()
{
    QDesktopServices::openUrl(
        QUrl::fromLocalFile( SSettings::instance().configDir() ) );
}

void SLogView::refreshStatus()
{
    const uint64_t dropped = model_->droppedCount();
    QString s = tr( "%1 rows" ).arg( model_->rowCount() );
    const int back = model_->backlog();
    if( back > 0 )    s += tr( ", %1 pending" ).arg( back );
    if( dropped > 0 ) s += tr( ", %1 dropped" ).arg( dropped );
    statusLabel_->setText( s );
}
