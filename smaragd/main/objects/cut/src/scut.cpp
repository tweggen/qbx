
#include <iostream>
#include <math.h>
#include <cstdlib>
#include <vector>
#include <chrono>

#include <QDebug>

#include "tw/core/twfraction.h"
#include "tw/graph/twcomponent.h"
#include "tw/sources/twrandomsource.h"
#include "tw/sources/twsamplereader.h"
#include "tw/sources/twloopreader.h"
#include "tw/sources/twgrainsource.h"
#include "tw/sources/twcapturingsource.h"
#include "app/model/sappcontext.h"
#include "app/model/sproject.h"
#include "app/objects/cut/scut.h"
#include "app/model/slink.h"
#include "app/objects/cut/scutrndrinline.h"
#include "app/persistence/sprojectloader.h"
#include "tw/schedule/capture_revalidator.h"

using namespace std;

SCutSnapshot SCut::getSnapshot() const
{
    // Non-blocking lock to avoid deadlock during UI paint when revalidator
    // is modifying state. If mutex is held, return the last good snapshot.
    std::unique_lock<std::mutex> lock(mutex(), std::defer_lock);
    if (!lock.try_lock()) {
        // Could not acquire lock: return last good snapshot (safe for rendering).
        // This is always initialized and contains valid data from previous successful lock.
        return lastGoodSnapshot_;
    }

    SCutSnapshot snap;
    snap.startOffset = startOffset_;
    snap.loopLength = loopLength_;
    snap.cutDuration = cutDuration_;
    snap.grainParams = grainParams_;
    snap.reader = currentReader_;  // Always valid, complete, atomic (Unix page cache)

    // Update the last good snapshot for fallback use when lock fails on next call.
    lastGoodSnapshot_ = snap;

    return snap;
}

void SCut::ensureReader()
{
    if( readerTried_ ) return;
    rebuildReader( getSnapshot() );
}

// (Re)build the playback chain: content source -> optional grain stretch ->
// our own cursor. Called eagerly from setGrainParams / setWindow (on the UI
// thread) so the one-time grain materialisation never lands in a realtime audio
// block. Builds into nextReader_ completely, then swaps atomically
// (Unix page cache model: audio thread always sees a complete, valid state).
void SCut::rebuildReader( const SCutSnapshot &snap )
{
    readerTried_ = true;

    // FAST PATH: skip the rebuild when the reader chain would be byte-for-byte
    // the same one we already hold. The chain depends only on the content source
    // (constant), whether a grain stage is interposed (and its params), and
    // whether a loop reader is used (and its loop window) — NOT on cutDuration or
    // (for a non-looping clip) the slip offset. Trimming/extending or slipping a
    // plain sample clip therefore reuses the existing reader instead of minting a
    // new component + latch on every drag tick. Only sample-backed cuts qualify:
    // a container cut reads through a capture sized to the window, so a duration
    // change there genuinely requires a fresh capture/reader.
    if( content_->getSObject().getRandomSource() && currentReader_.reader ) {
        bool needGrain = !snap.grainParams.isIdentity();
        bool needLoop  = ( snap.loopLength > 0 && snap.loopLength < snap.cutDuration );
        bool sameGrain = !needGrain
            || ( builtGrain_.stretch    == snap.grainParams.stretch
              && builtGrain_.pitchCents == snap.grainParams.pitchCents
              && builtGrain_.grainSize  == snap.grainParams.grainSize
              && builtGrain_.crossfade  == snap.grainParams.crossfade );
        bool sameLoop = !needLoop
            || ( builtLoopStart_ == snap.startOffset
              && builtLoopLength_ == snap.loopLength );
        if( builtNeedGrain_ == needGrain && builtNeedLoop_ == needLoop
            && sameGrain && sameLoop )
            return;   // chain unchanged — keep the current reader
    }

    // STEP 1: Build new reader state completely out-of-band
    // (audio thread continues using currentReader_ during this entire process)
    std::shared_ptr<twSampleReader> newReader;
    std::shared_ptr<twGrainSource> newGrain;
    bool newLooping = false;

    twRandomSource *rs = content_->getSObject().getRandomSource();
    // For container-backed cuts (no random source), build the capture synchronously
    // so the reader chain can be constructed properly. This ensures looped playback
    // of group cuts works correctly (bug b: cycle mode playback).
    bool needCapture = !rs;  // Only container-backed cuts need capture building
    bool grainAlreadyApplied = false;
    if( needCapture ) {
        buildCapture_();
        if( capture_ ) {
            rs = capture_.get();
        } else {
            // Container-backed capture build failed: no fallback
            goto swap_complete;
        }
    }
    // For grained sample-backed cuts: grain will be applied on-demand in the reader,
    // not pre-materialized. This avoids offset issues from materializing the entire output.

    {
        twRandomSource *view = rs;
        if( !snap.grainParams.isIdentity() ) {
            newGrain = std::make_shared<twGrainSource>( *rs, snap.grainParams );
            view = newGrain.get();
        }
        tw303aEnvironment &env = *(SAppContext::get().get303aEnvironment());

        // The cut window (startOffset_, cutDuration_, loopLength_) lives in the
        // grain OUTPUT (stretched) domain — the same domain twGrainSource is
        // addressed in (split action, stretch drag and the waveform preview all
        // agree on this; the source position is startOffset/stretch). So the
        // offsets pass through UNCHANGED whether or not a grain stage is
        // interposed. Multiplying by the stretch here (the old code) applied the
        // factor twice and made playback read a different source window than the
        // preview displayed.
        offset_t adjustedStartOffset = snap.startOffset;
        length_t adjustedLoopLength = snap.loopLength;

        if( snap.loopLength > 0 && snap.loopLength < snap.cutDuration ) {
            twLoopReader *lr = new twLoopReader( env, *view, adjustedStartOffset, adjustedLoopLength );
            lr->init();
            newReader = std::shared_ptr<twSampleReader>( lr );
            newLooping = true;
        } else {
            newReader = std::shared_ptr<twSampleReader>( view->acquireReader( env, adjustedStartOffset ) );
            newLooping = false;
        }
    }

swap_complete:
    // STEP 2: Atomic swap (Unix page cache model with refcounted lifecycle)
    // Audio thread snapshot keeps old readers alive via shared_ptr refcount.
    // Even if swapped out, readers not deleted until all snapshots released.
    {
        std::lock_guard<std::mutex> lock( readerSwapLock_ );

        // NO manual delete needed: shared_ptr handles deferred deletion
        // oldReader_ refcount decrements automatically when overwritten
        // If audio thread holds oldReader_ snapshot, it stays alive (refcount >= 1)

        // Move current to old (for deferred deletion via refcount)
        oldReader_ = currentReader_;

        // Move next to current (becomes visible to audio thread immediately)
        currentReader_.reader = newReader;
        currentReader_.grain = newGrain;
        currentReader_.captureRef = capture_;   // Share ownership: keeps capture alive during render
        currentReader_.looping = newLooping;
        currentReader_.generation++;  // Increment on successful swap
    }

    // Record the chain we just built so the next call can take the fast path.
    if( rs ) {
        builtNeedGrain_  = ( newGrain != NULL );
        builtNeedLoop_   = newLooping;
        builtGrain_      = snap.grainParams;
        builtLoopStart_  = snap.startOffset;
        builtLoopLength_ = snap.loopLength;
    }
}

