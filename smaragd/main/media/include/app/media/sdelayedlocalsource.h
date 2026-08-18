// app/media/sdelayedlocalsource.h — a TEST-ONLY provider that is remote-SHAPED.
//
// Proposal 38 gate 3. The deferred branch of a `media:` drop only exists for a
// source that reports NeedsFetch, and gate 3 has no network — so without this
// there is nothing to gate it against but a real server's timing. It is the
// same instinct as `twtestclap`: an in-repo fixture that makes a path testable
// without installing anything, and deterministic where the real thing is not.
//
// It reads the SAME committed fixture tree the local provider does
// (smaragd/tests/media/), reports NeedsFetch, and COPIES on fetch after a
// configurable delay — with a configurable failure, because "the fetch failed"
// is an acceptance criterion (gate 3 AC 4) and a real server cannot be asked to
// fail on cue.
//
// IT IS REGISTERED ONLY WHEN `SMARAGD_MEDIA_TEST_SOURCE=1`. A user must never
// find a "Delayed local (test)" entry in the source combo; the browser panel
// asks isEnabled() before constructing one.
//
// Listing is INHERITED verbatim from SLocalMediaSource — the walk, the private
// pool, the queued hand-back, the batching, the supersession. Only id(),
// caps() and fetch() differ, which is the whole point: what is under test is
// the DEFERRED PLACEMENT, not a second file-system walker.

#ifndef SDELAYEDLOCALSOURCE_H
#define SDELAYEDLOCALSOURCE_H

#include "app/media/slocalmediasource.h"

#include <QHash>
#include <QString>

class SDelayedLocalSource : public SLocalMediaSource
{
    Q_OBJECT

public:
    // SMARAGD_MEDIA_TEST_SOURCE=1. Read once per process.
    static bool isEnabled();

    static QString sourceIdString();   // "testdelay"

    explicit SDelayedLocalSource( QObject *parent = nullptr );
    ~SDelayedLocalSource() override;

    QString id() const override;
    QString displayName() const override;
    // NeedsFetch is the whole reason this class exists: it is what makes the
    // browser emit a `media:` payload and the drop take the deferred branch.
    int     caps() const override;

    // Copies filePath -> destPath after fetchDelayMs(), then emits
    // fetchFinished. A path CONTAINING failSubstring() fails instead, with a
    // message naming it.
    int  fetch( const QString &filePath, const QString &destPath ) override;
    void cancel( int requestId ) override;

    void setFetchDelayMs( int ms );
    int  fetchDelayMs() const { return delayMs_; }
    void setFailSubstring( const QString &needle );
    QString failSubstring() const { return failNeedle_; }

    // How many fetches actually COMPLETED (a cached second drop must not add
    // to this — gate 3 AC 1's reuse assertion).
    int fetchCount() const { return fetchCount_; }
    void resetFetchCount() { fetchCount_ = 0; }

private:
    QHash<int, bool> liveFetches_;
    int              delayMs_    = 150;
    QString          failNeedle_;
    int              fetchCount_ = 0;
};

#endif // SDELAYEDLOCALSOURCE_H
