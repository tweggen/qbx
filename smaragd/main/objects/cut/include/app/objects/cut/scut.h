
#ifndef _SCUT_H_
#define _SCUT_H_

#include <qobject.h>
#include <mutex>
#include <atomic>
#include <memory>
#include "app/model/sobject.h"
#include "app/model/slink.h"
#include "tw/sources/twgrainparams.h"
#include "tw/core/twfraction.h"
#include "tw/core/twdomains.h"
#include "tw/core/twtimemap.h"
#include "tw/pages/capture_page_pool.h"

class SProject;
class QWidget;
class twComponent;
class twRandomSource;
class twSampleReader;
class twGrainSource;
class twCapturingSource;
class SObjectRenderer;
class SCutRendererInline;
class SProjectLoader;
class CaptureRevalidator;

// Capture aspect bitmask (Preview/Playback/Metadata/Export) moved to the
// engine — the revalidator dispatches on these bits (proposal 14, Phase 0).
// capture_aspects.h keeps the SCutCaptureAspect name as an alias.
#include "tw/schedule/capture_aspects.h"

/**
 * A cut (slice) of content with timing information.
 *
 * Thread affinity: MIXED (synchronized via snapshot)
 *
 * Synchronization strategy (multithreading policy: Phase 1):
 * - Window parameters (startOffset_, loopStart_, loopLength_, cutDuration_, grainParams_):
 *   Protected by windowMutex_. UI thread modifies freely (with lock). Audio thread calls
 *   getSnapshot() to get immutable copy, avoiding lock contention during rendering.
 * - reader_, grain_, looping_: Part of snapshot; rebuilt by UI thread only, read via snapshot.
 * - content_: Accessed from UI thread (getDetailEditWidget, rendering) AND audio thread
 *   (getRootComponent→calcOutputTo). See SPlainWave thread affinity (file handle race).
 *
 * Execution paths:
 *   UI:    SMVActualView::paintEvent() → draw(SLink) → SPlainWaveRendererInline::draw()
 *   Audio: CoreAudio callback → rendering → getRootComponent()->calcOutputTo()
 */
// Double-buffer reader state: always has a complete, committed version (Unix page cache model).
// Audio thread always reads "current"; UI thread builds "next", then swaps atomically.
//
// ALL pointers are refcounted (shared_ptr) to prevent use-after-free:
// - Audio thread takes snapshot of currentReader_ (increments refcounts)
// - UI thread swaps readers (decrements old refcounts)
// - Objects not deleted until ALL readers release their snapshots
struct SCutReaderState {
    std::shared_ptr<twSampleReader> reader;      // Refcounted: always valid or null
    std::shared_ptr<twGrainSource> grain;        // Refcounted: optional grain processor
    std::shared_ptr<twCapturingSource> captureRef;  // Refcounted: keeps backing source alive
    bool looping = false;                        // True iff reader is a twLoopReader
    int generation = 0;                          // Incremented on each swap
};

// Window parameter change event queue for async processing after drag operations.
enum SCutWindowParamEventType {
    OFFSET_CHANGE,
    DURATION_CHANGE,
    LOOP_LENGTH_CHANGE,
    STRETCH_CHANGE
};
struct SCutWindowParamEvent {
    SCutWindowParamEventType type;
    double value;
};

// Immutable snapshot of SCut's playback window parameters for lock-free audio-thread reads.
// Audio thread takes a snapshot at buffer boundaries; UI thread modifies the original.
// ReaderState is swapped atomically (Unix page cache model).
// Window values carry their domain in the type (proposal 18 Phase 1, see
// tw/core/twdomains.h): the cut window lives in the grain OUTPUT (warped)
// domain; the clip duration is a clip-relative (timeline-rate) length.
struct SCutSnapshot {
    WarpedPos startOffset;       // Warped position where the clip window begins
    WarpedLen loopLength;        // Loop segment length (warped); 0 = no loop
    ClipLen   cutDuration;       // Timeline duration of the clip
    twGrainParams grainParams;   // Time-stretch/pitch-shift parameters
    SCutReaderState reader;      // Double-buffered reader state (always complete & valid)
};