// Render a container content (a track/mixer sub-arrangement) once into an owned
// twCapturingSource, so each placement reads a cheap, independent snapshot rather
// than re-pulling — and fighting the single cursor of — the live graph (proposal
// 07 step 5). The whole content [0, dur) is captured (the cut's startOffset_
// indexes into it exactly like a sample source), so two cuts windowing the same
// container still each get a correct view. Returns NULL for real sources (the
// sample path) or content we cannot capture.
// Recursively render `obj`'s mono output into dest[0,len) (pre-zeroed), reading
// every child RANDOM-ACCESS — never pulling a live, cursor-bearing streaming
// component. This is proposal 10 Phase 1: "random access composes; streams
// don't." A track-of-tracks is the sum of its children's block k, each faulted
// recursively, so there is no single shared cursor to fight (the nested-group
// mixed-content bug).
//
// Dispatch:
//   - leaf random source (e.g. SPlainWave): read it directly.
//   - SCut clip: a windowed view of its content (slip offset + grain stretch;
//     loop tiling is deferred — a looping clip captures its linear window).
//   - container (STrack / SStdMixer, or any other child-bearing SObject): sum
//     children at their start times, then apply the container's own gain/mute
//     (mirroring twTrackMix::calcOutputTo) so a captured container is
//     self-contained exactly like the live path.
// Internal: build the capture (container render or grained sample render) if needed.
// Initializes capture_ by rendering the content's audio (with grain if applicable).
// Should only be called when capture_ is null.
void SCut::buildCapture_()
{
    // Serialize concurrent builders: the UI thread (rebuildReader) and the
    // revalidator worker (revalPrepPreview) may both arrive here. The render
    // below can take tens of milliseconds, so this is a dedicated mutex — the
    // object mutex() must never be held that long.
    std::lock_guard<std::mutex> buildLock( captureBuildMutex_ );

    if( capture_ ) {
        return;  // Already built
    }

    SObject &c = content_->getSObject();
    SCutSnapshot snap = getSnapshot();
    tw303aEnvironment &env = *(SAppContext::get().get303aEnvironment());

    // Check if we need to build a capture:
    // 1. Container-backed cuts (no random source)
    // 2. Grained sample-backed cuts (need to materialize the grain transformation)
    bool isContainerBacked = !c.getRandomSource();
    bool isGrained = !snap.grainParams.isIdentity();

    if( !isContainerBacked && !isGrained ) {
        return;
    }

    if( !c.hasDuration() ) {
        return;
    }

    length_t need = (length_t) snap.startOffset + snap.cutDuration;
    length_t dur  = (length_t) c.getDuration();
    length_t n = dur > need ? dur : need;
    if( n <= 0 ) return;

    std::vector<sample_t> buf;
    length_t captureLen = n;

    if( isContainerBacked ) {
        // Container-backed: Phase 3 - use freezePage for page-based rendering
        // Instead of recursive offline rendering (renderObjectInto), call freezePage
        // on the content's twComponent to materialize its output to pages.
        // Seek container to start before freezing to ensure children are positioned.
        c.seekTo( 0 );
        twComponent &rootComp = c.getRootComponent();

        buf.resize( (size_t) n, 0.0f );

        // Freeze the component's output to pages and copy samples to buffer
        // Page at a time to avoid huge allocations.
        size_t remainingSamples = n;
        size_t bufOffset = 0;
        uint64_t pagePos = 0;

        while( remainingSamples > 0 && bufOffset < n ) {
            auto frozenPage = rootComp.freezePage(
                pagePos,
                nullptr,     // No input data for container renders
                0,
                (length_t) remainingSamples,
                env.getSRate(),
                nullptr      // No prior state
            );

            if( !frozenPage || frozenPage->validFrames == 0 ) {
                break;
            }

            // Copy frozen page samples to output buffer
            size_t samplesToCopy = std::min((size_t)frozenPage->validFrames, remainingSamples);
            for( size_t i = 0; i < samplesToCopy && i < frozenPage->samples.size(); ++i ) {
                buf[bufOffset + i] = frozenPage->samples[i];
            }

            bufOffset += samplesToCopy;
            remainingSamples -= samplesToCopy;
            pagePos += samplesToCopy;

            // Stop if we got fewer samples than requested (component can't produce more)
            if( samplesToCopy < frozenPage->samples.size() ) {
                break;
            }
        }
    } else if( isGrained ) {
        // Grained sample-backed: read through grain source to get transformed output
        // The grain source materializes the time-stretched output, so we read the
        // full transformed signal and store it for both preview and playback.
        twRandomSource *rs = c.getRandomSource();
        // Ensure the source is positioned at the start before grain processing
        c.seekTo( 0 );
        auto grainSource = std::make_shared<twGrainSource>( *rs, snap.grainParams );
        length_t grainedLen = grainSource->length();

        // startOffset already lives in the grain OUTPUT (stretched) domain, the
        // domain the grain source is addressed in — use it directly.
        offset_t grainOffset = snap.startOffset;

        // Read from the grain-stretched offset, limited to remaining content
        length_t availFromOffset = grainedLen > grainOffset ? grainedLen - grainOffset : 0;
        length_t toRead = snap.cutDuration > availFromOffset ? availFromOffset : snap.cutDuration;

        buf.resize( (size_t) toRead, 0.0f );
        if( toRead > 0 ) {
            grainSource->read( grainOffset, buf.data(), toRead, 0 );
        }
        captureLen = toRead;
    }

    {
        // Publish under the object mutex: readers (getPreview's null check,
        // invalidateCapture's reset) synchronize on the same lock.
        auto newCapture = std::make_shared<twCapturingSource>( std::move( buf ), captureLen, 1, env.getSRate() );
        std::lock_guard<std::mutex> lock( mutex() );
        capture_ = newCapture;
    }

    if( !captureConnected_ ) {
        // Transparent invalidation: any applied action drops the snapshot so the
        // next pull re-captures the edited arrangement.
        QObject::connect( &getProject(), SIGNAL( arrangementChanged() ),
                          this, SLOT( invalidateCapture() ) );
        captureConnected_ = true;
    }
}

