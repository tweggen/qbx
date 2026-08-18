// app/media/smediaregistry.h — id -> source, one per process.
//
// Proposal 38 §B.2/§B.4: the dock picks a source by ID, a drag payload names
// one by ID, and gate 3's deferred placement resolves one by ID long after the
// row that produced it is gone. So the id has to resolve somewhere, and that
// somewhere must survive a source being deleted underneath it.
//
// THE REGISTRY DOES NOT OWN ITS SOURCES. It connects to QObject::destroyed and
// forgets a source that dies, which is the only arrangement that keeps
// `source(id)` honest when gate 5 removes an account (and the only one gate 1
// AC 12 — delete a source mid-walk — can be written against). Ownership stays
// with whoever created the source; the app's composition root.

#ifndef SMEDIAREGISTRY_H
#define SMEDIAREGISTRY_H

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>

class SMediaSource;

class SMediaRegistry : public QObject
{
    Q_OBJECT

public:
    static SMediaRegistry &instance();

    // Registering a second source under a live id REPLACES the entry and logs
    // it — a silent replace is how two accounts end up sharing one connection.
    void registerSource( SMediaSource *source );
    void unregisterSource( const QString &id );

    SMediaSource *source( const QString &id ) const;
    QStringList   sourceIds() const;          // sorted
    int           count() const;

signals:
    void sourcesChanged();

private:
    explicit SMediaRegistry( QObject *parent = nullptr );

    void onSourceDestroyed( QObject *dead );

    QHash<QString, SMediaSource *> sources_;
};

#endif // SMEDIAREGISTRY_H
