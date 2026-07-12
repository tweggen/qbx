
#ifndef _SOBJECT_H
#define _SOBJECT_H

#include <qobject.h>
#include "tw303aenv.h"
#include <qtextstream.h>
#include <QDomElement>
#include <QList>
#include <QSet>
#include <mutex>
#include <memory>
#include "capture_page_pool.h"
#include "revalidatable.h"

class QWidget;

class twComponent;
class twRandomSource;
class SProject;
class SLink;
class SObjectRenderer;
class CaptureRevalidator;


/**
 * A lightweight, storage-agnostic view over a container's ordered SLink
 * children. Consumers iterate with a range-for
 * (`for( SLink *lk : obj->childLinks() )`) or index with at()/size(), and stay
 * decoupled from how the order is actually stored (an explicit list today, the
 * QObject child list historically). Order is owned by the container; QObject
 * parentage is only about lifetime.
 */
class SChildLinks {
public:
    using const_iterator = QList<SLink*>::const_iterator;
    explicit SChildLinks( const QList<SLink*> &list ) : list_( list ) {}
    const_iterator begin() const { return list_.cbegin(); }
    const_iterator end() const { return list_.cend(); }
    int size() const { return list_.size(); }
    bool isEmpty() const { return list_.isEmpty(); }
    SLink *at( int i ) const { return list_.at( i ); }
private:
    const QList<SLink*> &list_;
};


/**
 * This is QBX generic data container.
 * All data containers are children of the project object.
 * They linked together by SLink objects.
 *
 * All things marked as properties are user editable.
 */