void SCut::invalidateCapture()
{
    // Drop the cached render (async model).
    // Use reset() not delete: the shared_ptr releases SCut's reference, but readers
    // (audio thread) may still hold references via currentPage().
    {
        std::lock_guard<std::mutex> lock(mutex());
        currentPage_.reset();
        nextPage_.reset();
        capture_.reset();  // Also clear the old capture_ cache (Phase 5e integration)
        // Peaks are derived from capture_; a rebuilt capture must not be drawn
        // through the old envelope.
        if( capPeaks_ ) { ::free( capPeaks_ ); capPeaks_ = NULL; capPeakN_ = 0; }
        readerTried_ = false;  // Reset so ensureReader() will rebuild capture on next call
    }

    // Invalidate affected aspects: capture rebuild affects Preview, Playback, Metadata
    // but NOT Export (export is on-demand only and can be recomputed independently).
    // These aspects will be recomputed lazily on demand via async revalidator.
    invalidateAspects(Preview | Playback | Metadata);

    // NOTE: Do NOT eagerly rebuild anything here (lazy invalidation model).
    // The revalidator's background thread will handle recomputation when needed.

    // Notify parent containers that window parameters have changed (for live drag feedback)
    emit windowParamsChanged();
}

// IRevalidatable: runs on a revalidator worker before the generic preview
// render, no locks held. Container-backed (and grained) cuts preview from the
// offline capture; historically that was only built when PLAYBACK first
// touched the clip (ensureReader), so such clips sat as gray boxes after
// project load until played. Building it here gives them preview data as soon
// as the Preview aspect is revalidated.
bool SCut::revalPrepPreview()
{
    // No-ops for plain sample-backed cuts (they preview straight from the wave).
    buildCapture_();

    // Container-backed cuts have no live-preview fallback: without a capture
    // the clip cannot be drawn. Report failure so Preview stays stale and the
    // next paint's getCapture() retries (e.g. content still loading).
    if( !content_->getSObject().getRandomSource() ) {
        std::lock_guard<std::mutex> lock( mutex() );
        return capture_ != nullptr;
    }
    return true;
}

