#include "app/servicesui/slogmodel.h"

#include <QBrush>
#include <QColor>
#include <QStringList>

#include <algorithm>

using tw::LogLevel;
using tw::LogRecord;
using tw::TwLog;

SLogModel::SLogModel( QObject *parent )
    : QAbstractTableModel( parent )
{
    timer_.setInterval( 100 );
    connect( &timer_, &QTimer::timeout, this, &SLogModel::drain );

    // Start at the oldest record still in the ring, so opening the dock shows
    // the history the app has already accumulated rather than only what happens
    // from now on.
    cursor_ = TwLog::instance().firstSeq();

    const size_t cap = TwLog::instance().capacity();
    if( cap > 0 ) viewCap_ = static_cast<int>( cap );
}

int SLogModel::rowCount( const QModelIndex &parent ) const
{
    if( parent.isValid() ) return 0;
    return static_cast<int>( rows_.size() );
}

int SLogModel::columnCount( const QModelIndex &parent ) const
{
    if( parent.isValid() ) return 0;
    return ColumnCount;
}

QVariant SLogModel::data( const QModelIndex &index, int role ) const
{
    if( !index.isValid() ) return QVariant();
    const int r = index.row();
    if( r < 0 || r >= static_cast<int>( rows_.size() ) ) return QVariant();
    const Row &row = rows_[ static_cast<size_t>( r ) ];

    // Everything is precomputed at insert time; data() does no formatting and
    // no allocation beyond the implicit share, which is what keeps painting
    // cheap when the view is scrolled fast over hundreds of thousands of rows.
    if( role == Qt::DisplayRole ) {
        switch( index.column() ) {
            case ColTime:     return row.time;
            case ColLevel:    return row.level;
            case ColCategory: return row.category;
            case ColThread:   return row.thread;
            case ColMessage:  return row.message;
            default:          return QVariant();
        }
    }

    if( role == Qt::ForegroundRole ) {
        switch( row.lvl ) {
            case LogLevel::Error: return QBrush( QColor( 0xd0, 0x30, 0x30 ) );
            case LogLevel::Warn:  return QBrush( QColor( 0xc0, 0x80, 0x10 ) );
            case LogLevel::Debug: return QBrush( QColor( 0x80, 0x80, 0x80 ) );
            case LogLevel::Trace: return QBrush( QColor( 0xa0, 0xa0, 0xa0 ) );
            case LogLevel::Info:  break;
        }
        return QVariant();
    }

    if( role == Qt::ToolTipRole && index.column() == ColMessage )
        return row.message;

    return QVariant();
}

QVariant SLogModel::headerData( int section, Qt::Orientation o, int role ) const
{
    if( role != Qt::DisplayRole || o != Qt::Horizontal ) return QVariant();
    switch( section ) {
        case ColTime:     return tr( "Time" );
        case ColLevel:    return tr( "Lvl" );
        case ColCategory: return tr( "Category" );
        case ColThread:   return tr( "Thread" );
        case ColMessage:  return tr( "Message" );
        default:          return QVariant();
    }
}

void SLogModel::setLive( bool live )
{
    if( live ) {
        if( !timer_.isActive() ) {
            drain();              // catch up immediately, then tick
            timer_.start();
        }
    } else {
        timer_.stop();
    }
}

bool SLogModel::passes( const LogRecord &rec, const QString &cat ) const
{
    if( static_cast<int>( rec.level ) > static_cast<int>( level_ ) ) return false;
    if( !cats_.isEmpty() && !cats_.contains( cat ) ) return false;
    if( !text_.isEmpty() ) {
        const QString msg = QString::fromStdString( rec.text );
        if( !msg.contains( text_, Qt::CaseInsensitive ) &&
            !cat.contains( text_, Qt::CaseInsensitive ) )
            return false;
    }
    return true;
}

SLogModel::Row SLogModel::makeRow( const LogRecord &rec, const QString &cat ) const
{
    // TwLog::formatLine() renders the whole line; here the fields are wanted
    // separately, so take the timestamp from the same arithmetic it uses by
    // asking for the formatted line and splitting off its head. Cheaper and
    // less duplicated than re-deriving the wall clock here.
    const std::string line = TwLog::formatLine( rec );
    QString time;
    if( line.size() >= 12 ) time = QString::fromLatin1( line.data(), 12 );

    Row row;
    row.time     = time;
    row.level    = QString::fromLatin1( TwLog::levelName( rec.level ) );
    row.category = cat;
    row.thread   = QString::fromLatin1( TwLog::threadName( rec.threadId ) );
    row.message  = QString::fromStdString( rec.text );
    row.lvl      = rec.level;
    return row;
}

