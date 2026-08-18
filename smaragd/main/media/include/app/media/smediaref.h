// app/media/smediaref.h — the addressable identity of one media item.
//
// Proposal 38 §B.2. A media item is named by (sourceId, source-relative path)
// and by nothing else: a browser row, a drag payload, a cache key and a
// pending placement all quote the SAME pair, so there is one spelling to keep
// in step instead of four.
//
// The URI form is deliberately NOT a QUrl. A source id may itself contain a
// colon ("nextcloud:<accountId>"), which QUrl would read as a port in the
// authority, and the path is stored DECODED — percent-encoding it here would
// mean every consumer had to know whether it held the encoded or the decoded
// spelling. The format is therefore the plain concatenation
//
//     smedia://<sourceId>/<path>
//
// split at the FIRST '/' after the authority, which leaves a Windows path
// ("C:/samples/kick.wav") intact on the right-hand side.

#ifndef SMEDIAREF_H
#define SMEDIAREF_H

#include <QDateTime>
#include <QList>
#include <QMetaType>
#include <QString>
#include <QVector>

struct SMediaRef
{
    QString sourceId;   // "local" | "nextcloud:<accountId>"
    QString path;       // source-relative, '/'-separated, never percent-encoded

    SMediaRef() = default;
    SMediaRef( QString source, QString p )
        : sourceId( std::move( source ) ), path( std::move( p ) ) {}

    bool isValid() const { return !sourceId.isEmpty(); }

    QString toUri() const;
    static SMediaRef fromUri( const QString &uri );

    bool operator==( const SMediaRef &o ) const
    {
        return sourceId == o.sourceId && path == o.path;
    }
    bool operator!=( const SMediaRef &o ) const { return !( *this == o ); }
};

struct SMediaEntry
{
    QString   name;              // display name, decoded
    SMediaRef ref;
    bool      isDir     = false;
    qint64    sizeBytes = -1;    // -1 unknown
    QDateTime modified;          // invalid = unknown
    QString   etag;              // remote only; "" locally
};

Q_DECLARE_METATYPE( SMediaRef )
Q_DECLARE_METATYPE( SMediaEntry )
Q_DECLARE_METATYPE( QVector<SMediaEntry> )

namespace smedia {
// Registers the metatypes above. A batch travels over a QUEUED connection
// (the provider ABI's whole point), and a queued argument must be a
// registered metatype or the emission is dropped with a runtime warning
// nobody reads. Called from SMediaSource's constructor, so no caller has to
// remember it.
void registerMediaMetaTypes();
} // namespace smedia

#endif // SMEDIAREF_H