// Build a peak cache of the capture (the rendered snapshot) for waveform
// preview, in the container frame domain. min/max are stored as positive
// magnitudes in [0,127], matching SObject::straightCalcPreviewData()'s
// convention so the same draw loop (swaveformdraw) applies.
bool SCut::ensureCapturePeaks()
{
    if( capPeaks_ ) return true;
    if( !capture_ ) return false;
    length_t len = (length_t) capture_->length();
    if( len <= 0 ) return false;

    offset_t skip = 256;
    while( len < skip * 128 && skip > 1 ) skip >>= 1;
    if( skip < 1 ) skip = 1;
    while( (offset_t)( len / skip ) >= 0x200000 ) skip *= 2;
    offset_t n = (offset_t)( len / skip ) + 1;

    capPeaks_ = (preview_t *) ::calloc( sizeof( preview_t ), n );
    if( !capPeaks_ ) return false;
    capPeakSkip_ = skip;
    capPeakN_ = n;

    sample_t *buf = (sample_t *) ::malloc( skip * sizeof( sample_t ) );
    if( !buf ) { ::free( capPeaks_ ); capPeaks_ = NULL; capPeakN_ = 0; return false; }

    for( offset_t i = 0; i < (offset_t) len; i += skip ) {
        offset_t chunk = ( i + skip <= (offset_t) len ) ? skip : ( (offset_t) len - i );
        capture_->read( i, buf, chunk, 0 );
        sample_t mn = SAMPLE_NORM_MAX, mx = SAMPLE_NORM_MIN;
        for( offset_t j = 0; j < chunk; ++j ) {
            sample_t a = buf[j];
            if( a < mn ) mn = a;
            if( a > mx ) mx = a;
        }
        // Signed envelope, matching SObject::straightCalcPreviewData()'s
        // convention: .max is the upper (positive) edge, .min the lower
        // (negative) edge, each in [-128,127]. (Clamping to [0,127] here was the
        // bug that turned the waveform into a comb.)
        double mxs = ( mx * 127. ) / SAMPLE_NORM_MAX;
        if( mxs > 127. ) mxs = 127.; if( mxs < -128. ) mxs = -128.;
        double mns = ( mn * -127. ) / SAMPLE_NORM_MIN;
        if( mns > 127. ) mns = 127.; if( mns < -128. ) mns = -128.;
        offset_t idx = i / skip;
        if( idx < n ) {
            capPeaks_[idx].min = (char) mns;
            capPeaks_[idx].max = (char) mxs;
        }
    }
    ::free( buf );
    return true;
}

// Waveform peaks for the asset preview, read from the capture (shared with the
// audio render). `start`/`length` are in the container frame domain; the cut
// renderer's InlineRenderContext maps the clip window there. Sample cuts have no
// capture and defer to the base preview.
int SCut::getPreview( preview_t *dest, offset_t start, length_t length,
                      offset_t nProbes )
{
    // Try async capture first (non-blocking, may be stale or invalid)
    // If not ready, fall back to live content preview (sample-backed cuts only)
    auto page = getPreviewCapture();
    if( !page || !capture_ ) {
        // No capture available (yet or ever)
        // For sample-backed cuts: preview the content live
        // For container-backed cuts: return error (no fallback)
        if( getContent().getRandomSource() ) {
            // Sample-backed: live preview available
            return getContent().getPreview( dest, start, length, nProbes );
        } else {
            // Container-backed: no live preview; page cache only
            // Return error so UI can show placeholder
            return -1;
        }
    }

    // Capture exists but may not have peaks yet
    if( !ensureCapturePeaks() ) {
        // For sample-backed cuts: try live preview
        // For container-backed cuts: return error
        if( getContent().getRandomSource() ) {
            return getContent().getPreview( dest, start, length, nProbes );
        } else {
            return -1;
        }
    }

    if( nProbes <= 0 ) return -1;
    if( length < 1 ) length = 1;

    for( offset_t k = 0; k < nProbes; ++k ) {
        // Aggregate the true signed envelope over each output column: the lower
        // edge is the smallest .min, the upper edge the largest .max (this is
        // what avoids aliasing combs when zoomed out).
        // Note: start/length are already in the grain OUTPUT (stretched) domain from InlineRenderContext,
        // matching the peak cache indexing, so no conversion needed.
        offset_t s = start + ( k * length ) / nProbes;
        offset_t e = start + ( ( k + 1 ) * length ) / nProbes;
        offset_t i0 = s / capPeakSkip_;
        offset_t i1 = ( e > s ? e - 1 : s ) / capPeakSkip_;
        char aggMin = 0, aggMax = 0;   // default: silence -> flat midline
        bool any = false;
        for( offset_t i = i0; i <= i1 && i < capPeakN_; ++i ) {
            char pmn = capPeaks_[i].min, pmx = capPeaks_[i].max;
            if( !any ) { aggMin = pmn; aggMax = pmx; any = true; }
            else {
                if( pmn < aggMin ) aggMin = pmn;
                if( pmx > aggMax ) aggMax = pmx;
            }
        }
        dest[k].min = aggMin;
        dest[k].max = aggMax;
    }
    return 0;
}

twComponent &SCut::getRootComponent()
{
    // Get reader snapshot WITHOUT triggering expensive ensureReader() during UI initialization.
    // Only build reader if it exists (was previously built for playback).
    // If not built yet, fall back to content component (lazy evaluation).
    SCutSnapshot snap = getSnapshot();
    if( snap.reader.reader ) return *snap.reader.reader;

    // Reader not yet built (no playback request yet). This is normal during project load.
    // ensureReader() will be called lazily when audio actually needs this clip (playback).
    // For now, use the content's component (works for UI, rendering pipeline).
    return content_->getRootComponent();
}

QWidget *SCut::getDetailEditWidget( QWidget * )
{
    return NULL;
}

QWidget *SCut::getInlineEditWidget( QWidget * )
{
    return NULL;
}

SObjectRenderer *SCut::getInlineRenderer( void ) 
{
    if( !inlineRenderer_ ) {
        inlineRenderer_ = new SCutRendererInline( *this );
    }
    return inlineRenderer_;
}

