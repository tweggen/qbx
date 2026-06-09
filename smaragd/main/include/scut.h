
#ifndef _SCUT_H_
#define _SCUT_H_

#include <qobject.h>
#include <mutex>
#include <atomic>
#include "sobject.h"
#include "slink.h"
#include "twgrainparams.h"

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
struct SCutReaderState {
    twSampleReader *reader;      // Always valid: non-null or fallback to content's root
    twGrainSource *grain;        // Optional grain processor
    bool looping;                // True iff reader is a twLoopReader
    int generation;              // Incremented on each swap
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
struct SCutSnapshot {
    offset_t startOffset;        // Source sample position where clip window begins
    length_t loopLength;         // Loop segment length; 0 = no loop
    length_t cutDuration;        // Timeline duration of clip (includes stretching)
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

    virtual twComponent &getRootComponent();
    virtual QWidget *getDetailEditWidget( QWidget *parent );
    virtual QWidget *getInlineEditWidget( QWidget *parent );
    virtual SObjectRenderer *getInlineRenderer();    

    virtual int readPostChildrenAttributes( QDomElement &element );
    
    virtual int seekTo( offset_t );
    SObject &getContent() const { return content_->getSObject(); }
    offset_t getLoopStart() const;
    offset_t getStartOffset() const { return startOffset_; }
    // Length of the segment that repeats when this cut is longer than it (the
    // "previously visible cut" captured by the right-upper edge loop gesture).
    // 0 means no loop; the loop is active iff 0 < loopLength_ < cutDuration_.
    length_t getLoopLength() const { return loopLength_; }
    bool isLooping() const { return loopLength_ > 0 && loopLength_ < cutDuration_; }
    // Set the loop length / stretch / full grain params for drawing only, without
    // rebuilding the audio chain or the preserve-span rescale (used for live drag
    // feedback and for cloning; the chain is rebuilt once afterwards, e.g. by
    // setWindow on release).
    void setLoopLengthRaw( length_t l ) { loopLength_ = l; }
    void setStretchRaw( double s ) { grainParams_.stretch = s; }
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
    void setWindow( offset_t startOffset, length_t duration,
                    length_t loopLength, double stretch );

    // Grain time-stretch / pitch-shift parameters for this clip (proposal 06).
    double getStretch() const { return grainParams_.stretch; }
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
    void invalidateCapture();

    // Ensure the capture is built (used for synchronous rebuild during drags).
    // Returns the capture if one was built, NULL if content is not capturable.
    twRandomSource *ensureCapture();
    // Ensure the peak cache of the capture is built (for waveform display).
    bool ensureCapturePeaks();

private slots:

protected:
    virtual int serializeSelfAttributes( QTextStream &o );

private:
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

    // Queue of pending window parameter changes (populated during drag,
    // processed after drag completes). Allows drag operations to queue changes
    // without calling expensive invalidateCapture() on the drag event path.
    std::vector<SCutWindowParamEvent> windowParamEventQueue_;
    std::mutex queueMutex_;  // Protects the event queue (minimal contention)

    SLink *content_;

    // Window parameters: accessed by both UI thread (modifications) and audio thread (reading).
    // Use windowMutex_ to synchronize access when modifying window/grain params.
    mutable std::mutex windowMutex_;
    offset_t startOffset_;
    offset_t loopStart_;
    length_t loopLength_;
    length_t cutDuration_;
    twGrainParams grainParams_;

    SCutRendererInline *inlineRenderer_;
    bool readerTried_;

    // DOUBLE-BUFFER READER STATE (Unix page cache model)
    // currentReader_: always valid & complete, read by audio thread
    // nextReader_: being built by UI thread, swapped in atomically when ready
    // oldReader_: previous currentReader_, freed after swap
    mutable std::mutex readerSwapLock_;  // Protects swap operation
    SCutReaderState currentReader_{nullptr, nullptr, false, 0};
    SCutReaderState nextReader_{nullptr, nullptr, false, 0};
    SCutReaderState oldReader_{nullptr, nullptr, false, 0};  // For deferred deletion
    // Cached render of a container content (proposal 07 step 5). Built lazily by
    // ensureCapture(); invalidated on any edit via invalidateCapture().
    twCapturingSource *capture_ = nullptr;
    bool captureConnected_ = false;   // connected to arrangementChanged once
    // Peak cache over the capture, for the waveform preview. Built lazily from
    // capture_; freed with it in invalidateCapture()/dtor.
    preview_t *capPeaks_ = nullptr;
    offset_t   capPeakSkip_ = 0;   // capture frames per peak
    offset_t   capPeakN_ = 0;      // number of peaks
};

#endif
