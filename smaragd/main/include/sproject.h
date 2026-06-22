
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
#include <memory>

#include "twfraction.h"
#include "capture_page_pool.h"

class QDomElement;

class SObject;
class SExternFile;
class SLink;
class CaptureRevalidator;

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

    // Mark project as partial/corrupt so destructor skips risky cleanup
    void markAsPartialLoad();
    bool isPartialLoad() const { return isPartialLoad_; }

    virtual int serialize( QTextStream & );
    double getBPMTempo() const { return bpmTempo_; }
    int serializeSelfAttributes( QTextStream & );
    int readPreChildrenAttributes( QDomElement &element );

    // Project sample rate (persisted; default 48000 for a fresh project, 44100
    // when loading a pre-sample-rate file). Applied to tw303aEnvironment when
    // the project becomes current.
    int getSRate() const { return sampleRate_; }
    const std::vector<std::uint32_t> &candidateRates() const { return candidateRates_; }

    // Position factor for time-based coordinates (exact rational arithmetic).
    // Default: 1/sampleRate (positions measured in sample units).
    // Can be overridden at any hierarchy level for stretching/transformation.
    Fraction getPosFactor() const { return posFactor_; }
    void setPosFactor( const Fraction &factor ) { posFactor_ = factor; }

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

    // Async capture revalidation (Phase 4).
    // Access to pool and revalidator for SCut integration.
    CaptureRevalidator* getRevalidator() { return revalidator_.get(); }
    CapturePagePool* getPagePool() { return pagePool_.get(); }

    // Suppress capture invalidation during project loading.
    // During deserialization, all pages are empty anyway, so there's no point
    // in triggering invalidation chains. Wrap entire load sequence in:
    //   project->disableInvalidation();
    //   // ... load from XML ...
    //   project->enableInvalidation();  // Triggers full revalidation of all cuts
    // All setters work normally; invalidation is simply deferred until load completes.
    void disableInvalidation() { invalidationSuppressed_++; }
    void enableInvalidation();  // Triggers revalidation pass for loaded cuts
    bool isInvalidationSuppressed() const { return invalidationSuppressed_ > 0; }

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
    Fraction posFactor_;  // Time coordinate scaling (default: 1/sampleRate)
    std::vector<std::uint32_t> candidateRates_;
    QVariantMap properties_;

    QHash<QString,SExternFile*> externFileDict_;
    QHash<QString,SObject*> assetDict_;

    bool isPartialLoad_ = false;  // True if load failed partway through

    // Invalidation suppression counter (for project loading).
    // When > 0, invalidateAspects() returns early without scheduling revalidation.
    // Incremented by disableInvalidation(), decremented by enableInvalidation().
    // Counter allows nested disable/enable pairs (e.g., subcomponents that also load).
    int invalidationSuppressed_ = 0;

    // Async capture revalidation (Phase 4).
    // Pre-allocated page pool and worker thread pool for non-blocking capture access.
    std::unique_ptr<CapturePagePool> pagePool_;       // 512MB pool (2048 pages)
    std::unique_ptr<CaptureRevalidator> revalidator_;  // 8 worker threads (configurable)
};

#endif