int SCut::seekTo( offset_t off )
{
    // FIXME: bounds check!!!
    ensureReader();
    SCutSnapshot snap = getSnapshot();

    // A loop reader is cut-relative (it adds its own loop base = startOffset_);
    // a plain reader needs startOffset_ folded in here.
    // snap.reader.reader is always valid (double-buffer model: Unix page cache).
    //
    // startOffset_ already lives in the grain OUTPUT (stretched) domain — the
    // domain the reader (twGrainSource view) is addressed in — so no stretch
    // conversion is applied here.
    offset_t seekPos = snap.reader.looping ? off : off + snap.startOffset;
    if( snap.reader.reader ) return snap.reader.reader->seekTo( seekPos );
    return content_->getSObject().seekTo( seekPos );
}

// Map a clip-relative position into the reader's own domain. This is the same
// mapping SCut::seekTo applies before seeking, factored out so twView can
// translate positions for BOTH seekTo and freezePage: the freeze path seeks the
// component directly with the position it is given, so the slip offset must be
// folded in up front (this was the split-clip-renders-from-file-start bug).
// ensureReader() runs first so the component handed to the track (via
// getRootComponent) is our own reader, in the domain this mapping targets.
offset_t SCut::mapTimelineToComponentPos( offset_t off )
{
    ensureReader();
    SCutSnapshot snap = getSnapshot();

    // A loop reader is cut-relative (it adds its own loop base = startOffset_).
    if( snap.reader.looping ) return off;

    // Plain reader over source/grain view: fold the slip offset in. Both `off`
    // (clip-relative, timeline samples) and startOffset_ live in the grain
    // OUTPUT (stretched) domain, which is also the domain the grain view is
    // addressed in — no stretch conversion.
    return off + snap.startOffset;
}

void SCut::setStartOffset( offset_t off )
{
    {
        std::lock_guard<std::mutex> lock(mutex());
        startOffset_ = off;
    }
    invalidateCapture();  // Window change requires new capture (formal guidelines)
}

void SCut::setDuration( length_t dur )
{
    {
        std::lock_guard<std::mutex> lock(mutex());
        cutDuration_ = dur;
    }
    invalidateCapture();  // Invalidate UI data; twView decides if revalidation needed
    // Reader rebuild deferred to ensureReader() on playback access (demand-driven)
    emit durationChanged( dur );
}

void SCut::setLoopStart( offset_t s )
{
    {
        std::lock_guard<std::mutex> lock(mutex());
        loopStart_ = s;
    }
}

void SCut::setLoopLength( length_t l )
{
    length_t dur;
    {
        std::lock_guard<std::mutex> lock(mutex());
        loopLength_ = l;
        dur = cutDuration_;
    }
    invalidateCapture();  // Invalidate UI data; twView decides if revalidation needed
    // Reader rebuild deferred to ensureReader() on playback access (demand-driven)
    emit durationChanged( dur );
}

void SCut::setWindow( offset_t startOffset, length_t duration,
                      length_t loopLength, double stretch )
{
    {
        std::lock_guard<std::mutex> lock(mutex());
        startOffset_ = startOffset;
        cutDuration_ = duration;
        loopLength_  = loopLength;
        grainParams_.stretch = stretch;
    }
    invalidateCapture();  // Invalidate UI data; twView decides if revalidation needed
    // Reader rebuild deferred to ensureReader() on playback access (demand-driven)
    emit durationChanged( duration );
}

offset_t SCut::getLoopStart() const
{
    return loopStart_;
}

length_t SCut::getDuration() const
{
    // Use snapshot for consistent reads (multithreading policy: Phase 1).
    // Audio thread may be reading duration during rendering; ensure we get
    // a consistent value taken together with other window parameters.
    return getSnapshot().cutDuration;
}

void SCut::setGrainParams( const twGrainParams &p )
{
    SCutSnapshot snap;

    // All window parameter modifications must be protected by mutex()
    // (formal concurrency guidelines: Contract B, Snapshot Pattern).
    // Read oldStretch INSIDE lock to avoid TOCTOU race (formal verification).
    {
        std::lock_guard<std::mutex> lock(mutex());
        double oldStretch = grainParams_.stretch;  // ← Read inside lock
        grainParams_ = p;
        if( oldStretch > 0.0 && p.stretch > 0.0 && p.stretch != oldStretch ) {
            double k = p.stretch / oldStretch;
            cutDuration_ = (length_t) llround( (double) cutDuration_ * k );
            startOffset_ = (offset_t) llround( (double) startOffset_ * k );
        }
        // Manually construct snapshot without re-acquiring lock
        snap.startOffset = startOffset_;
        snap.loopLength = loopLength_;
        snap.cutDuration = cutDuration_;
        snap.grainParams = grainParams_;
        snap.reader = currentReader_;
    }

    rebuildReader( snap );   // pre-build off the audio thread (caller is the UI thread)
    emit durationChanged( snap.cutDuration );
}

void SCut::setStretch( double s )
{
    twGrainParams p;
    {
        std::lock_guard<std::mutex> lock(mutex());  // Read under lock
        p = grainParams_;  // Snapshot params
    }
    p.stretch = s;  // Modify copy outside lock
    setGrainParams( p );  // Pass modified copy
}

void SCut::setPitchCents( double cents )
{
    twGrainParams p;
    {
        std::lock_guard<std::mutex> lock(mutex());  // Read under lock
        p = grainParams_;  // Snapshot params
    }
    p.pitchCents = cents;  // Modify copy outside lock
    setGrainParams( p );  // Pass modified copy
}

