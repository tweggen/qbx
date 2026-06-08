
#ifndef _SCUT_H_
#define _SCUT_H_

#include <qobject.h>
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
 * Thread affinity: MIXED (not thread-safe)
 * - content_: accessed from UI thread (getDetailEditWidget, rendering) AND audio thread (getRootComponent→calcOutputTo)
 * - startOffset_, loopStart_, cutDuration_: read from both UI and audio threads
 *
 * RACE CONDITION: When content_ points to a SPlainWave, the underlying file handle
 * (twWavInput::file_) is accessed from both threads without synchronization.
 *
 * Execution paths:
 *   UI:    SMVActualView::paintEvent() → draw(SLink) → SPlainWaveRendererInline::draw()
 *   Audio: CoreAudio callback → rendering → getRootComponent()->calcOutputTo()
 */
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

public slots:
    virtual void setLoopStart( offset_t );
    virtual void setLoopLength( length_t );
    virtual void setStartOffset( offset_t );
    virtual void setDuration( length_t );
    void setStretch( double );
    void setPitchCents( double );

private slots:
    // Drop the cached render of a container content (and any reader built over
    // it); the next pull re-captures. Connected to SProject::arrangementChanged
    // so a cut over a group/mixer transparently reflects edits (proposal 05
    // feature (b) / 07 step 5). Only container-backed cuts ever connect this.
    void invalidateCapture();

protected:
    virtual int serializeSelfAttributes( QTextStream &o );

private:
    // Lazily acquire our own independent read cursor over the content's sample
    // data, so two cuts of one source never share a play position (proposal 07).
    // When grain params are non-identity, an owned twGrainSource is interposed
    // (proposal 06). Stays a passthrough/fallback when the content is not a
    // random-access source.
    void ensureReader();
    void rebuildReader();
    // When the content is not a random-access source but is a seekable container
    // (a track/mixer sub-arrangement), render its output ONCE into an owned
    // twCapturingSource so placements read a cheap, independent snapshot instead
    // of re-pulling the live graph (proposal 07 step 5). Returns NULL when the
    // content is a real source (sample path) or cannot be captured.
    twRandomSource *ensureCapture();
    // Build (once) a peak-cache of the capture for waveform preview, in the
    // container frame domain. Dropped together with the capture on edit.
    bool ensureCapturePeaks();

    SLink *content_;
    offset_t startOffset_;
    offset_t loopStart_;
    length_t loopLength_;
    length_t cutDuration_;
    bool     looping_;   // true while reader_ is a twLoopReader (cursor base-aware)
    SCutRendererInline *inlineRenderer_;
    twSampleReader *reader_;
    twGrainSource *grain_;
    twGrainParams grainParams_;
    bool readerTried_;
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
