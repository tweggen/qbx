#ifndef _SRECORDINGCONTENT_H_
#define _SRECORDINGCONTENT_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "app/model/sobject.h"

class SRecordingRendererInline;
class twGrowingCaptureSource;

/**
 * THE GROWING RECORDING CONTENT (proposal 21 L3b, design D7).
 *
 * An `SObject` whose duration GROWS while a capture is running. The recording
 * CUT is an ordinary `SCut` over one of these, so every window operation, every
 * renderer and the whole arranger geometry work on it unchanged; what is new is
 * only that `getDuration()` answers a different number every tenth of a second
 * and that the waveform stops at a FRONTIER the renderer draws.
 *
 * It is a VIEW of a `twGrowingCaptureSource` (L3a) — a window `[startFrame,
 * frontier)` into the bridge's capture pages, so several armed tracks can show
 * the same capture without a second copy of the audio, and so a punch-in that
 * begins mid-capture starts at its own frame rather than at the device's.
 *
 * THREE THINGS THAT ARE DELIBERATE AND WILL LOOK WRONG OTHERWISE.
 *
 * 1. `getRootComponent()` is NULL and `isLiveRecording()` is true, and
 *    `STrack` therefore keeps this clip OUT of the bus mixers entirely — the
 *    same routing decision `objects/track` already makes for MIDI clips, for
 *    the same reason. There is no component to freeze pages from, so inserting
 *    it would cost a dummy freeze per page per clip AND make
 *    `twView::getComponent() returned nullptr` fire once per freeze forever.
 *    What the user HEARS while recording is the live monitor lane (D9's
 *    "Auto = input while stopped or recording"), never this clip; what they
 *    SEE is this clip. At stop the whole thing is replaced by the final
 *    WAV-backed cut, which is a perfectly ordinary one.
 *
 * 2. `getRandomSource()` IS the growing source, non-null, which is what routes
 *    `SCutRendererInline` down its sample-backed branch to
 *    `getInlineRenderer()` instead of down the container branch (which would
 *    demand a rendered capture that will never exist). A short read past the
 *    frontier zero-fills and never blocks — that is L3a's contract and it is
 *    what makes a concurrent UI read of a live capture safe.
 *
 * 3. The peak ladder is EXTENDED FROM THE FRONTIER, never recomputed. A
 *    five-minute take rescanned ten times a second is 90 GB of reads a minute;
 *    extending is O(new frames). `extendPeaks_()` is UI-thread only and reads
 *    only below the frontier it loaded, which is exactly the discipline
 *    `twGrowingCaptureSource` documents for a reader.
 *
 * Growth is published by `publishGrowth()`, called by `SAudioRecorder` on its
 * main-thread ~10 Hz tick. The bridge thread NEVER touches this object: it
 * stores a frontier in an atomic and that is the whole of the hand-over
 * (THREADING.md rule 1, and design 4's "the bridge thread never touches the
 * model").
 */
class SRecordingContent
    : public SObject
{
    Q_OBJECT
public:
    /// `src` is the bridge's capture pages; `startFrame` the capture frame this
    /// content begins at (0 for a take that started with the capture, non-zero
    /// for a punch-in that began later).
    SRecordingContent( SProject *project,
                       std::shared_ptr<twGrowingCaptureSource> src,
                       std::uint64_t startFrame = 0 );
    ~SRecordingContent() override;

    // ---- SObject, the four pure virtuals --------------------------------
    std::shared_ptr<twComponent> getRootComponent() override
    { return std::shared_ptr<twComponent>(); }
    QWidget *getDetailEditWidget( QWidget *parent = NULL ) override;
    QWidget *getInlineEditWidget( QWidget *parent = NULL ) override;
    SObjectRenderer *getInlineRenderer() override;

    // ---- SObject, the ones that make it a growing audio content ---------
    twRandomSource *getRandomSource() override;
    bool     hasDuration() const override { return true; }
    length_t getDuration() const override;
    bool     hasPreview() const override { return true; }
    int      getPreview( preview_t *dest, offset_t start, length_t length,
                         offset_t nProbes ) override;
    /// THE routing predicate (see note 1 above).
    bool isLiveRecording() const override { return true; }

    // ---- the recorder's side --------------------------------------------
    /// Main thread, ~10 Hz. Re-reads the frontier, extends the peak ladder and
    /// emits `durationChanged` when the length actually moved. Returns the
    /// new length in frames.
    length_t publishGrowth();

    /// Frames available in THIS content's window, right now.
    length_t availableFrames() const;

    /// The capture frame this content's frame 0 is.
    std::uint64_t startFrame() const { return startFrame_; }

    /// Stop growing: the capture has ended, so the length is now final.
    /// Further `publishGrowth()` calls are no-ops.
    void freeze() { frozen_ = true; }
    bool isFrozen() const { return frozen_; }

    const std::shared_ptr<twGrowingCaptureSource> &source() const { return src_; }

private:
    void extendPeaks_();

    std::shared_ptr<twGrowingCaptureSource> src_;
    std::uint64_t startFrame_ = 0;
    length_t      published_  = 0;      // last length emitted
    bool          frozen_     = false;

    // The incremental peak ladder, in this content's own frame domain. One
    // entry per `peakHop_` frames, signed envelope in [-128,127], the same
    // convention SObject::straightCalcPreviewData() uses so the shared
    // draw loop (swaveformdraw) applies unchanged.
    std::vector<preview_t> peaks_;
    std::uint64_t          peaksThrough_ = 0;   // frames of THIS window folded
    static constexpr offset_t peakHop_ = 256;

    SRecordingRendererInline *inlineRenderer_ = nullptr;
    std::vector<sample_t>     scanBuf_;         // extendPeaks_ scratch
};

#endif // _SRECORDINGCONTENT_H_