SCut::~SCut()
{
    // Unregister from content's dependents (lazy invalidation, proposal 06).
    // This cut is no longer referencing the content, so remove this link from
    // the content's dependent set. The content can now change without affecting this cut.
    if( content_ ) {
        content_->getSObject().removeDependentLink(content_);
    }

    // Clean up all reader states (double-buffer model with refcounting)
    // No manual delete needed: shared_ptr handles automatic destruction
    // when refcount reaches zero (after all audio thread snapshots released)
    currentReader_.reader.reset();
    currentReader_.grain.reset();
    oldReader_.reader.reset();
    oldReader_.grain.reset();
    nextReader_.reader.reset();
    nextReader_.grain.reset();

    // capture_ is a shared_ptr — it will be freed when all references are released
    capture_.reset();
    if( capPeaks_ ) { ::free( capPeaks_ ); capPeaks_ = NULL; }
    delete content_;
    content_ = NULL;
}

SCut::SCut( SProject *parentProject, SLink &content )
    : SObject( parentProject ),
      startOffset_( 0 ),
      loopLength_( 0 ),
      inlineRenderer_( NULL ),
      readerTried_( false )
{
    // Get revalidator from project (Phase 4, async capture model)
    revalidator_ = (parentProject != nullptr) ? parentProject->getRevalidator() : nullptr;

    content_ = &content;
    content_->setParent(this);
    /* was:
    if( content_->parent() ) {
        content_->parent()->removeChild( content_ );
    }
    insertChild( content_ );
    */
    if( content_->getSObject().hasDuration() ) {
        cutDuration_ = content_->getSObject().getDuration();
    } else {
        // Default to 0.5 seconds, calculated from project sample rate
        int srate = (parentProject != nullptr) ? parentProject->getSRate() : 48000;
        cutDuration_ = srate / 2;
    }

    // Register as dependent of the content object (lazy invalidation, proposal 06).
    // When the content's audio state changes (mute, solo, volume), this cut will be
    // notified to invalidate only affected aspects (Playback, Metadata), not the
    // entire arrangement.
    content_->getSObject().addDependentLink(content_);
}

SCut::SCut( SProject *parentProject, SObject &content )
    : SObject( parentProject ),
      startOffset_( 0 ),
      loopLength_( 0 ),
      inlineRenderer_( NULL ),
      readerTried_( false )
{
    // Get revalidator from project (Phase 4, async capture model)
    revalidator_ = (parentProject != nullptr) ? parentProject->getRevalidator() : nullptr;

    content_ = new SLink( content, this );
    if( content_->getSObject().hasDuration() ) {
        cutDuration_ = content_->getSObject().getDuration();
    } else {
        // Default to 0.5 seconds, calculated from project sample rate
        int srate = (parentProject != nullptr) ? parentProject->getSRate() : 48000;
        cutDuration_ = srate / 2;
    }
    // Loop start defaults to no loop.
    loopStart_ = cutDuration_;

    // Register as dependent of the content object (lazy invalidation, proposal 06).
    // When the content's audio state changes (mute, solo, volume), this cut will be
    // notified to invalidate only affected aspects (Playback, Metadata), not the
    // entire arrangement.
    content_->getSObject().addDependentLink(content_);
}

int SCut::serializeSelfAttributes( QTextStream &o )
{
    Fraction startOffsetFrac(getStartOffset(), 1);
    Fraction cutDurationFrac(cutDuration_, 1);
    Fraction loopLengthFrac(loopLength_, 1);

    o << " startOffset='" << QString::fromStdString(startOffsetFrac.toString()) << "'"
      << " cutDuration='" << QString::fromStdString(cutDurationFrac.toString()) << "'"
      << " loopLength='" << QString::fromStdString(loopLengthFrac.toString()) << "'"
      << " stretch='" << grainParams_.stretch << "'"
      << " pitchCents='" << grainParams_.pitchCents << "'";

    // Grain parameters stored as time (milliseconds) for rate independence.
    // Default 48kHz: 2048 frames ≈ 42.67 ms, 512 frames ≈ 10.67 ms
    int srate = 48000;  // default fallback
    if( parent() ) srate = getProject().getSRate();
    double grainSizeMs = (grainParams_.grainSize * 1000.0) / srate;
    double crossfadeMs = (grainParams_.crossfade * 1000.0) / srate;
    o << " grainSizeMs='" << grainSizeMs << "'"
      << " crossfadeMs='" << crossfadeMs << "'";

    SObject::serializeSelfAttributes( o );
    return 0;
}

