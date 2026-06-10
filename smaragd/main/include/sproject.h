
#ifndef _SPROJECT_H
#define _SPROJECT_H

#include <qobject.h>
#include <qstring.h>
//#include <qptrlist.h>
#include <qhash.h>
#include <qtextstream.h>
#include <QVariant>
#include <QVariantMap>
#include <QList>

#include <cstdint>
#include <vector>

class QDomElement;

class SObject;
class SExternFile;
class SLink;

struct SObjectRegistryEntry;

class SProject 
    : public QObject
{
    Q_OBJECT
public:
    SProject();
    virtual ~SProject();

    const QString &getFileName();
    SObject *getRootComponent() const;
    void setRootComponent( SObject * );

    SLink *linkToFile( QString & );

    virtual int serialize( QTextStream & );
    double getBPMTempo() const { return bpmTempo_; }
    int serializeSelfAttributes( QTextStream & );
    int readPreChildrenAttributes( QDomElement &element );

    // Project sample rate (persisted; default 48000 for a fresh project, 44100
    // when loading a pre-sample-rate file). Applied to tw303aEnvironment when
    // the project becomes current.
    int getSRate() const { return sampleRate_; }
    const std::vector<std::uint32_t> &candidateRates() const { return candidateRates_; }

    // Render-related queries
    // Total project duration in seconds (based on arrangement length)
    double getDurationSeconds() const;

    // Time selection (locator in/out markers or arrangement selection)
    bool hasTimeSelection() const;
    struct TimeRange {
        double startSeconds;
        double endSeconds;
    };
    TimeRange getTimeSelection() const;
    void setTimeSelection(double startSeconds, double endSeconds);
    void clearTimeSelection();

    // Generic per-project property store (a QVariantMap; serialized as JSON).
    // Named prop/setProp/hasProp rather than property/setProperty to avoid
    // shadowing QObject's meta-property API. Well-known keys + defaults live in
    // sprojectprops.h.
    QVariant prop( const QString &key, const QVariant &defaultValue = QVariant() ) const;
    void setProp( const QString &key, const QVariant &value );
    bool hasProp( const QString &key ) const;
    const QVariantMap &properties() const { return properties_; }

    // --- Live asset registry (proposal 05 feature (b)) -------------------
    // An asset is a named, shared SObject (today: an SCut windowing a container
    // — the root mixer or a folder track) kept as a reusable resource,
    // independent of where it is placed in the arrangement. The registry pins
    // one reference so the asset survives with zero placements; editing the
    // underlying container changes every placement live (the SObject is pulled
    // each buffer). See plan/proposed/05_TRACK_GROUPING_AND_LIVE_ASSETS.md.
    void registerAsset( const QString &name, SObject *body );
    void unregisterAsset( const QString &name );
    SObject *asset( const QString &name ) const;
    bool hasAsset( const QString &name ) const;
    QList<QString> assetNames() const;

    // Fire arrangementChanged(). Called from the action chokepoint after every
    // applied action so cached renders (asset captures) invalidate transparently
    // on any edit. Coarse by design (every action, not just arrangement edits);
    // over-invalidation only costs a re-render, never correctness.
    void notifyArrangementChanged() { emit arrangementChanged(); }

signals:
    void fileNameChanged( const QString & );
    void assetAdded( const QString &name, SObject &body );
    void assetRemoved( const QString &name );
    // Any applied action; consumers that cache rendered audio drop their cache.
    void arrangementChanged();
    void externFileAdded( const SExternFile & );
    void externFileRemoved( const QString );
    void bpmTempoChanged( double );
    void sampleRateChanged( int );
    void propertyChanged( const QString &key, const QVariant &value );

public slots:
    void setFileName( const QString & );
    void addExternObject( const SExternFile & );
    void removeExternObject( QString & );
    void setBPMTempo( double );
    void setSRate( int );
    void setCandidateRates( std::vector<std::uint32_t> );

protected:

private slots:

private:
    QString fileName_;
    SLink *soRoot_;
    double bpmTempo_;
    int sampleRate_;
    std::vector<std::uint32_t> candidateRates_;
    QVariantMap properties_;

    QHash<QString,SExternFile*> externFileDict_;
    QHash<QString,SObject*> assetDict_;
};

#endif
