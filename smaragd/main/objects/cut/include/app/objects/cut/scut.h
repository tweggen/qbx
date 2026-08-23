
#ifndef _SCUT_H_
#define _SCUT_H_

#include <qobject.h>
#include <mutex>
#include <atomic>
#include <memory>
#include <chrono>
#include "app/model/sobject.h"
#include "app/model/slink.h"
#include "app/model/sclipwindow.h"
#include "tw/sources/twgrainparams.h"
#include "tw/core/twfraction.h"
#include "tw/core/twdomains.h"
#include "tw/core/twtimemap.h"
#include "tw/pages/capture_page_pool.h"
#include "tw/events/tweventclipset.h"

class SProject;
class QWidget;
class QTimer;
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
    : public SObject,
      public SClipWindow    // the WINDOW layer, for the type-agnostic verbs
{
    Q_OBJECT
    Q_PROPERTY( double Stretch READ getStretch WRITE setStretch )
    Q_PROPERTY( double PitchCents READ getPitchCents WRITE setPitchCents )
public:
    // The cut always creates its OWN content link (+1 ref on the content).
    // There is deliberately no SLink-adopting overload: adopting a caller's
    // link inverted ownership (the caller kept a pointer it no longer owned
    // and typically deleted it, dangling content_ — the split/ensureSCut
    // wrap-path use-after-free). Callers holding a temporary link pass
    // link->getSObject() and delete their link AFTER the cut exists, so the
    // content's refcount never dips to zero in between.
    SCut( SProject *parentProject, SObject &content );
    virtual ~SCut();

    static SLink *instantiateFromDomElement( SProjectLoader &projectLoader, 
					     QDomElement &element, 
					     SObject *parent );

    // Immutable snapshot of playback parameters for lock-free audio-thread reads.
    // Reader state is double-buffered (Unix page cache model): currentReader_ is
    // always complete & valid. Safe to call from audio thread.
    SCutSnapshot getSnapshot() const;

    // Proposal 27 M1 readiness protocol: gate/ungate this clip's audio render.
    // While gated the clip's reader freezes SILENT pages (twComponent
    // readiness gate); flipping in EITHER direction invalidates the reader's
    // source-position-keyed pages (the track-level walk does not reach them)
    // plus every timeline range this clip covers, so playback/render converge
    // without restart — order-independent, epoch-driven, no latch. The flag
    // survives reader rebuilds (readers are minted lazily). M1 callers: the
    // set-render-gate test verb; M5 wires real spectral-aspect readiness here.
    void setRenderGateReady( bool ready );

    // Proposal 19 Phase 2b: blocking snapshot for the freeze/resolution path —
    // always returns the CURRENT reader (never the try-lock lastGoodSnapshot_
    // fallback), so a freeze that observes the post-edit epoch also observes the
    // post-edit reader. NOT for the RT audio thread (use getSnapshot() there).
    SCutSnapshot getSnapshotBlocking() const;

    virtual std::shared_ptr<twComponent> getRootComponent() override;
    virtual QWidget *getDetailEditWidget( QWidget *parent ) override;
    virtual QWidget *getInlineEditWidget( QWidget *parent ) override;
    virtual SObjectRenderer *getInlineRenderer() override;    

    virtual int readPostChildrenAttributes( QDomElement &element ) override;
    
    virtual int seekTo( offset_t ) override;
    // Fold the slip offset (grain-stretched when a grain stage is active) into
    // a clip-relative position, so tracks can seek/freeze our reader directly.
    // A looping reader is already cut-relative and maps identically.
    virtual offset_t mapTimelineToComponentPos( offset_t off ) override;
    // Inv-1: resolve component + mapped position from ONE snapshot (never two
    // straddling getSnapshotBlocking() reads that a lazy reader build can split).
    virtual twResolvedClip resolveClip( offset_t off ) override;
    /// Proposal 21 L3b: a window over a live recording IS a live recording,
    /// so the track's routing predicate holds for the cut the arranger
    /// actually owns and not only for the content behind it.
    bool isLiveRecording() const override
    { return content_ && content_->getSObject().isLiveRecording(); }

    /// Proposal 41 D7/M4, same shape as isLiveRecording() above: a window
    /// over a pure-event fragment IS pure-event content, so buildCapture_ /
    /// ensureReader / invalidateAspects / getPreview see the short-circuit
    /// through the CUT they actually own, not only through the fragment.
    bool isPureEventContent() const override
    { return content_ && content_->getSObject().isPureEventContent(); }

    /// Proposal 41 D4/M3: THE residual event feed reaching a track through
    /// this placement — our window's slip/loop map applied over whatever the
    /// content underneath reports through its OWN resolveEventFeed(). Never
    /// names SLaneFragment (objects/cut may not depend on objects/fragment,
    /// CONTRACT.md / check_layering.py — the whole point of the base-class
    /// virtual is that this works for ANY content, known or not). Refuses a
    /// non-unity rate (D5) rather than double-converting it: the tick/frame
    /// conversion already happened exactly once, inside the content's own
    /// window(s); a second, frame-domain stretch here would stop the part
    /// following tempo.
    twEventClipResolved resolveEventFeed( offset_t clipPos ) override;

    SObject &getContent() const { return content_->getSObject(); }
    WarpedPos getLoopStart() const;
    // The clip's slip anchor. AUTHORITATIVE storage is the SOURCE-domain
    // anchor srcStart_ (proposal 18 Phase 3): it points into concrete
    // material and is invariant under stretch edits, so re-stretching can
    // never drift it. The warped-domain offset is DERIVED exactly and
    // floored once here — the render-boundary rounding rule.
    const Fraction &getSrcStart() const { return srcStart_; }

    // Proposal 28 W1: THE source<->warped conversion authority for this clip.
    // No anchors -> pure scalar stretch (bit-identical to the historic
    // expressions); anchors -> the user's piecewise warp map. Built on
    // demand from grainParams_ so no cached copy can go stale.
    twWarpMap warpMap() const {
        return twWarpMap( grainParams_.warpAnchors,
                          grainParams_.stretch > Fraction(0)
                              ? grainParams_.stretch : Fraction(1) );
    }
    Fraction sourceToWarpedExact( const Fraction &src ) const {
        return warpMap().srcToWarped( src );
    }
    Fraction warpedToSourceExact( const Fraction &warped ) const {
        return warpMap().warpedToSrc( warped );
    }

    WarpedPos getStartOffset() const {
        return WarpedPos( warpMap().srcToWarped( srcStart_ ).floorToInt() );
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
        srcStart_ = warpMap().warpedToSrc( Fraction( o.frames() ) );
    }
    void setDurationRaw( ClipLen d ) { cutDuration_ = d; }
    void setGrainParamsRaw( const twGrainParams &p ) { grainParams_ = p; }
    virtual bool hasDuration() const override { return true; }
    virtual length_t getDuration() const override;
    // Proposal 19 Phase 2b: blocking-snapshot duration for the EDIT/signal path.
    // getDuration() uses getSnapshot()'s try-lock → stale lastGoodSnapshot_
    // fallback (correct for the RT audio thread), which under background-worker
    // mutex contention can return a pre-edit duration; propagated through a
    // durationChanged emit that becomes twTrackMix::updateClip, that stale value
    // leaves clip windows wrong (the takes_group_broadcast flake). Edit-path
    // emitters must read the CURRENT duration. Not for the RT audio thread.
    length_t getDurationBlocking() const override;

    // Range-scoped invalidation (proposal 18 Phase 5): dirty ranges of our
    // CONTENT arrive in the SOURCE domain; map them through the cut window
    // (stretch, slip, loop tiling) into clip-relative ranges. An edit
    // outside the audible window maps to NOTHING.
    QList<SDirtyRange> mapChildRangesToSelf(
        SLink *childLink, const QList<SDirtyRange> &childRanges ) override;

    // The FORWARD twin of mapChildRangesToSelf, for a single position
    // (proposal 09 §15): a clip-relative position -> the position in our
    // CONTENT. Same three stages, applied in the other order — loop fold in
    // the warped domain, then the exact warp inverse — so the two can never
    // disagree about where the window maps.
    //
    // NON-BLOCKING by contract (SObject::windowStep): a repaint asks it. It
    // takes the try-lock snapshot and NEVER calls ensureReader(), which for a
    // container-backed asset would build a capture — a full render — on the UI
    // thread.
    bool windowStep( offset_t clipRel, SWindowStep &out ) const override;

    // Waveform preview for a container-backed asset cut: peaks come from the
    // capture (the rendered snapshot shared with audio), in the container's frame
    // domain, so `start` is the container offset the cut window maps to. Sample
    // cuts have no capture here and fall back to the base preview. Tier 2 of the
    // asset-preview work.
    virtual int getPreview( preview_t *dest, offset_t start, length_t length,
                            offset_t nProbes ) override;

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

    // W1: replace the user warp-anchor list (sanitized; empty clears).
    // Undoable via SResizeClipAction's warpAnchors attribute.
    void setWarpAnchors( const std::vector<twWarpAnchor> &anchors );

    // Shared time maps (proposal 18 Phase 4). BOTH the audio path and the
    // waveform preview consume these - there is no second implementation
    // of the clip's position mapping to disagree with.
    //
    // clipToSourceMap: clip-relative timeline position -> SOURCE sample,
    // source = srcStart + rel/stretch, exact. (For a looping clip this maps
    // a position WITHIN one repetition - the tiling itself is the reader's
    // twLoopMap.)
    // SCALAR-ONLY affine form — valid iff no warp anchors exist (asserting
    // callers migrated to clipToSource(); kept for the no-anchor fast path).
    twAffineMap clipToSourceMap() const {
        Fraction st = grainParams_.stretch > Fraction(0) ? grainParams_.stretch
                                                         : Fraction(1);
        return twAffineMap( Fraction(1) / st, srcStart_ );
    }
    // Map-aware clip->source (W1): `rel` is the clip-relative warped-domain
    // distance from the window start; result is the SOURCE position. Equals
    // clipToSourceMap().map(rel) exactly when no anchors exist.
    Fraction clipToSource( const Fraction &rel ) const {
        const twWarpMap wm = warpMap();
        return wm.warpedToSrc( wm.srcToWarped( srcStart_ ) + rel );
    }
    // clipToReaderMap: clip-relative position -> the reader's own domain
    // (what SCut::resolveClip folds via twView). A looping reader is cut-relative
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
    // W4: opt-in formant preservation for the vocoder pitch stage.
    bool getPreserveFormants() const { return grainParams_.preserveFormants; }
    void setPreserveFormants( bool on );
    // Formant shift, in cents, independent of pitch/rate: a spectral-envelope
    // warp the vocoder backend applies regardless of the pitch stage.
    // Default 0 = no-op. See twGrainParams::formantShiftCents.
    double getFormantShiftCents() const { return grainParams_.formantShiftCents; }
    void setFormantShiftCents( double cents );
    const twGrainParams &getGrainParams() const { return grainParams_; }
    void setGrainParams( const twGrainParams & );

    // --- SClipWindow (proposal 37 D8b) -----------------------------------
    //
    // The window arithmetic every windowed verb needs, expressed without
    // naming SCut. Each one FORWARDS to the member above it: this is a
    // vocabulary, not a second implementation, so there is nothing here that
    // can disagree with the cut's own behaviour. Frames in, frames out
    // (SClipWindow rule 1); the two exact forms carry the SOURCE-domain
    // anchor, which is authoritative (invariant 4).
    SObject &asObject() override { return *this; }
    SObject &windowContent() const override { return getContent(); }

    // --- the take-COLUMN seam, FORWARDED ONE LEVEL (proposal 42 M2) --------
    //
    // A window over a take column IS a placement of that column, so it answers
    // the column questions on the column's behalf. Without this the seam was
    // NOT TOTAL: `SObject`'s base returns 0 / null / nothing, so every consumer
    // that reached a take through it silently got the WRAPPER instead — the
    // MIDI event verbs (rejected outright, because the wrapper's `sequence()`
    // is null over a stack), the event editor and virtual keyboard (bound an
    // empty ref), the clip-properties panel (no take navigation), the
    // automation `cut:` owner (a per-take envelope landed on the wrapper) and
    // four testkit asserts (`take=` silently ignored).
    //
    // THE SEAM MEANS "TAKE k OF THIS PLACEMENT'S COLUMN". It does NOT mean
    // "the window whose parameters this placement carries" — that question is
    // `SClipWindow::parametersOf()`, and on a wrapped column the two give
    // different answers on purpose.
    SClipWindow *windowTakeAt( int index ) const override
    { return windowContent().windowTakeAt( index ); }
    int windowTakeCount() const override
    { return windowContent().windowTakeCount(); }
    int activeWindowTakeIndex() const override
    { return windowContent().activeWindowTakeIndex(); }
    SLink *insertWindowTake( SClipWindow &window, int atIndex ) override
    { return windowContent().insertWindowTake( window, atIndex ); }
    void removeWindowTake( int index ) override
    { windowContent().removeWindowTake( index ); }
    void setActiveWindowTake( int index ) override
    { windowContent().setActiveWindowTake( index ); }

    length_t duration() const override { return getDuration(); }
    length_t durationBlocking() const override { return getDurationBlocking(); }
    length_t loopLength() const override { return getLoopLength().frames(); }
    offset_t startOffset() const override { return getStartOffset().frames(); }
    Fraction stretchOrRate() const override { return getStretchExact(); }
    Fraction contentAnchorExact() const override { return getSrcStart(); }
    Fraction timelineToSourceExact( const Fraction &relTimeline ) const override
    { return clipToSource( relTimeline ); }

    void setDurationFromTimeline( length_t d ) override { setDuration( d ); }
    void setStartOffsetFromTimeline( offset_t off ) override
    { setStartOffset( off ); }
    void setWindowFromTimeline( offset_t startOffset, length_t duration,
                                length_t loopLength,
                                const Fraction &stretchOrRate ) override
    {
        // The offset is read in the timeline domain the window will have AFTER
        // this edit, i.e. through the NEW stretch — the same reading
        // SResizeClipAction::readXml gives a legacy warped `startOffset`
        // attribute. Converted here, exactly once, never by the caller.
        const twWarpMap wm( grainParams_.warpAnchors,
                            stretchOrRate > Fraction( 0 ) ? stretchOrRate
                                                          : Fraction( 1 ) );
        setWindow( wm.warpedToSrc( Fraction( (int64_t) startOffset ) ),
                   ClipLen( duration ), WarpedLen( loopLength ), stretchOrRate );
    }
    void setWindowExact( const Fraction &contentAnchor, length_t duration,
                         length_t loopLength,
                         const Fraction &stretchOrRate ) override
    {
        setWindow( contentAnchor, ClipLen( duration ), WarpedLen( loopLength ),
                   stretchOrRate );
    }
    void setContentAnchorExact( const Fraction &contentAnchor ) override
    { setSrcStartRaw( contentAnchor ); }

    SClipWindow *cloneWindowOverContent( SProject *project,
                                         SObject &content ) const override;

    // Transposition limit, in cents. ±2 octaves; setPitchCents() and the
    // set-pitch action both clamp to it, so no entry point can store a value
    // the others would refuse.
    static constexpr double PITCH_CENTS_LIMIT = 2400.0;
    static double clampPitchCents( double cents ) {
        if( cents >  PITCH_CENTS_LIMIT ) return  PITCH_CENTS_LIMIT;
        if( cents < -PITCH_CENTS_LIMIT ) return -PITCH_CENTS_LIMIT;
        return cents;
    }

    // Formant-shift limit, in cents: +-12 semitones. A narrower range than
    // pitch's +-2 octaves — the cepstral envelope estimate the vocoder backend
    // uses is a coloration filter, not a formant synthesizer, and stops being
    // musically useful well before pitch does. setFormantShiftCents() and the
    // set-formant-shift action both clamp to it.
    static constexpr double FORMANT_SHIFT_CENTS_LIMIT = 1200.0;
    static double clampFormantShiftCents( double cents ) {
        if( cents >  FORMANT_SHIFT_CENTS_LIMIT ) return  FORMANT_SHIFT_CENTS_LIMIT;
        if( cents < -FORMANT_SHIFT_CENTS_LIMIT ) return -FORMANT_SHIFT_CENTS_LIMIT;
        return cents;
    }

signals:
    // Emitted when window parameters change (slip, stretch, loop, trim, extend)
    // during a drag. Parent containers use this to invalidate their renders.
    void windowParamsChanged();

public slots:
    virtual void setLoopStart( offset_t );
    virtual void setLoopLength( length_t );
    virtual void setStartOffset( offset_t );
    virtual void setDuration( length_t ) override;
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
    // arrangementChanged gate: only a cut that has ever materialized a capture
    // (container-backed or grained) re-captures on an applied action. Plain
    // sample cuts never did — dropping their pages + rescheduling three
    // aspects for EVERY cut on EVERY action was an invalidation storm that
    // stalled renders (the workers=8 takes_group_broadcast hang).
    void onArrangementChanged();

    // The per-clip static volume (SObject::volume_, dB — see
    // ssetclipvolumeaction.h). Connected to our OWN volumeChanged in the ctor,
    // the same self-signal idiom STrack uses for its fader
    // (onTrackVolumeChanged). Unlike the track fader there is no twGainStage
    // here to push a scalar into directly: the value is picked up by
    // STrack::refreshClipGainCurves() the next time it runs, which this
    // invalidation is what triggers.
    void onVolumeChanged( double db );

    // Trailing edge of the slip-invalidation throttle (see
    // invalidateRenderPathForSlip). Both run on THIS object's thread: the
    // timer is a child QObject, and armSlipInvalidateTimer_ is only ever
    // reached through a QMetaObject::invokeMethod with AutoConnection.
    void armSlipInvalidateTimer_();
    void flushSlipInvalidation_();

protected:
    virtual int serializeSelfAttributes( QTextStream &o ) override;

private:
    // W9: a slip-only edit (startOffset / srcStart / loopStart) changes which
    // source material this clip maps to WITHOUT touching its timeline extent,
    // so nothing else on the render path notices — resolveClip folds the new
    // slip on the very next freeze while the already-frozen track / chain /
    // mixer pages keep the pre-slip audio at the same position (mixed
    // generations). Every slip setter therefore stales this clip's whole
    // timeline range, exactly like setRenderGateReady does.
    //
    // Reader pages are NOT bumped, and must not be: they are keyed in the
    // reader's own SOURCE domain (POSITION_DOMAINS rule 4), so a slipped clip
    // simply asks for different source pages — the ones it already has stay
    // correct.
    //
    // THROTTLED, because the live slip drag calls setStartOffset once per
    // mouse-move and each call walks the project tree from the root: the first
    // call after a quiet period invalidates IMMEDIATELY (so a single
    // programmatic edit — qxa, undo, load — is unthrottled and deterministic),
    // and a call inside the window arms the single-shot timer instead, which
    // guarantees the FINAL slip position of a drag always lands.
    void invalidateRenderPathForSlip( length_t duration );

    static constexpr int SLIP_INVALIDATE_MS = 333;   // ~3 invalidations / second

    // NOT the object mutex(): the throttle is consulted AFTER the edit's
    // critical section has been released, and taking mutex() again there
    // would serialize a drag tick against the audio thread's snapshot reads
    // for no reason.
    mutable std::mutex slipThrottleMutex_;           // guards the three below
    std::chrono::steady_clock::time_point lastSlipInvalidate_{};
    bool slipInvalidateEver_ = false;
    bool slipInvalidatePending_ = false;
    // Thread-confined to this object's thread (see the slots above), not
    // covered by slipThrottleMutex_.
    QTimer *slipInvalidateTimer_ = nullptr;          // child QObject, lazily built

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
    // Assemble a consistent snapshot from live fields; caller MUST hold mutex().
    // Shared by getSnapshot() (try-lock) and getSnapshotBlocking().
    SCutSnapshot buildSnapshot_nolock() const;
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
    // Atomic: ensureReader() reads it outside mutex() (Phase 2b).
    std::atomic<bool> readerTried_;

    // The CONTENT EPOCH the current capture was built from, and 0 when there is
    // no capture or the content is not container-backed (proposal 09 M0, second
    // half).
    //
    // WHY A STAMP AND NOT AN ORDERING. A container capture is rebuilt on a
    // revalidator worker while the edit that invalidated it bumps the content
    // epoch on the main thread, and the two RACE: measured, a rebuild that
    // starts a few hundred microseconds before the bump lands reads the
    // PRE-EDIT pages, publishes them, and sets readerTried_ -- after which
    // ensureReader() returns immediately and the stale snapshot is what the
    // next render hears. It then sticks until some LATER edit invalidates
    // again. Failure rate measured at 3-in-6 on an idle box, at
    // SMARAGD_REVAL_WORKERS=1 as well as 8, so it is not the worker pool.
    //
    // Stamping makes the staleness self-healing under ANY ordering rather than
    // forcing one: a capture whose stamp no longer matches its content is a
    // MISS and is rebuilt on the next access, exactly as a frozen page whose
    // width no longer matches its producer is a miss (proposal 36 4.5). The
    // stamp is taken BEFORE the freeze loop, so a content change DURING a build
    // leaves the older value and is correctly detected as stale.
    std::atomic<uint64_t> captureContentEpoch_{ 0 };

    // How many times in a row we have rebuilt this capture WITHOUT the stamp
    // converging, and the bound on it.
    //
    // THIS BOUND IS LOAD-BEARING AND ITS ABSENCE HUNG THE APP. The check above
    // assumes a rebuild eventually produces a capture whose stamp matches the
    // content's current epoch. For a CONTAINER capture that assumption is
    // false: buildCapture_ freezes the container's pages, and doing so can
    // perturb the very epoch it stamps against -- so every rebuild came out
    // stale, ensureReader() invalidated again, and each invalidateCapture()
    // scheduled a Preview/Playback revalidation whose completion repainted the
    // arranger, which called ensureReader() again. Measured in the field as an
    // uninterrupted stream of "[PREVIEW] recompute" for one object with the UI
    // thread stuck in SCutRendererInline::draw.
    //
    // Past the bound we ACCEPT the capture we have and say so once. That is a
    // strictly better failure than spinning, and it is not a new one: it is the
    // pre-stamp behaviour (a capture that heals on the next real edit), which
    // is the same bargain proposal 16 already makes for playback when it serves
    // a stale page rather than stalling.
    static const int kMaxCaptureRebuildAttempts = 2;
    std::atomic<int> captureRebuildAttempts_{ 0 };
    std::atomic<bool> captureStaleReported_{ false };

    // The content's current render-chain epoch, or 0 when the content has no
    // root component (a leaf sample). 0 == 0 makes the check inert for
    // sample-backed and grained captures, which are invalidated explicitly by
    // their own window/grain edits and have no container epoch to track.
    uint64_t contentEpochForCapture_() const;

    // Proposal 27 M1: desired readiness of this clip's reader (see
    // setRenderGateReady()); applied to every newly built reader.
    std::atomic<bool> renderGateReady_{true};

    // DOUBLE-BUFFER READER STATE (Unix page cache model)
    // currentReader_: always valid & complete, read by audio thread
    // nextReader_: being built by UI thread, swapped in atomically when ready
    // oldReader_: previous currentReader_, freed after swap
    // Proposal 19 Phase 2b: currentReader_ is now published and read under the
    // object mutex() (getSnapshot/getSnapshotBlocking/rebuildReader swap), so a
    // separate readerSwapLock_ is gone — it guarded only the write side, never
    // the readers, so the swap was never atomic against a concurrent freeze.
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

public:
    // Atomically read capture_ into an OWNED copy. capture_ is published by
    // buildCapture_() (a revalidator worker OR the render thread) and reset by
    // invalidateCapture() (UI/arrangement thread), so reading the member
    // directly from another thread is a shared_ptr data race — and the owned
    // copy is also what keeps the source alive for the duration of the caller's
    // use, whatever the interleaving. rebuildReader() already does this inline;
    // every other cross-thread reader goes through here.
    //
    // Note captureBuildMutex_ does NOT serialize against this: invalidateCapture()
    // resets under mutex(), so only mutex() excludes a concurrent reset.
    std::shared_ptr<twCapturingSource> captureSnapshot() const {
        std::lock_guard<std::mutex> lock( mutex() );
        return capture_;
    }

private:
    // CAPTURE GENERATION — the guard that makes invalidateCapture() WIN a race
    // it used to lose. buildCapture_ can take tens of milliseconds and runs on
    // a revalidator worker holding only captureBuildMutex_, which deliberately
    // does NOT exclude invalidateCapture() (that resets under mutex()). So an
    // invalidation landing mid-build was simply overwritten by the build's own
    // publish a moment later, and the stale capture then read as fresh.
    //
    // MEASURED, on the wrapped take column: a worker entered buildCapture_ and
    // resolved the stack at take 0; `select-take` set take 1 and invalidated in
    // the same millisecond; the worker published its take-0 capture 2 ms later;
    // the next render served it. The click "worked" and the audio did not move.
    //
    // buildCapture_ samples this ONCE before it reads any content and publishes
    // only if it has not moved. A discarded build costs one rebuild, which the
    // very next ensureReader() does anyway (capture_ is null and readerTried_
    // is false); publishing it costs correctness.
    std::atomic<uint64_t> captureGen_{ 0 };

    // Set (never cleared) when buildCapture_ publishes a capture; gates
    // onArrangementChanged(). Atomic: written on the reval worker.
    std::atomic<bool> everHadCapture_{false};
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
