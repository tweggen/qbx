// app/media/smediacache.h — the content-addressed local cache a fetch lands in.
//
// Proposal 38 §B.6, gate 3. Two stages exist and they are DIFFERENT THINGS:
// this is the first one. A fetch always lands here, keyed by content so that a
// re-fetch of an unchanged file is free; the SECOND stage (the copy into
// <projectdir>/media/, which is what makes a saved .qxp portable) belongs to
// app/media/smediadrop.h and is not this class's business.
//
// Read main/media/CONTRACT.md before changing anything here. Three properties
// are load-bearing and none of them is obvious from the API:
//
//   * EVERY WRITE GOES THROUGH QSaveFile (trap T7). The sidecar store paid for
//     this lesson in full: a fixed `<path>.tmp` let two processes truncate and
//     interleave into one temp and one of them published the mixture. A fetch
//     writes a PER-WRITER temp (`<key>.<pid>.<seq>.part`) and publish() streams
//     it into place through QSaveFile, which is write-to-temp-then-rename done
//     by Qt. Two processes fetching one key therefore leave ONE intact file.
//   * EVICTION MAY NEVER DELETE A FILE THE OPEN PROJECT REFERENCES (trap T17).
//     The cap is a cache policy and a project is not a cache. Without the pin,
//     "LRU past 2 GB" honestly reads as *a clip that plays today and is silent
//     next week*. evictToCap() takes the pinned set — SProject::externFiles()
//     — and a cache that cannot get under its cap because the project pins too
//     much LOGS THAT AND STOPS rather than evicting anyway.
//   * THE CONTENT KEY NEEDS METADATA THE DRAG PAYLOAD DOES NOT CARRY. §B.6
//     specifies sha1(sourceId + path + etag|mtime + size), but a `media:` drop
//     carries an SMediaRef and nothing else — that is what keeps the arranger's
//     new branch five lines long. The BROWSER is the only thing that ever holds
//     an SMediaEntry, so it feeds one here as it lists (noteEntry()). A ref the
//     cache has never seen still keys — over (sourceId, path) alone — and says
//     so in the key's own shape, because a key that refused to exist would turn
//     an un-listed drop into a failure instead of into a cache that cannot
//     detect a remote change.
//
// NO WIDGET, EVER (CONTRACT.md). This is app_core.

#ifndef SMEDIACACHE_H
#define SMEDIACACHE_H

#include "app/media/smediaref.h"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>

class SMediaCache : public QObject
{
    Q_OBJECT

public:
    // The process-wide cache. Its root is resolved ONCE, on first use:
    //
    //   SMARAGD_MEDIA_CACHE_DIR=<path>   that path, and setRoot() cannot
    //                                    override it (the knob exists so
    //                                    `ctest -j4` can isolate four
    //                                    processes; a later injection winning
    //                                    would defeat that)
    //   SMARAGD_MEDIA_CACHE_DIR=off      DISABLED — every lookup misses, every
    //                                    publish() is a no-op, and a fetch
    //                                    goes to a per-request temp that is
    //                                    never reused. The engine result is
    //                                    unchanged, only slower; the same
    //                                    contract SMARAGD_SIDECAR_DIR=off has
    //   otherwise                        <configDir>/mediacache, injected by
    //                                    the shell (SApplication) exactly as
    //                                    it injects the plugin scan cache's
    //                                    path — app_core cannot see SSettings
    static SMediaCache &instance();

    // An EXPLICIT root, for a unit test. `enabled=false` is the `off` shape.
    explicit SMediaCache( const QString &root, bool enabled = true,
                          QObject *parent = nullptr );

    bool    isEnabled() const { return enabled_; }
    QString root() const { return root_; }
    // Ignored (with a debug line) when SMARAGD_MEDIA_CACHE_DIR named one.
    void    setRoot( const QString &dir );

    // SOpt::MediaCacheCapMB, pushed down by the shell for the same reason the
    // root is. <= 0 means "no cap" and is never the default.
    qint64 capMB() const { return capMB_; }
    void   setCapMB( qint64 mb );

    // Feed the cache what a LISTING knew about an entry. Called by the browser
    // panel as rows arrive; never required, always better (see the header
    // comment). Only files are remembered.
    void noteEntry( const SMediaEntry &entry );

    // sha1( sourceId + '\n' + path + '\n' + etag|mtime + '\n' + size ), hex.
    // Stable across calls for the same known metadata, and DIFFERENT the
    // moment etag, mtime or size moves — which is the whole point: it is what
    // makes a stale cached copy a MISS rather than silently wrong audio.
    QString contentKey( const SMediaRef &ref ) const;

    // The cached file for `ref`, or "" for a miss (including "disabled").
    // A hit TOUCHES the file, which is what makes the LRU an LRU.
    QString lookup( const SMediaRef &ref );

    // Where a fetch of `ref` should write. A PER-WRITER name, always — two
    // processes fetching one key must not share a temp (T7). Empty when the
    // cache is disabled means the caller picks its own temp.
    QString reserveFetchPath( const SMediaRef &ref ) const;

    // Move the bytes at `fetched` into the cache under `ref`'s key, through
    // QSaveFile. Returns the published path, or "" (with *error set) on
    // failure. `fetched` is REMOVED on success. When the cache is disabled the
    // fetched path is returned unchanged and nothing is written.
    QString publish( const SMediaRef &ref, const QString &fetched,
                     QString *error = nullptr );

    // Delete least-recently-used entries until the cache is under its cap.
    // `pinned` is the set of absolute paths that may NEVER be deleted — the
    // open project's externFiles(). Returns how many files were removed; logs
    // every one of them, and logs (once) when the cap cannot be reached.
    int evictToCap( const QSet<QString> &pinned );

    // Bytes currently resident.
    qint64 totalBytes() const;

    // Delete everything under the cache root. TEST USE ONLY, and REFUSED
    // unless the root was named by SMARAGD_MEDIA_CACHE_DIR (or handed in
    // explicitly by a unit test): a qxa case that ran with the knob unset would
    // otherwise wipe the developer's real cache. Returns false when refused.
    //
    // It exists because the gate asserts an exact FETCH COUNT on a cold key,
    // and a cache directory that survives between runs would make the second
    // run of the case measure a hit where the first measured a fetch.
    bool clearAll();

    // How many times publish() actually wrote a file. The reuse assertion in
    // media_cache_test reads this; nothing in production does.
    int publishCount() const { return publishCount_; }
    void resetCounters() { publishCount_ = 0; }

private:
    SMediaCache();
    void ensureRoot() const;
    QString bucketFor( const QString &sourceId ) const;
    QString suffixFor( const SMediaRef &ref ) const;

    struct Meta {
        QString etag;
        qint64  size = -1;
        qint64  mtimeSecs = -1;
    };

    QString          root_;
    bool             enabled_  = true;
    bool             rootLocked_ = false;   // the env knob named it
    qint64           capMB_    = 2048;
    QHash<QString, Meta> known_;            // ref.toUri() -> what a listing saw
    int              publishCount_ = 0;
    mutable int      fetchSeq_ = 0;
};

#endif // SMEDIACACHE_H
