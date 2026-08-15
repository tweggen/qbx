
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

#include "tw/core/twfraction.h"
#include "tw/pages/capture_page_pool.h"

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

    /**
     * Wire-format version written into <SProject formatVersion='...'>.
     *
     * 1 = every project written before proposal 36 (the attribute is absent
     *     there, so a reader that finds nothing must assume 1).
     * 2 = the loader's prune-and-retry recovery per element kind (D8a): a
     *     document written by this build may rely on a reader that keeps a
     *     container whose child is missing.
     *
     * A reader NEVER refuses a higher version. Refusing would make a project
     * unopenable by the very builds a user is most likely to have, and every
     * element is skippable by name anyway (an unknown element is warned about
     * and dropped); it warns instead, so a load that then looks odd has a
     * printed reason.
     */
    static constexpr int FORMAT_VERSION = 2;

    /** The version this document declared (1 when the attribute was absent). */
    int formatVersion() const { return formatVersion_; }

    const QString &getFileName();

    // Where this project lives on disk (absolute, '/'-separated; empty for an
    // untitled project). Set by SSaveProjectAction / SLoadProjectAction, which
    // are the only two paths in or out of a .qxp.
    //
    // NOT serialized, and deliberately separate from fileName_: this is the
    // ANCHOR external file references are stored relative to (SFilePathRef), so
    // it has to describe where the file is RIGHT NOW, not where it was when the
    // document was written. A path baked into the file would be exactly wrong
    // after the project folder is moved or copied to another machine — the case
    // relative storage exists to survive.
    void setProjectFilePath( const QString &path );
    const QString &projectFilePath() const { return projectFilePath_; }

    SObject *getRootComponent() const;
    void setRootComponent( SObject * );

    SLink *linkToFile( QString & );

    // Base directory for resolving RELATIVE sample paths passed to linkToFile
    // (set by the action-script runner to the .qxa's directory). When set and a
    // relative path exists under it, linkToFile rewrites the path to that
    // absolute location, so scripts find their fixtures regardless of the
    // process working directory. Empty (default) = resolve against the CWD, as
    // before — interactive/GUI use is unaffected.
    void setSampleBaseDir( const QString &dir ) { sampleBaseDir_ = dir; }
    QString sampleBaseDir() const { return sampleBaseDir_; }
    // Factory for creating an extern-file object from a path, registered by
    // the owning slice (SPlainWave) from a static initializer — the model
    // names no concrete types (proposal 14, Phase 5). Returns NULL on load
    // failure (the factory owns cleanup of its partial object).
    typedef SExternFile *(*ExternFileFactory)( SProject *, QString &fileName );
    static void registerExternFileFactory( ExternFileFactory );

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
    // Reverse lookup: the registered name of an asset BODY, or empty when the
    // object is not a registered asset. A placement links the body itself
    // (SPlaceAssetAction), so this is how a deletion tells "this clip IS the
    // asset" (undo re-places it, keeping identity) from "this clip is a copy
    // that merely windows the same container" (undo rebuilds a cut).
    QString assetNameOf( const SObject *body ) const;

    // Fire arrangementChanged(). Called from the action chokepoint after every
    // applied action so cached renders (asset captures) invalidate transparently
    // on any edit. Coarse by design (every action, not just arrangement edits);
    // over-invalidation only costs a re-render, never correctness.
    void notifyArrangementChanged() { emit arrangementChanged(); }

    // Fire captureRevalidated(). Invoked (queued) from revalidator worker
    // threads when an async recompute lands, so views repaint and pick up the
    // fresh preview. Deliberately NOT arrangementChanged: that one also drops
    // caches, which would re-schedule the very revalidation that completed.
    Q_INVOKABLE void notifyCaptureRevalidated() { emit captureRevalidated(); }

    // Async capture revalidation (Phase 4).
    // Access to pool and revalidator for SCut integration.
    CaptureRevalidator* getRevalidator() { return revalidator_.get(); }
    CapturePagePool* getPagePool() { return pagePool_.get(); }

    // Proposal 19 Phase 2b: quiesce background revalidation for an exclusive
    // offline render (the render owns the graph; "offline renders stay exact").
    // pauseRevalidation() blocks until in-flight worker jobs drain. Wrappers so
    // app/shell need not include tw/schedule (layering). No-op if no revalidator.
    void pauseRevalidation();
    void resumeRevalidation();

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

    // Read-only access to the loaded extern files / assets, so views (e.g. the
    // extern-file list) can back-fill their rows for a project that was already
    // populated during load — before they connected to the add/remove signals.
    const QHash<QString,SExternFile*> &externFiles() const { return externFileDict_; }
    const QHash<QString,SObject*> &assets() const { return assetDict_; }

signals:
    void fileNameChanged( const QString & );
    void assetAdded( const QString &name, SObject &body );
    void assetRemoved( const QString &name );
    // Any applied action; consumers that cache rendered audio drop their cache.
    void arrangementChanged();
    // An async capture revalidation landed (preview/metadata fresh); views
    // repaint but nothing invalidates (see notifyCaptureRevalidated()).
    void captureRevalidated();
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
    QString projectFilePath_; // see setProjectFilePath()
    QString sampleBaseDir_;   // see setSampleBaseDir()
    SLink *soRoot_;
    double bpmTempo_;
    int sampleRate_;
    // Declared wire-format version of the document that was loaded; 1 for a
    // file with no formatVersion attribute (see FORMAT_VERSION).
    int formatVersion_ = FORMAT_VERSION;
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