class SObject
    : public QObject,
      public IRevalidatable   // engine-side revalidator contract (proposal 14)
{
    Q_OBJECT
    Q_PROPERTY( bool Solo READ isSolo WRITE setSolo )
    Q_PROPERTY( bool Muted READ isMuted WRITE setMuted )
    Q_PROPERTY( bool ArmedForRecording READ isArmedForRecording WRITE setArmedForRecording )
    Q_PROPERTY( double Volume READ getVolume WRITE setVolume )
    Q_PROPERTY( double Pan READ getPan WRITE setPan )
    Q_PROPERTY( double Delay READ getDelay WRITE setDelay )
    Q_PROPERTY( QString SName READ getSName WRITE setSName )
        
//Q_PROPERTY( type name READ getFunction [WRITE setFunction]
//            [STORED bool] [DESIGNABLE bool] [RESET resetFunction])


public:
    SObject( SProject* );
    virtual ~SObject();

    // WARNING: Returns reference. Will crash if parent is null.
    // CRITICAL: Destructors MUST use getProjectSafe() instead.
    // This method asserts if called during destruction (parent becoming invalid).
    SProject &getProject() const {
        Q_ASSERT_X(parent() != nullptr, "SObject::getProject",
                   "Parent project is null - destructor should use getProjectSafe()");
        return *(SProject *)parent();
    }

    // Safe accessor: returns pointer (may be null) for use during destruction.
    // Destructors MUST use this instead of getProject() to handle the case
    // where the SObject is being destroyed while its parent is invalid.
    SProject *getProjectSafe() const;

    virtual int serialize( QTextStream & );
    virtual int readPreChildrenAttributes( QDomElement &element );
    virtual int readPostChildrenAttributes( QDomElement &element );

    /**
     * Return the DSP root component from the SObject.
     * Its outputs may be connected in various ways, as the SObject
     * may be included at different parts of the entire arrangement.
     */
    virtual twComponent &getRootComponent() = 0;

    /**
     * If this object is backed by random-access sample data, return that source
     * (proposal 07). Consumers use it to mint independent readers and to read
     * statelessly (e.g. preview rendering) without disturbing any play cursor.
     * The default returns NULL: most objects are not random-access sources.
     */
    virtual twRandomSource *getRandomSource() { return NULL; }

    /**
     * Return a widget suitable for full-screen editing this SObject.
     */
    virtual QWidget *getDetailEditWidget( QWidget *parent=NULL ) = 0;
    /**
     * Don't know what, but provided...
     */
    virtual QWidget *getInlineEditWidget( QWidget *parent=NULL ) = 0;
    
    virtual SObjectRenderer *getInlineRenderer() = 0;        

    /**
     * Seek the underlying twComponents to a given point, so that subsequent
     * playback will start there.
     * This method is provided inside the objects to facilate different underlying
     * interfaces.
     */
    virtual int seekTo( offset_t );

    /**
     * Map a clip-relative timeline position into this object's root component's
     * own position domain. Tracks compute clip-relative positions; a windowed
     * clip (SCut) plays a reader over the SOURCE material, so its slip offset
     * (grain-stretched as needed) must be folded in before the component is
     * seeked or page-frozen. Default is the identity: most objects' root
     * components are already clip-relative. Consumed via twView (see STrack).
     */
    virtual offset_t mapTimelineToComponentPos( offset_t off ) { return off; }

    /**
     * Ordered view of this container's SLink children. Prefer this and the
     * childAt()/childCount() accessors over QObject::children() everywhere order
     * matters, so call sites stay decoupled from the storage.
     */
    SChildLinks childLinks() const { return SChildLinks( childOrder_ ); }
    int childCount() const { return childOrder_.size(); }
    SLink *childAt( int index ) const;
    int indexOfChild( const SLink *child ) const;
    int indexOfChildObject( const SObject &child ) const;

    /**
     * Reorder this container's SLink children so the child currently at
     * fromIndex ends up at toIndex (the others shift to fill). No-op if either
     * index is out of range or they are equal. Order is just the explicit list,
     * so this is a plain list move — QObject parentage and refcounts are
     * untouched and no childObject signals fire.
     */
    void moveChildToIndex( int fromIndex, int toIndex );

    /**
     * Return the number of references to this object (from SLinks)
     */
    int getNReferences() const;
    
    /**
     * Returns true, if this object has no links to other objects.
     */
    virtual bool isEmpty() const;

    virtual bool hasDuration() const;
    virtual length_t getDuration() const;    
    
    virtual bool hasPreview() const;
    virtual int getPreview( preview_t *dest,
                    offset_t start, length_t length,
                    offset_t nProbes );

    /**
     * Phase 5e: Unified page cache API
     *
     * All SObjects use the same capture/revalidation system for preview,
     * playback, metadata, and export. This provides async, non-blocking
     * access to cached rendered output.
     *
     * Synchronization:
     * - currentPage_ pointer: atomic_load/store (lock-free reads)
     * - Page contents: protected by page->pageMutex
     * - Single mutex() per object: protects window parameters, not pages
     */

    // Non-blocking access to capture page (may be stale or invalid).
    // Returns immediately with current page if valid, or nullptr if not ready.
    // Never blocks on revalidation; schedules it if needed.
    // Returns stale data if available (acceptable for UI preview).
    std::shared_ptr<CapturePageData> getCapture(uint32_t aspectsMask);

    // Get current page without locking. Safe because shared_ptr copy is atomic
    // (uses std::atomic_load internally).
    std::shared_ptr<CapturePageData> currentPage() const {
        return std::atomic_load(&currentPage_);
    }

    // Check if revalidation is needed for specific aspects.
    // _nolock: caller must hold mutex() before calling.
    // Internal version used by revalidator when processing jobs.
    bool needsRevalidation_nolock(uint32_t aspectsMask) const;

    /**
     * Abstract interface for revalidation (Phase 5e).
     *
     * Each SObject type implements these to compute capture data.
     * Called by CaptureRevalidator worker threads with page lock held.
     *
     * Implementation notes:
     * - Called outside audio thread; can block
     * - Revalidator guarantees page is exclusive (not visible to readers)
     * - Aspects indicate which fields caller wants computed
     * - Set page.validAspects |= computedAspects before returning
     *
     * Note: recomputePreview() removed (Phase 5 — now uses freezePreviewPage)
     */
    virtual void recomputeMetadata(CapturePageData& page) {}
    virtual void recomputeExport(CapturePageData& page) {}

    // IRevalidatable (the engine-side revalidator contract): thin delegations
    // to the members above, preserving the historical dispatch exactly — the
    // *_nolock page methods bind statically to SObject's own implementations
    // (as the revalidator's SObject* calls always did), while the recompute
    // hooks and getRootComponent() stay virtual per object type.
    std::mutex &revalMutex() const override { return mutex(); }
    bool revalNeeded_nolock(uint32_t aspects) const override
        { return needsRevalidation_nolock(aspects); }
    std::shared_ptr<CapturePageData> revalGetNextPage_nolock() const override
        { return getNextPage_nolock(); }
    void revalSetNextPage_nolock(std::shared_ptr<CapturePageData> page) override
        { setNextPage_nolock(page); }
    void revalSwapPages_nolock() override { swapPages_nolock(); }
    twComponent &revalRootComponent() override { return getRootComponent(); }
    void revalRecomputeMetadata(CapturePageData &page) override
        { recomputeMetadata(page); }
    void revalRecomputeExport(CapturePageData &page) override
        { recomputeExport(page); }

    // User properties.
    bool isSolo() const
        { return solo_; }
    bool isMuted() const
        { return muted_; }
    bool isArmedForRecording() const
        { return armed_; }
    // Recording channel selection (bitmask: 0 = all channels, 1<<n = channel n)
    uint32_t getRecordingChannels() const
        { return recordingChannels_; }
    double getVolume() const
        { return volume_; }
    double getPan() const
        { return pan_; }
    double getDelay() const
        { return delay_; }
    QString getSName() const
        { return sName_; }    

public slots:
    void setSolo( bool );
    void setMuted( bool );
    void setArmedForRecording( bool );
    void setRecordingChannels( uint32_t channels );
    void setVolume( double );
    void setPan( double );
    void setDelay( double );
    void setSName( const QString & );    

signals:
    // For the properties
    void soloChanged( bool );
    void mutedChanged( bool );
    void armedForRecordingChanged( bool );
    void recordingChannelsChanged( uint32_t );
    void volumeChanged( double );
    void panChanged( double );
    void delayChanged( double );
    void sNameChanged( const QString & );


    /**
     * Ths link's duration changed.
     */
    void durationChanged( length_t newDuration );

    /**
     * Child object was added.
     */
    void childObjectAdded( SLink &child );
    
    /**
     * Child object was removed.
     * At calling time, the link is not yet deleted.
     */ 
    void childObjectRemoved( SLink &child );

    /**
     * This signal is emitted, if the object becoms unreferenced.
     */
    void gotUnreferenced();

    /**
     * This signal is emitted, if the object receives its first reference.
     */
    void gotReferenced();

    void nRefsChanged();

public slots:
    /**
     * Set a different duration.
     */
    virtual void setDuration( length_t );

    /**
     * Add a reference.
     */
    void addRef();

    /**
     * Remove a reference.
     */
    void removeRef();

    /**
     * Forget the current preview.
     */
    virtual void invalidatePreview();

    /**
     * Notify dependents (objects that reference this one) that specific aspects
     * have changed. Only affected dependents are invalidated (lazy invalidation).
     * Called by audio state changes (mute, solo, volume) that don't affect arrangement.
     * Example: setMuted() → notifyDependentsChanged(Playback | Metadata)
     */
    void notifyDependentsChanged(uint32_t affectedAspects);

    /**
     * Register a dependent link (object that references this one via SLink).
     * Called when an asset is placed or a cut references this object.
     * Uses SLink (the native reference primitive) to track who depends on this object.
     */
    void addDependentLink(SLink *dependentLink);

    /**
     * Unregister a dependent link. Called when a placement or cut is removed.
     */
    void removeDependentLink(SLink *dependentLink);

    // Helper methods for revalidator integration (Phase 5e).
    // _nolock suffix indicates caller MUST hold mutex() before calling.
    // These are friends-only methods, non-locking to avoid recursive lock deadlock.
    friend class CaptureRevalidator;

    // Atomic swap pages. _nolock: caller must hold mutex()
    // Uses std::atomic_store for thread-safe write (pairs with atomic_load in currentPage()).
    void swapPages_nolock() {
        std::atomic_store(&currentPage_, nextPage_);
        nextPage_ = nullptr;
    }

    // Get next capture page. _nolock: caller must hold mutex()
    std::shared_ptr<CapturePageData> getNextPage_nolock() const {
        return nextPage_;
    }

    // Set next capture page. _nolock: caller must hold mutex()
    void setNextPage_nolock(std::shared_ptr<CapturePageData> page) {
        nextPage_ = page;
    }

protected:
    /**
     * Thread safety: all derived classes use this mutex to protect their state.
     * Single mutex per object; see async_revalidation_phase4.md for rationale.
     * Usage: std::lock_guard<std::mutex> lock(mutex());
     */
    std::mutex& mutex() const {
        return stateMutex_;
    }

    offset_t getChildrenExtent( offset_t &firstStart, offset_t &lastEnd,
                                int &nUndefStart, int &nUndefDuration ) const;
    offset_t getFirstChildStartTime() const;
    length_t getAllChildsDuration() const;

    int getStraightPreview( preview_t *, offset_t, length_t, offset_t );
    virtual void childEvent( QChildEvent * );

    virtual int serializeSelfAttributes( QTextStream &o );

    int getChildIndex( SObject & ) const;

protected:
    // Thread safety: mutex for all derived class state (single mutex per object).
    // Mutable so const methods can lock. Protected by mutex() accessor.
    // All derived classes should protect their state with this mutex.
    mutable std::mutex stateMutex_;

    // Phase 5e: Page cache infrastructure (unified across all SObjects).
    // Two-page buffer model (Unix page cache pattern):
    // - currentPage_: visible to readers (via atomic_load)
    // - nextPage_: being built by revalidator (exclusive, not yet visible)
    // When revalidator finishes, it atomic_swaps them.
    //
    // Access synchronization:
    // - Pointer itself: atomic_load/store (no mutex needed)
    // - Page contents: protected by page->pageMutex
    // - Window parameters (startOffset, duration, etc.): protected by stateMutex_
    std::shared_ptr<CapturePageData> currentPage_;
    std::shared_ptr<CapturePageData> nextPage_;

    // Revalidator: borrowed from SProject (not owned).
    // Spawned background threads that build pages asynchronously.
    CaptureRevalidator* revalidator_ = nullptr;

    // Bitmask tracking which aspects are valid in currentPage_.
    // Updated by revalidator when pages are swapped and marked complete.
    uint32_t validAspects_ = 0;

private:

    void gotChild( SLink & );
    void lostChild( SLink & );
    int straightCalcPreviewData();
    // Source of truth for child order (membership mirrors QObject::children(),
    // maintained in childEvent(); order is independent and set by
    // moveChildToIndex()).
    QList<SLink*> childOrder_;

    // Lazy invalidation + dependency tracking (proposal 06).
    // Set of SLink objects that reference this object (the native way to track references).
    // When this object's state changes, dependent links are notified.
    // Example: track output is captured by a cut link; edit track mute → cut's
    // Playback aspect invalidated, not entire scene.
    mutable std::mutex dependentsMutex_;
    QSet<SLink*> dependentLinks_;
    int nRefs_;
    offset_t previewForLength_;
    offset_t nPreviewProbes_;
    preview_t *previewData_;
    offset_t previewSkip_;

    bool solo_;
    bool muted_;
    bool armed_;
    double volume_;
    // Recording channel selection: bitmask of channels (bit 0 = ch 0, etc)
    // 0 means "all channels" (default). Set via setRecordingChannels().
    uint32_t recordingChannels_ = 0;

    // Thread-safe state: audio thread may read volume while UI thread modifies it.
    // Made public so preview rendering can snapshot the volume safely.
public:
    mutable std::mutex volumeMutex_;
private:
    double pan_;
    double delay_;
    QString sName_;
};

#endif