int SCut::readPostChildrenAttributes( QDomElement &element )
{
    SObject::readPostChildrenAttributes( element );

    // Note: Invalidation is suppressed during project load via SProject::disableInvalidation(),
    // so calling setStartOffset() is safe - it won't trigger invalidation chains.
    // This way setters work normally and code stays consistent.

    QString data;
    data = element.attribute( "startOffset", "0" );
    Fraction startOffsetFrac = parseFractionOrDouble( data.toStdString() );
    setStartOffset( (offset_t)startOffsetFrac.toDouble() );

    // Load cutDuration. If missing, use a sensible default based on project sample rate
    // (0.5 seconds). This matches the constructor default and ensures consistency
    // across different project sample rates.
    data = element.attribute( "cutDuration" );
    if( data.isEmpty() ) {
        int srate = 48000;  // default fallback
        if( parent() ) srate = getProject().getSRate();
        cutDuration_ = srate / 2;
    } else {
        Fraction cutDurationFrac = parseFractionOrDouble( data.toStdString() );
        cutDuration_ = (length_t)cutDurationFrac.toDouble();
    }
    data = element.attribute( "loopLength", "0" );
    Fraction loopLengthFrac = parseFractionOrDouble( data.toStdString() );
    loopLength_ = (length_t)loopLengthFrac.toDouble();

    // Grain params: stretch and pitchCents are dimensionless, grainSize and
    // crossfade are now stored as milliseconds (rate-independent) and converted
    // to samples based on project sample rate.
    grainParams_.stretch    = element.attribute( "stretch", "1.0" ).toDouble();
    grainParams_.pitchCents = element.attribute( "pitchCents", "0.0" ).toDouble();

    int srate = 48000;  // default fallback
    if( parent() ) srate = getProject().getSRate();

    // Try to load time-based values (new format)
    QString grainSizeMsStr = element.attribute( "grainSizeMs" );
    if( !grainSizeMsStr.isEmpty() ) {
        // New format: stored as milliseconds, convert to samples
        double grainSizeMs = grainSizeMsStr.toDouble();
        grainParams_.grainSize = (length_t)( ( grainSizeMs * srate ) / 1000.0 + 0.5 );
    } else {
        // Backwards compatibility: old format was samples at assumed 48kHz
        // Scale to current project sample rate
        QString grainSizeStr = element.attribute( "grainSize" );
        if( grainSizeStr.isEmpty() ) {
            // Default: 42.67 ms (2048 @ 48kHz)
            grainParams_.grainSize = (length_t)( ( 42.666667 * srate ) / 1000.0 + 0.5 );
        } else {
            // Assume old value was at 48kHz, scale proportionally
            length_t oldSamples = grainSizeStr.toLongLong();
            grainParams_.grainSize = (length_t)( ( oldSamples * srate ) / 48000.0 + 0.5 );
        }
    }

    QString crossfadeMsStr = element.attribute( "crossfadeMs" );
    if( !crossfadeMsStr.isEmpty() ) {
        // New format: stored as milliseconds, convert to samples
        double crossfadeMs = crossfadeMsStr.toDouble();
        grainParams_.crossfade = (length_t)( ( crossfadeMs * srate ) / 1000.0 + 0.5 );
    } else {
        // Backwards compatibility: old format was samples at assumed 48kHz
        QString crossfadeStr = element.attribute( "crossfade" );
        if( crossfadeStr.isEmpty() ) {
            // Default: 10.666667 ms (512 @ 48kHz)
            grainParams_.crossfade = (length_t)( ( 10.666667 * srate ) / 1000.0 + 0.5 );
        } else {
            // Assume old value was at 48kHz, scale proportionally
            length_t oldSamples = crossfadeStr.toLongLong();
            grainParams_.crossfade = (length_t)( ( oldSamples * srate ) / 48000.0 + 0.5 );
        }
    }

    // Pre-build the playback chain now (load thread) so a stretched or looping
    // clip restored from disk does not materialise on the first realtime block.
    if( !grainParams_.isIdentity() || ( loopLength_ > 0 && loopLength_ < cutDuration_ ) )
        rebuildReader( getSnapshot() );

    return 0;
}

SLink *SCut::instantiateFromDomElement(
    SProjectLoader &projectLoader, QDomElement &element, SObject *parent )
{
    SLink *contentLink = NULL;
    // Find the first link child
    QDomNode childNode = element.firstChild();
    while( !childNode.isNull() ) {
        if( childNode.isElement() ) {
            qWarning() << "found SCut child " << childNode.nodeName() << Qt::endl;
            if( childNode.nodeName() == "SLink" ) {
                QDomElement childElement = childNode.toElement();
                QString objectId = childElement.attribute( "objectId" );
                // Look up the object id.
                contentLink = projectLoader.getObjectDictionary().value( objectId );
                if( contentLink ) break;
            }
        }
        childNode = childNode.nextSibling();
    }

    if( !contentLink ) {
        qWarning( "SCut did not have a child!!" );
        return NULL;
    }
    SCut *cut = new SCut( &projectLoader.getProject(), contentLink->getSObject() );
    cut->readPreChildrenAttributes( element );
    // Now read out the properties.
    cut->readPostChildrenAttributes( element );

    return new SLink( *cut, parent );
}

void SCut::queueWindowParamEvent( SCutWindowParamEventType type, double value )
{
    // Queue a window parameter change event for later processing.
    // Uses inherited mutex() from SObject (one mutex per object policy).
    std::lock_guard<std::mutex> lock( mutex() );
    SCutWindowParamEvent event{ type, value };
    windowParamEventQueue_.push_back( event );
}