class SCut
    : public SObject
{
    Q_OBJECT
    Q_PROPERTY( double Stretch READ getStretch WRITE setStretch )
    Q_PROPERTY( double PitchCents READ getPitchCents WRITE setPitchCents )
public:
    SCut( SProject *parentProject, SObject &content );
    SCut( SProject *parentProject, SLink &content );
    virtual ~SCut();

    static SLink *instantiateFromDomElement( SProjectLoader &projectLoader, 
					     QDomElement &element, 
					     SObject *parent );

    // Immutable snapshot of playback parameters for lock-free audio-thread reads.
    // Reader state is double-buffered (Unix page cache model): currentReader_ is
    // always complete & valid. Safe to call from audio thread.
    SCutSnapshot getSnapshot() const;

    virtual std::shared_ptr<twComponent> getRootComponent();
    virtual QWidget *getDetailEditWidget( QWidget *parent );
    virtual QWidget *getInlineEditWidget( QWidget *parent );
    virtual SObjectRenderer *getInlineRenderer();    

    virtual int readPostChildrenAttributes( QDomElement &element );
    
    virtual int seekTo( offset_t );
    // Fold the slip offset (grain-stretched when a grain stage is active) into
    // a clip-relative position, so tracks can seek/freeze our reader directly.
    // A looping reader is already cut-relative and maps identically.
    virtual offset_t mapTimelineToComponentPos( offset_t off );
    SObject &getContent() const { return content_->getSObject(); }
    WarpedPos getLoopStart() const;
    // The clip's slip anchor. AUTHORITATIVE storage is the SOURCE-domain
    // anchor srcStart_ (proposal 18 Phase 3): it points into concrete
    // material and is invariant under stretch edits, so re-stretching can
    // never drift it. The warped-domain offset is DERIVED exactly and
    // floored once here — the render-boundary rounding rule.
    const Fraction &getSrcStart() const { return srcStart_; }
    WarpedPos getStartOffset() const {
        return WarpedPos( ( srcStart_ * grainParams_.stretch ).floorToInt() );
    }
    // Length of the segment that repeats when this cut is longer than it (the
    // "previously visible cut" captured by the right-upper edge loop gesture).
    // 0 means no loop; the loop is active iff 0 < loopLength_ < cutDuration_.
    WarpedLen getLoopLength() const { return loopLength_; }
    bool isLooping() const {
        return loopLength_ > WarpedLen(0)
            && loopLength_ < warpedFromClip(cutDuration_);
    }
    // Set the loop length / stretch / full grain params for drawing only, without
    // rebuilding the audio chain or the preserve-span rescale (used for live drag
    // feedback and for cloning; the chain is rebuilt once afterwards, e.g. by
    // setWindow on release).
    void setLoopLengthRaw( WarpedLen l ) { loopLength_ = l; }
    // Changing the stretch does NOT move the source anchor — that is the
    // point of source-authoritative storage (the old per-edit offset
    // rescale, and its rounding, are gone).
    void setStretchRaw( const Fraction &s ) { grainParams_.stretch = s; }
    void setSrcStartRaw( const Fraction &s ) { srcStart_ = s; }
    // Warped-domain setter (gestures/actions that measured in the warped
    // domain): converts EXACTLY via the current stretch.
    void setStartOffsetRaw( WarpedPos o ) {
        srcStart_ = grainParams_.stretch > Fraction(0)
                  ? Fraction( o.frames() ) / grainParams_.stretch
                  : Fraction( o.frames() );
    }
    void setDurationRaw( ClipLen d ) { cutDuration_ = d; }
    void setGrainParamsRaw( const twGrainParams &p ) { grainParams_ = p; }
    virtual bool hasDuration() const { return true; }
    virtual length_t getDuration() const;

    // Waveform preview for a container-backed asset cut: peaks come from the
    // capture (the rendered snapshot shared with audio), in the container's frame
    // domain, so `start` is the container offset the cut window maps to. Sample
    // cuts have no capture here and fall back to the base preview. Tier 2 of the
    // asset-preview work.
    virtual int getPreview( preview_t *dest, offset_t start, length_t length,
                            offset_t nProbes );

    // Set the whole clip window at once (slip offset, timeline duration, loop
    // segment length, grain stretch) and rebuild the playback chain exactly
    // once. Unlike setGrainParams() this does NOT preserve-span-rescale — the
    // caller supplies already-final values. The undoable form of the clip-edge
    // gestures (see SResizeClipAction).
    void setWindow( const Fraction &srcStart, ClipLen duration,
                    WarpedLen loopLength, const Fraction &stretch );
    // Source-anchor slip with capture invalidation (exact counterpart of
    // the warped setStartOffset slot).
    void setSrcStart( const Fraction &srcStart );

    // Shared time maps (proposal 18 Phase 4). BOTH the audio path and the
    // waveform preview consume these - there is no second implementation
    // of the clip's position mapping to disagree with.
    //
    // clipToSourceMap: clip-relative timeline position -> SOURCE sample,
    // source = srcStart + rel/stretch, exact. (For a looping clip this maps
    // a position WITHIN one repetition - the tiling itself is the reader's
    // twLoopMap.)
    twAffineMap clipToSourceMap() const {
        Fraction st = grainParams_.stretch > Fraction(0) ? grainParams_.stretch
                                                         : Fraction(1);
        return twAffineMap( Fraction(1) / st, srcStart_ );
    }
    // clipToReaderMap: clip-relative position -> the reader's own domain
    // (what twView::MapPosFn needs). A looping reader is cut-relative
    // (identity - it owns its loop base internally via twLoopMap); a plain
    // reader is addressed in the warped domain, so the slip offset folds in.
    twAffineMap clipToReaderMap( bool looping ) const {
        return looping ? twAffineMap( Fraction(1), Fraction(0) )
                       : twAffineMap( Fraction(1),
                                      Fraction( getStartOffset().frames() ) );
    }

    // Grain time-stretch / pitch-shift parameters for this clip (proposal 06).
    // The stretch is an EXACT rational (proposal 18 Phase 2); getStretch()
    // is the APPROXIMATE view for display/pixel math and the Qt property.
    const Fraction &getStretchExact() const { return grainParams_.stretch; }
    double getStretch() const { return grainParams_.stretch.approxDouble(); }
    double getPitchCents() const { return grainParams_.pitchCents; }
    const twGrainParams &getGrainParams() const { return grainParams_; }
    void setGrainParams( const twGrainParams & );

signals:
    // Emitted when window parameters change (slip, stretch, loop, trim, extend)
    // during a drag. Parent containers use this to invalidate their renders.
    void windowParamsChanged();

public slots:
    virtual void setLoopStart( offset_t );
    virtual void setLoopLength( length_t );
    virtual void setStartOffset( offset_t );
    virtual void setDuration( length_t );
    void setStretch( double );
    void setPitchCents( double );

    // Queue a window parameter change event for later processing. Used during
    // drag operations to avoid calling invalidateCapture() on the critical path.
    void queueWindowParamEvent( SCutWindowParamEventType type, double value );
    void processWindowParamEvents();  // Called after drag to apply queued changes

    // Drop the cached render of a container content (and any reader built over
    // it); the next pull re-captures. Connected to SProject::arrangementChanged
    // so a cut over a group/mixer transparently reflects edits (proposal 05
    // feature (b) / 07 step 5). Also called during dragging for live preview
    // feedback. Only container-backed cuts ever use this.
    // Async model: schedules revalidation, returns immediately (no hang).
    void invalidateCapture();

    // Lazy invalidation + aspect-based caching (Phase 3/4).
    // Invalidate specific aspects of the capture (Preview, Playback, Metadata, Export).
    // Schedules async revalidation to background thread; returns immediately.
    void invalidateAspects(uint32_t aspects) override;

    // Non-blocking capture access: get current/stale page, or schedule async revalidation.
    // Returns immediately with current page if valid, stale page, or nullptr.
    // Never waits for revalidation (falls back to stale data).
    std::shared_ptr<CapturePageData> getCapture(uint32_t aspectsMask);

    // Convenience: get playback capture (Playback | Metadata aspects).
    // Used by audio thread: never blocks, stale data acceptable.
    std::shared_ptr<CapturePageData> getPlaybackCapture() {
        return getCapture(Playback | Metadata);
    }

    // Convenience: get preview capture (Preview aspect only).
    // Used by UI painting: never blocks, stale data shown briefly then refreshed.
    std::shared_ptr<CapturePageData> getPreviewCapture() {
        return getCapture(Preview);
    }

    // Check if revalidation is needed for specific aspects.
    // Used by revalidator to decide whether to process a job.
    bool needsRevalidation(uint32_t aspectsMask) const;

    // Query which aspects are currently valid (for diagnostics/optimization).
    bool isAspectValid(uint32_t aspect) const {
        return (validAspects_ & aspect) == aspect;
    }

    // Ensure peak cache is built (for waveform display).
    // TODO: Phase 4 - integrate with capture page pool model.
    bool ensureCapturePeaks();

    // IRevalidatable: build the offline capture on the revalidator worker so
    // container-backed (and grained) cuts have preview data without waiting
    // for the first playback to construct the reader chain. Returns false
    // while the capture cannot be built yet (Preview stays stale and retries).
    bool revalPrepPreview() override;

    // Old compatibility method (deprecated, will be removed).
    // TODO: Task 8 - replace all call sites with getPreviewCapture() or getPlaybackCapture().
    twRandomSource *ensureCapture() { return nullptr; }  // Stub for compilation

private slots:

protected:
    virtual int serializeSelfAttributes( QTextStream &o );

private:
    // Check if revalidation is needed for specific aspects. _nolock: caller must hold mutex().
    // Internal version used by public needsRevalidation() when lock is acquired.
    bool needsRevalidation_nolock(uint32_t aspectsMask) const;

    // Lazily acquire our own independent read cursor over the content's sample
    // data, so two cuts of one source never share a play position (proposal 07).
    // When grain params are non-identity, an owned twGrainSource is interposed
    // (proposal 06). Stays a passthrough/fallback when the content is not a
    // random-access source.
    void ensureReader();
    // Rebuild the playback chain with given snapshot parameters. Called from UI
    // thread setters; accepts snapshot to avoid reading unlocked members
    // (multithreading policy: Phase 1 Option B).
    void rebuildReader( const SCutSnapshot &snap );
    // Build the capture (container render) if needed. Internal method.
    // Callable from the UI thread (rebuildReader) and the revalidator worker
    // (revalPrepPreview); captureBuildMutex_ serializes concurrent builders.
    // TODO: Phase 4 future - integrate with capture page pool revalidation.
    void buildCapture_();
    mutable std::mutex captureBuildMutex_;  // Serializes buildCapture_() across threads

    // Helper methods for revalidator integration (Phase 4)
    // _nolock suffix indicates caller MUST hold mutex() before calling.
    // These are friends-only methods, non-locking to avoid recursive lock deadlock.
    friend class CaptureRevalidator;

    // Return current capture page without locking.
    // Uses std::atomic_load for thread-safe read (C++17).
    // May return stale data if revalidator is modifying the page, which is acceptable.
    std::shared_ptr<CapturePageData> currentPage() const {
        return std::atomic_load(&currentPage_);
    }

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

    // Queue of pending window parameter changes (populated during drag,
    // processed after drag completes). Allows drag operations to queue changes
    // without calling expensive invalidateCapture() on the drag event path.
    // Protected by the same mutex() as all other SCut state (one mutex per object).
    std::vector<SCutWindowParamEvent> windowParamEventQueue_;

    SLink *content_;

    // Window parameters: accessed by both UI thread (modifications) and audio thread (reading).
    // Use inherited mutex() from SObject to synchronize access.
    // Domain-typed (proposal 18 Phases 1+3): the slip anchor is stored in
    // the SOURCE domain as an exact rational (integral for freshly edited
    // clips; legacy loads may carry the old rounding as a fraction). The
    // loop segment stays a warped/timeline length (musical quantity), the
    // duration a clip-relative length.
    Fraction  srcStart_;
    WarpedPos loopStart_;
    WarpedLen loopLength_;
    ClipLen   cutDuration_;
    twGrainParams grainParams_;

    SCutRendererInline *inlineRenderer_;
    bool readerTried_;

    // DOUBLE-BUFFER READER STATE (Unix page cache model)
    // currentReader_: always valid & complete, read by audio thread
    // nextReader_: being built by UI thread, swapped in atomically when ready
    // oldReader_: previous currentReader_, freed after swap
    mutable std::mutex readerSwapLock_;  // Protects swap operation
    SCutReaderState currentReader_{nullptr, nullptr, nullptr, false, 0};
    SCutReaderState nextReader_{nullptr, nullptr, nullptr, false, 0};
    SCutReaderState oldReader_{nullptr, nullptr, nullptr, false, 0};  // For deferred deletion
    // Descriptor of the reader chain currently built, so rebuildReader() can skip
    // re-acquiring an identical chain. A non-looping sample cut's reader does not
    // depend on duration or slip offset, so a plain trim/extend (which retriggers
    // rebuildReader on every drag tick) need not mint a fresh reader each time.
    bool          builtNeedGrain_ = false;
    bool          builtNeedLoop_  = false;
    twGrainParams builtGrain_;
    WarpedPos     builtLoopStart_;
    WarpedLen     builtLoopLength_;

    // Cached render of a container content (proposal 07 step 5).
    // Used temporarily until fully integrated with capture page pool.
    // TODO: Phase 4 future - replace entirely with two-page buffer model.
    std::shared_ptr<twCapturingSource> capture_;
    bool captureConnected_ = false;   // connected to arrangementChanged once
    // Peak cache over the capture, for the waveform preview.
    // TODO: Phase 4 future - replace entirely with two-page buffer model.
    preview_t *capPeaks_ = nullptr;
    offset_t   capPeakSkip_ = 0;
    offset_t   capPeakN_ = 0;

    // Two-page capture buffer model (Phase 4, async revalidation).
    // Gradually replaces capture_ and capPeaks_ implementation with pool-based model.
    // currentPage_ and nextPage_ inherited from SObject base class (no shadowing).
    // When readers release shared_ptr, old page freed (no stale pointers, no use-after-free)
    CaptureRevalidator *revalidator_;  // Borrowed from SProject, not owned

    // Aspect tracking: bitmask of valid aspects in currentPage_.
    // Updated by revalidator when pages are swapped and marked complete.
    uint32_t validAspects_ = 0;

    // Last good snapshot: returned when lock acquisition fails to prevent returning
    // uninitialized data. Updated whenever getSnapshot() successfully acquires the lock.
    mutable SCutSnapshot lastGoodSnapshot_{};
};

#endif
