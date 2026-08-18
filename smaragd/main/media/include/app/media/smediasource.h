// app/media/smediasource.h — the media SOURCE ABI (proposal 38 §B.2).
//
// One abstract QObject that a local file system, a WebDAV server or anything
// else answers listing, search and fetch requests through. Read
// main/media/CONTRACT.md before changing anything here: the six invariants
// beside this class are the whole reason it looks like this, and two of them
// (the queued hand-back, and supersession by id) are the difference between
// code that works and code that works until it loses a race.
//
// NO WIDGET MAY EVER BE INCLUDED IN THIS MODULE. That is a contract plus a
// grep (gate 1 AC 11), not a compiler error: app_model links Qt::Widgets
// PUBLIC, so every widget header is reachable from every layer.

#ifndef SMEDIASOURCE_H
#define SMEDIASOURCE_H

#include "app/media/smediaref.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

class SMediaSource : public QObject
{
    Q_OBJECT

public:
    enum Cap {
        CanBrowse  = 1,
        CanSearch  = 2,
        CanStream  = 4,
        NeedsFetch = 8,
    };

    explicit SMediaSource( QObject *parent = nullptr );
    ~SMediaSource() override;

    virtual QString id() const          = 0;
    virtual QString displayName() const = 0;
    virtual int     caps() const        = 0;

    // Every call is ASYNC and returns a request id (> 0). Results arrive on
    // the MAIN thread, tagged with that id. A request id is never reused
    // within a process.
    virtual int  listDirectory( const QString &dirPath )                = 0;
    virtual int  search( const QString &rootPath, const QString &needle,
                         bool recursive, const QStringList &suffixes )  = 0;
    virtual int  fetch( const QString &filePath, const QString &destPath ) = 0;
    virtual void cancel( int requestId )                                = 0;

    // The suffix filter listDirectory() applies to FILES (directories are
    // always listed — you must be able to navigate past a filter). An EMPTY
    // list means no filter.
    //
    // This is source STATE rather than a listDirectory() parameter because
    // the dock has ONE filter control that governs both modes, and because a
    // lazily-expanded tree issues a listDirectory() per expanded row with no
    // caller in a position to re-supply it. search() keeps its explicit
    // `suffixes` argument: a search request is a snapshot of the whole query.
    void        setSuffixFilter( const QStringList &suffixes );
    QStringList suffixFilter() const;

signals:
    // `batch` may be partial: batches stream (inv. 3). `final` is emitted
    // EXACTLY ONCE per request that completes, and never for one that was
    // cancelled. `truncatedCount > 0` on the final batch means the walk hit a
    // bound (inv. 4) and is a LOWER BOUND on what was not delivered — never
    // silence.
    void entriesReady( int requestId, const QVector<SMediaEntry> &batch,
                       bool final, int truncatedCount );
    void requestFailed( int requestId, const QString &message );
    void fetchProgress( int requestId, qint64 done, qint64 total );
    void fetchFinished( int requestId, const QString &localPath );

protected:
    // The id the next request should take. Monotone, never reused.
    int nextRequestId();

private:
    QStringList suffixFilter_;
    int         nextRequestId_ = 0;
};

#endif // SMEDIASOURCE_H
