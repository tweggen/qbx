#ifndef SLOGMODEL_H
#define SLOGMODEL_H

#include <QAbstractTableModel>
#include <QSet>
#include <QString>
#include <QTimer>

#include "tw/core/twlog.h"

#include <deque>
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

    // Longest single drain tick since the last reset, in ms. This — not the
    // duration of an event-loop pump — is the honest measure of what the dock
    // costs the GUI thread: a pump also carries whatever else the app had
    // queued (device latency probes at startup can alone take ~90 ms).
    qint64 worstDrainMs() const { return worstDrainMs_; }
    void   resetDrainStats() { worstDrainMs_ = 0; }

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

    // A deque, NOT a vector. Both hot operations are O(1)/O(k) here and O(N) in
    // a vector: eviction erases from the FRONT (a vector would move every
    // remaining row), and growth to the 200k cap would make a vector
    // periodically reallocate and move the lot. Either spike lands directly on
    // the GUI thread.
    std::deque<Row> rows_;
    uint64_t         cursor_ = 0;          // next ring seq we have not consumed
    QTimer           timer_;

    tw::LogLevel  level_ = tw::LogLevel::Trace;
    QSet<QString> cats_;
    QString       text_;

    // One tick absorbs as much as fits in a TIME budget, not a fixed count. A
    // count cannot be right across machines and message sizes — at 20 000
    // records a tick this blocked the GUI thread for 105 ms on a burst. The
    // remainder arrives on the next tick; the status line shows the backlog.
    static constexpr int kDrainBudgetMs = 8;
    static constexpr int kDrainChunk    = 2000;   // records per budget check
    // Bound on displayed rows. Beyond this the oldest are evicted in one
    // beginRemoveRows, mirroring the ring's own eviction.
    int viewCap_ = 200000;

    qint64 worstDrainMs_ = 0;
};

#endif // SLOGMODEL_H