void SLogModel::drain()
{
    TwLog &log = TwLog::instance();

    const uint64_t first = log.firstSeq();
    if( cursor_ < first ) cursor_ = first;      // the ring lapped past us

    uint64_t to = log.nextSeq();
    if( cursor_ >= to ) return;
    if( to - cursor_ > static_cast<uint64_t>( kDrainPerTick ) )
        to = cursor_ + kDrainPerTick;

    std::vector<LogRecord> batch;
    log.snapshot( cursor_, to, batch );
    if( batch.empty() ) { cursor_ = to; return; }
    cursor_ = batch.back().seq + 1;

    std::vector<Row> keep;
    keep.reserve( batch.size() );
    for( const LogRecord &rec : batch ) {
        const QString cat = QString::fromLatin1( TwLog::categoryName( rec.catId ) );
        if( passes( rec, cat ) ) keep.push_back( makeRow( rec, cat ) );
    }
    if( keep.empty() ) return;

    // ONE insert for the whole surviving batch, not one per record.
    const int at = static_cast<int>( rows_.size() );
    beginInsertRows( QModelIndex(), at, at + static_cast<int>( keep.size() ) - 1 );
    rows_.insert( rows_.end(), keep.begin(), keep.end() );
    endInsertRows();

    // ...and ONE removal if we are over the cap.
    if( static_cast<int>( rows_.size() ) > viewCap_ ) {
        const int excess = static_cast<int>( rows_.size() ) - viewCap_;
        beginRemoveRows( QModelIndex(), 0, excess - 1 );
        rows_.erase( rows_.begin(), rows_.begin() + excess );
        endRemoveRows();
    }

    emit rowsAppended();
}

void SLogModel::rescan()
{
    TwLog &log = TwLog::instance();

    beginResetModel();
    rows_.clear();
    const uint64_t first = log.firstSeq();
    const uint64_t last  = log.nextSeq();

    // Walk the resident ring in chunks rather than snapshotting all of it at
    // once: a full 200k-record copy would spike memory for no reason.
    std::vector<LogRecord> batch;
    for( uint64_t s = first; s < last; ) {
        const uint64_t chunkEnd = std::min<uint64_t>( s + 8192, last );
        log.snapshot( s, chunkEnd, batch );
        if( batch.empty() ) break;
        for( const LogRecord &rec : batch ) {
            const QString cat = QString::fromLatin1( TwLog::categoryName( rec.catId ) );
            if( passes( rec, cat ) ) rows_.push_back( makeRow( rec, cat ) );
        }
        s = batch.back().seq + 1;
    }
    if( static_cast<int>( rows_.size() ) > viewCap_ )
        rows_.erase( rows_.begin(),
                     rows_.begin() + ( static_cast<int>( rows_.size() ) - viewCap_ ) );
    cursor_ = last;
    endResetModel();

    emit rowsAppended();
}

void SLogModel::setLevelFilter( LogLevel maxLevel )
{
    if( level_ == maxLevel ) return;
    level_ = maxLevel;
    rescan();
}

void SLogModel::setCategoryFilter( const QSet<QString> &cats )
{
    if( cats_ == cats ) return;
    cats_ = cats;
    rescan();
}

void SLogModel::setTextFilter( const QString &substring )
{
    if( text_ == substring ) return;
    text_ = substring;
    rescan();
}

QStringList SLogModel::knownCategories() const
{
    QStringList out;
    const uint16_t n = TwLog::categoryCount();
    for( uint16_t i = 0; i < n; ++i ) {
        const QString c = QString::fromLatin1( TwLog::categoryName( i ) );
        if( !c.isEmpty() ) out << c;
    }
    out.sort();
    return out;
}

void SLogModel::clearView()
{
    beginResetModel();
    rows_.clear();
    cursor_ = TwLog::instance().nextSeq();
    endResetModel();
}

int SLogModel::backlog() const
{
    const uint64_t to = TwLog::instance().nextSeq();
    return to > cursor_ ? static_cast<int>( to - cursor_ ) : 0;
}

QString SLogModel::rowText( int row ) const
{
    if( row < 0 || row >= static_cast<int>( rows_.size() ) ) return QString();
    const Row &r = rows_[ static_cast<size_t>( row ) ];
    return QStringLiteral( "%1 [%2] %3 %4 %5" )
        .arg( r.time, r.level, r.category, r.thread, r.message );
}

QString SLogModel::allText() const
{
    QString out;
    out.reserve( static_cast<int>( rows_.size() ) * 80 );
    for( int i = 0; i < static_cast<int>( rows_.size() ); ++i ) {
        out += rowText( i );
        out += QLatin1Char( '\n' );
    }
    return out;
}