void SCut::processWindowParamEvents()
{
    // Process all queued window parameter events.
    // Called after drag completes to apply all changes atomically,
    // including invalidateCapture() and rebuildReader().
    // Uses inherited mutex() from SObject (one mutex per object policy).
    std::vector<SCutWindowParamEvent> events;
    {
        std::lock_guard<std::mutex> lock( mutex() );
        if( windowParamEventQueue_.empty() ) return;
        events = windowParamEventQueue_;
        windowParamEventQueue_.clear();
    }

    // Apply all events under windowMutex_, then handle expensive operations
    SCutSnapshot snap;
    bool needsCaptureBuild = false;
    bool needsReaderBuild = false;

    {
        std::lock_guard<std::mutex> lock(mutex());
        for( const SCutWindowParamEvent &event : events ) {
            switch( event.type ) {
            case OFFSET_CHANGE:
                startOffset_ = (offset_t) event.value;
                needsCaptureBuild = true;
                break;
            case DURATION_CHANGE:
                cutDuration_ = (length_t) event.value;
                needsCaptureBuild = true;
                break;
            case LOOP_LENGTH_CHANGE:
                loopLength_ = (length_t) event.value;
                needsCaptureBuild = true;
                needsReaderBuild = true;
                break;
            case STRETCH_CHANGE:
                grainParams_.stretch = event.value;
                needsCaptureBuild = true;
                needsReaderBuild = true;
                break;
            }
        }
        // Manually construct snapshot without re-acquiring lock (we already hold it)
        snap.startOffset = startOffset_;
        snap.loopLength = loopLength_;
        snap.cutDuration = cutDuration_;
        snap.grainParams = grainParams_;
        snap.reader = currentReader_;
    }

    // Call invalidateCapture and rebuildReader outside the lock
    if( needsCaptureBuild ) {
        invalidateCapture();
    }
    if( needsReaderBuild ) {
        rebuildReader( snap );
    }
}

// Lazy invalidation: mark specific aspects as needing recomputation.
// Async model: schedules revalidation in background, returns immediately.
// No blocking, no eager rebuild. Stale data available as fallback.
void SCut::invalidateAspects(uint32_t aspects)
{
    if (aspects == 0 || !revalidator_) return;

    // During project loading, invalidation is suppressed (all pages invalid anyway).
    // When enableInvalidation() is called, a full recomputation pass will begin.
    SProject *proj = getProjectSafe();
    if (proj && proj->isInvalidationSuppressed()) {
        return;  // Skip invalidation chain while loading
    }

    auto start = std::chrono::high_resolution_clock::now();

    {
        std::lock_guard<std::mutex> lock(mutex());

        // If Playback is invalidated, drop the stale page
        // (audio data in currentPage will become stale with mute/solo applied)
        if (aspects & Playback) {
            currentPage_.reset();
        }

        // Clear the bits for invalidated aspects
        validAspects_ &= ~aspects;
    }

    // Determine priority based on aspect type
    int priority = 5;  // Default: Metadata
    if (aspects & Playback) priority = 10;  // High: audio thread needs this
    if (aspects & Preview)  priority = 1;   // Low: UI can tolerate stale

    // Schedule async revalidation (returns immediately, no blocking)
    revalidator_->scheduleRevalidation(this, aspects, priority);

    // Propagate invalidation to dependents (transitive chain).
    // If this cut's Playback is stale, cuts that reference this cut are also stale.
    // Example: Track A muted → Cut C (refs Track A) invalidated → Cut D (refs Cut C) invalidated
    if (aspects & Playback) {
        notifyDependentsChanged(Playback | Metadata);
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::high_resolution_clock::now() - start).count();
    if (aspects & Playback) {
        qWarning() << "invalidateAspects(Playback) for cut" << getSName()
                   << "took" << elapsed << "μs";
    }
}

// Non-blocking capture access: get current/stale page or schedule async revalidation.
// Returns immediately with current page if all aspects valid, else stale page, else nil.
// Never waits for revalidation (falls back to stale data for zero audio dropouts).
std::shared_ptr<CapturePageData> SCut::getCapture(uint32_t aspectsMask)
{
    if (aspectsMask == 0 || !revalidator_) return nullptr;

    // Get current page (may be stale, may be null, never blocks)
    // Read current page without locking (shared_ptr copy is atomic).
    auto page = currentPage();

    // If current page has all needed aspects, return immediately
    // Acquire page lock to safely read validAspects (prevents torn reads during concurrent writes)
    if (page) {
        std::lock_guard<std::mutex> pageLock(page->pageMutex);
        if ((page->validAspects & aspectsMask) == aspectsMask) {
            return page;
        }
    }

    // Page missing aspects: unconditionally schedule revalidation.
    // The revalidator's job processor will check needsRevalidation_nolock() under the lock
    // to skip if state changed between now and job dequeue. This avoids the try_lock
    // workaround and keeps the UI render path non-blocking.
    int priority = 5;  // Default: Metadata
    if (aspectsMask & Playback) priority = 10;
    if (aspectsMask & Preview)  priority = 1;
    revalidator_->scheduleRevalidation(this, aspectsMask, priority);

    // Return current page anyway (stale is OK; better than null/dropout)
    return page;
}

// Check if revalidation is needed for specific aspects.
// Used by revalidator to decide whether to process a job.
bool SCut::needsRevalidation_nolock(uint32_t aspectsMask) const
{
    if (aspectsMask == 0) return false;
    // Note: _nolock refers to not holding cut mutex; we still need page lock for valid aspect check
    if (!currentPage_) return true;
    std::lock_guard<std::mutex> pageLock(currentPage_->pageMutex);
    return (currentPage_->validAspects & aspectsMask) != aspectsMask;
}

bool SCut::needsRevalidation(uint32_t aspectsMask) const
{
    if (aspectsMask == 0) return false;

    std::lock_guard<std::mutex> lock(mutex());
    return needsRevalidation_nolock(aspectsMask);
}

// Phase 5e: Page cache implementation

// Self-registration with the project loader (proposal 14, Phase 5): the
// persistence module names no concrete types; each slice registers its own
// element name. Relies on the app being an OBJECT library (no TU elision).
static const bool s_registered_scut =
    ( SProjectLoader::registerSObjectClass( "SCut",
          SCut::instantiateFromDomElement ), true );
