#ifndef SLOGMODEL_H
#define SLOGMODEL_H

#include <QAbstractTableModel>
#include <QSet>
#include <QString>
#include <QTimer>

#include "tw/core/twlog.h"

#include <vector>

// Table model over the TwLog ring (proposal 24 §6.1).
//
// This holds its OWN filtered materialization rather than sitting behind a
// QSortFilterProxyModel, and that is the central performance decision: a proxy
// over a 200 000-row source rebuilds its full mapping on every filter change
// and re-evaluates on every batch insert, which is exactly the "the log window
// must not bog the application down" failure mode. Here filtering is a
// predicate applied once, as records are drained.
//
// The drain is a timer, not a signal from the sink. Producers are engine and
// worker threads that must never touch Qt (docs/contracts/THREADING.md), so
// they cannot notify us; instead we hold a cursor into the ring and pull.
class SLogModel : public QAbstractTableModel
{
    Q_OBJECT
public:
    enum Column { ColTime = 0, ColLevel, ColCategory, ColThread, ColMessage,
                  ColumnCount };

    explicit SLogModel( QObject *parent = nullptr );

    int      rowCount( const QModelIndex &parent = QModelIndex() ) const override;
    int      columnCount( const QModelIndex &parent = QModelIndex() ) const override;
    QVariant data( const QModelIndex &index, int role = Qt::DisplayRole ) const override;
    QVariant headerData( int section, Qt::Orientation o,
                         int role = Qt::DisplayRole ) const override;

    // Start/stop draining. The dock wires this to QDockWidget::visibilityChanged
    // so a closed log costs exactly nothing.
    void setLive( bool live );
    bool isLive() const { return timer_.isActive(); }

    // Filters. Each triggers one full re-scan of the resident ring — O(N) once,
    // for an explicit user action.
    void setLevelFilter( tw::LogLevel maxLevel );
    void setCategoryFilter( const QSet<QString> &cats );  // empty == all
    void setTextFilter( const QString &substring );

    tw::LogLevel      levelFilter() const { return level_; }
    QSet<QString>     categoryFilter() const { return cats_; }
    QString           textFilter() const { return text_; }

    // Categories seen in the sink so far, for the filter menu.
    QStringList knownCategories() const;

    // Drop everything currently displayed and resume from the newest record.
    // Does NOT clear the sink — other consumers (the file) keep their history.
    void clearView();

    // One record as a copyable line, and the whole view as text.
    QString rowText( int row ) const;
    QString allText() const;

    uint64_t droppedCount() const { return tw::TwLog::instance().droppedCount(); }
    int      backlog() const;     // records in the ring not yet drained

signals:
    void rowsAppended();          // so the view can decide about auto-scroll

private slots:
    void drain();

private:
    struct Row {
        QString  time;
        QString  level;
        QString  category;
        QString  thread;
        QString  message;
        tw::LogLevel lvl;
    };

    bool passes( const tw::LogRecord &rec, const QString &cat ) const;
    Row  makeRow( const tw::LogRecord &rec, const QString &cat ) const;
    void rescan();

    std::vector<Row> rows_;
    uint64_t         cursor_ = 0;          // next ring seq we have not consumed
    QTimer           timer_;

    tw::LogLevel  level_ = tw::LogLevel::Trace;
    QSet<QString> cats_;
    QString       text_;

    // Bound on how many records one tick may absorb, so a burst can never stall
    // the GUI thread; the remainder arrives on the next tick.
    static constexpr int kDrainPerTick = 20000;
    // Bound on displayed rows. Beyond this the oldest are evicted in one
    // beginRemoveRows, mirroring the ring's own eviction.
    int viewCap_ = 200000;
};

#endif // SLOGMODEL_H
