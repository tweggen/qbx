
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

// Caller must hold mutex(). Assemble a consistent snapshot from live fields and
// refresh lastGoodSnapshot_ for the try-lock fallback path.
SCutSnapshot SCut::buildSnapshot_nolock() const
{
    SCutSnapshot snap;
    snap.startOffset = getStartOffset();   // derived: floor(srcStart * stretch)
    snap.loopLength = loopLength_;
    snap.cutDuration = cutDuration_;
    snap.grainParams = grainParams_;
    snap.reader = currentReader_;  // consistent with builtXxx_ under mutex()

    // Update the last good snapshot for fallback use when lock fails on next call.
    lastGoodSnapshot_ = snap;
    return snap;
}

SCutSnapshot SCut::getSnapshot() const
{
    // Non-blocking lock to avoid deadlock during UI paint when revalidator
    // is modifying state. If mutex is held, return the last good snapshot.
    std::unique_lock<std::mutex> lock(mutex(), std::defer_lock);
    if (!lock.try_lock()) {
        // Could not acquire lock: return last good snapshot (safe for RT audio;
        // proposal 16 serves the stale-but-consistent reader during an edit).
        return lastGoodSnapshot_;
    }
    return buildSnapshot_nolock();
}

// Proposal 19 Phase 2b: the freeze path must resolve the CURRENT reader, never a
// stale lastGoodSnapshot_ — a freeze that observes the post-edit content epoch
// must also observe the post-edit reader, or it stamps stale content as current
// (the takes_group_broadcast flake). Blocks on mutex(); this is bounded because
// edits and the reader swap hold mutex() only for short critical sections (the
// heavy reader build runs unlocked). Only the freeze/resolution path uses this;
// the RT audio path keeps getSnapshot()'s try-lock fallback.
SCutSnapshot SCut::getSnapshotBlocking() const
{
    std::lock_guard<std::mutex> lock(mutex());
    return buildSnapshot_nolock();
}

void SCut::ensureReader()
{
    if( readerTried_ ) return;
    // Blocking snapshot (Phase 2b): build from the CURRENT window params. With
    // the try-lock getSnapshot(), a second concurrent first-build (two broadcast
    // tracks sharing this SCut) could fail the try-lock and build from the
    // DEFAULT lastGoodSnapshot_ — minting an identity-grain/no-loop reader that
    // overwrites the correct one. Blocking guarantees the real params.
    rebuildReader( getSnapshotBlocking() );
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
    // Proposal 19 Phase 2b: read currentReader_/builtXxx_ under mutex() — the
    // swap below and getSnapshot() both use mutex(), so this fast-path read can
    // never tear against a concurrent swap (two broadcast tracks freezing the
    // same shared SCut used to race here on first build).
    {
        std::lock_guard<std::mutex> lock( mutex() );
        if( content_->getSObject().getRandomSource() && currentReader_.reader ) {
            bool needGrain = !snap.grainParams.isIdentity();
            bool needLoop  = ( snap.loopLength > WarpedLen(0)
                            && snap.loopLength < warpedFromClip(snap.cutDuration) );
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
    }

    // STEP 1: Build new reader state completely out-of-band
    // (audio thread continues using currentReader_ during this entire process)
    std::shared_ptr<twSampleReader> newReader;
    std::shared_ptr<twGrainSource> newGrain;
    std::shared_ptr<twCapturingSource> captureLocal;
    bool newLooping = false;

    twRandomSource *rs = content_->getSObject().getRandomSource();
    // For container-backed cuts (no random source), build the capture synchronously
    // so the reader chain can be constructed properly. This ensures looped playback
    // of group cuts works correctly (bug b: cycle mode playback).
    bool needCapture = !rs;  // Only container-backed cuts need capture building
    if( needCapture ) {
        buildCapture_();
        {
            // Pin the capture we build the chain over: capture_ is published
            // and reset under mutex(), and a concurrent invalidateCapture()
            // must not free it between here and the reader construction below.
            std::lock_guard<std::mutex> lock( mutex() );
            captureLocal = capture_;
        }
        if( captureLocal ) {
            rs = captureLocal.get();
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
        // grain OUTPUT (warped) domain — the same domain twGrainSource is
        // addressed in (split action, stretch drag and the waveform preview all
        // agree on this; the source position is startOffset/stretch). The
        // WarpedPos/WarpedLen types guarantee no stretch factor is folded in
        // here (multiplying by the stretch was the old double-apply bug);
        // .frames() unwraps at the reader seam, which is integral — no
        // rounding takes place.
        if( snap.loopLength > WarpedLen(0)
            && snap.loopLength < warpedFromClip(snap.cutDuration) ) {
            // Must be shared before init(): createOutputLatches() calls
            // shared_from_this() (throws std::bad_weak_ptr on a raw instance).
            auto lr = std::make_shared<twLoopReader>( env, *view,
                                                      snap.startOffset.frames(),
                                                      snap.loopLength.frames() );
            lr->init();
            newReader = lr;
            newLooping = true;
        } else {
            newReader = view->acquireReader( env, snap.startOffset.frames() );
            newLooping = false;
        }

        // The scheduler's PageNode holds ONLY the reader component; src_ is a
        // raw reference into newGrain/captureLocal. Anchor the whole upstream
        // chain in the reader itself so a later reader swap (drag churn
        // overwriting oldReader_) or ~SCut can never free the grain/capture
        // while a queued freeze still renders through this reader.
        newReader->retainUpstream( newGrain );
        newReader->retainUpstream( captureLocal );
    }

swap_complete:
    // STEP 2: Atomic swap (Unix page cache model with refcounted lifecycle)
    // Audio thread snapshot keeps old readers alive via shared_ptr refcount.
    // Even if swapped out, readers not deleted until all snapshots released.
    //
    // Proposal 19 Phase 2b: publish currentReader_ AND the "last built chain"
    // record (builtXxx_) atomically under mutex() — the same lock getSnapshot()
    // and the fast path use. A struct-of-shared_ptr assignment is NOT atomic, so
    // the old readerSwapLock_ (used at only this one site, never on the read
    // side) never actually excluded a reader; a freeze could observe a torn
    // currentReader_ (new .reader + old .grain, or a shared_ptr mid-reassign).
    {
        std::lock_guard<std::mutex> lock( mutex() );

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

        // Record the chain we just built so the next call can take the fast path
        // (paired with currentReader_ under the same lock — no torn read).
        if( rs ) {
            builtNeedGrain_  = ( newGrain != NULL );
            builtNeedLoop_   = newLooping;
            builtGrain_      = snap.grainParams;
            builtLoopStart_  = snap.startOffset;
            builtLoopLength_ = snap.loopLength;
        }

        // The reader is part of the snapshot: keep the try-lock fallback
        // current so a failed try_lock never serves the pre-swap chain (P19).
        buildSnapshot_nolock();
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
    // Blocking: a capture built from a stale window would be stamped valid with
    // wrong extents (same stale try-lock class; the worker holds no object lock
    // here — dispatchRecomputation runs outside CS1 — so no self-deadlock).
    SCutSnapshot snap = getSnapshotBlocking();
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

    // The capture must cover the window's warped end (slip offset folded in). A
    // looping cut only needs its ONE linear loop segment captured — the loop
    // reader wraps within [startOffset, startOffset+loopLength) and the preview
    // tiles the same segment — so capturing the full (extended) duration would
    // only zero-pad a huge tail that nothing reads (and that the linear preview
    // used to draw as a flat/zero waveform past the content end).
    bool looping = snap.loopLength > WarpedLen( 0 )
                && snap.loopLength < warpedFromClip( snap.cutDuration );
    length_t windowLen = looping ? snap.loopLength.frames()
                                 : snap.cutDuration.frames();
    WarpedPos windowEnd = warpedFromClip( ClipPos( windowLen ), snap.startOffset );
    length_t need = (length_t) windowEnd.frames();
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
        std::shared_ptr<twComponent> rootComp = c.getRootComponent();

        buf.resize( (size_t) n, 0.0f );

        // Freeze the component's output to pages and copy samples to buffer
        // Page at a time to avoid huge allocations.
        size_t remainingSamples = n;
        size_t bufOffset = 0;
        uint64_t pagePos = 0;

        while( remainingSamples > 0 && bufOffset < n ) {
            auto frozenPage = rootComp->freezePage(
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

        // startOffset already lives in the grain OUTPUT (warped) domain, the
        // domain the grain source is addressed in — unwrap at the seam.
        offset_t grainOffset = (offset_t) snap.startOffset.frames();

        // Read from the grain-stretched offset, limited to remaining content
        length_t availFromOffset = grainedLen > (length_t) grainOffset
                                 ? grainedLen - (length_t) grainOffset : 0;
        length_t wantFrames = snap.cutDuration.frames();
        length_t toRead = wantFrames > availFromOffset ? availFromOffset : wantFrames;

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
    // Arms onArrangementChanged() (the connect itself lives in the ctor: this
    // function runs on the revalidator worker, where Qt calls are banned).
    everHadCapture_.store( true );
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

std::shared_ptr<twComponent> SCut::getRootComponent()
{
    // Get reader snapshot WITHOUT triggering expensive ensureReader() during UI initialization.
    // Only build reader if it exists (was previously built for playback).
    // If not built yet, fall back to content component (lazy evaluation).
    // Blocking snapshot (Phase 2b): the freeze path must see the current reader.
    SCutSnapshot snap = getSnapshotBlocking();
    if( snap.reader.reader ) return std::static_pointer_cast<twComponent>(snap.reader.reader);

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
    // Blocking snapshot (Phase 2b): seek the CURRENT reader, not a stale one.
    SCutSnapshot snap = getSnapshotBlocking();

    // A loop reader is cut-relative (it adds its own loop base = startOffset_);
    // a plain reader needs startOffset_ folded in here.
    // snap.reader.reader is always valid (double-buffer model: Unix page cache).
    //
    // startOffset_ already lives in the grain OUTPUT (warped) domain — the
    // domain the reader (twGrainSource view) is addressed in. The typed
    // conversion folds the slip offset in; no stretch factor can appear here.
    offset_t seekPos = (offset_t) clipToReaderMap( snap.reader.looping )
                           .map( Fraction( (int64_t) off ) ).floorToInt();
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
    // Blocking snapshot (Phase 2b): map against the CURRENT reader domain so a
    // freeze never folds a pre-edit slip/loop mapping into a post-edit page.
    SCutSnapshot snap = getSnapshotBlocking();

    // Same shared map as seekTo and the preview (proposal 18 Phase 4): a
    // loop reader is cut-relative (identity), a plain reader is addressed
    // in the warped domain (slip folded in, never a stretch factor).
    return (offset_t) clipToReaderMap( snap.reader.looping )
               .map( Fraction( (int64_t) off ) ).floorToInt();
}

// Proposal 19 Inv-1: resolve component AND mapped position from ONE snapshot.
// This is exactly getRootComponent() + mapTimelineToComponentPos() fused so both
// read the SAME reader — a freeze that observes the post-edit reader as its
// component also folds the post-edit slip/loop mapping, and vice versa. Splitting
// them into two getSnapshotBlocking() calls (as twView used to) let a concurrent
// lazy rebuildReader() land between them, so the component and the mapping could
// come from different reader generations (the takes_group_broadcast race class).
twResolvedClip SCut::resolveClip( offset_t off )
{
    ensureReader();
    SCutSnapshot snap = getSnapshotBlocking();

    twResolvedClip r;
    // Component: the built reader if we have one, else the shared content source
    // (matches getRootComponent()'s fallback for the not-yet-built case).
    r.component = snap.reader.reader
        ? std::static_pointer_cast<twComponent>( snap.reader.reader )
        : content_->getRootComponent();
    // Mapping: identical fold to mapTimelineToComponentPos(), against THIS snap.
    r.mappedPos = (offset_t) clipToReaderMap( snap.reader.looping )
                      .map( Fraction( (int64_t) off ) ).floorToInt();
    return r;
}


// Map content (SOURCE-domain) dirty ranges into clip-relative ranges
// (proposal 18 Phase 5). source -> warped is the exact StretchMap
// (conservative floor/ceil at the edges); a looping window then yields one
// image per repetition via twLoopMap::preimagesWithin - the exact
// counterpart of "an edit inside the loop segment dirties EVERY repetition,
// and nothing else". Non-looping windows subtract the slip anchor and clamp.
QList<SObject::SDirtyRange> SCut::mapChildRangesToSelf(
    SLink *childLink, const QList<SDirtyRange> &childRanges )
{
    Q_UNUSED( childLink );
    // Blocking: this maps EDIT dirty ranges — a stale stretch/window here
    // mis-scopes invalidation (stale try-lock class; edit path, may block).
    SCutSnapshot snap = getSnapshotBlocking();
    const Fraction stretch = snap.grainParams.stretch > Fraction(0)
                           ? snap.grainParams.stretch : Fraction(1);
    const int64_t dur = snap.cutDuration.frames();
    const bool looping = snap.loopLength > WarpedLen(0)
                      && snap.loopLength < warpedFromClip(snap.cutDuration);

    QList<SDirtyRange> out;
    for( const SDirtyRange &r : childRanges ) {
        if( r.end <= r.start ) continue;
        Fraction wStart = Fraction( (int64_t) r.start ) * stretch;
        Fraction wEnd   = Fraction( (int64_t) r.end ) * stretch;
        if( looping ) {
            twLoopMap loop( Fraction( snap.startOffset.frames() ),
                            Fraction( snap.loopLength.frames() ) );
            for( const twMapSegment &seg : loop.preimagesWithin(
                     wStart, wEnd - wStart, Fraction(0), Fraction(dur) ) ) {
                int64_t s = seg.parentStart.floorToInt();
                int64_t e = ( seg.parentStart + seg.length ).ceilToInt();
                if( s < 0 ) s = 0;
                if( e > dur ) e = dur;
                if( e > s ) out.append( { (offset_t) s, (offset_t) e } );
            }
        } else {
            int64_t s = ( wStart - Fraction( snap.startOffset.frames() ) ).floorToInt();
            int64_t e = ( wEnd - Fraction( snap.startOffset.frames() ) ).ceilToInt();
            if( s < 0 ) s = 0;
            if( e > dur ) e = dur;
            if( e > s ) out.append( { (offset_t) s, (offset_t) e } );
        }
    }
    return out;
}

void SCut::setStartOffset( offset_t off )
{
    {
        std::lock_guard<std::mutex> lock(mutex());
        setStartOffsetRaw( WarpedPos( (int64_t) off ) );
        buildSnapshot_nolock();   // keep the try-lock fallback current (P19)
    }
    invalidateCapture();  // Window change requires new capture (formal guidelines)
}

void SCut::setSrcStart( const Fraction &srcStart )
{
    {
        std::lock_guard<std::mutex> lock(mutex());
        srcStart_ = srcStart;
        buildSnapshot_nolock();   // keep the try-lock fallback current (P19)
    }
    invalidateCapture();  // Window change requires new capture (formal guidelines)
}

void SCut::setDuration( length_t dur )
{
    {
        std::lock_guard<std::mutex> lock(mutex());
        cutDuration_ = ClipLen( dur );
        buildSnapshot_nolock();   // keep the try-lock fallback current (P19)
    }
    invalidateCapture();  // Invalidate UI data; twView decides if revalidation needed
    // Reader rebuild deferred to ensureReader() on playback access (demand-driven)
    emit durationChanged( dur );
}

void SCut::setLoopStart( offset_t s )
{
    {
        std::lock_guard<std::mutex> lock(mutex());
        loopStart_ = WarpedPos( (int64_t) s );
    }
}

void SCut::setLoopLength( length_t l )
{
    length_t dur;
    {
        std::lock_guard<std::mutex> lock(mutex());
        loopLength_ = WarpedLen( l );
        dur = cutDuration_.frames();
        buildSnapshot_nolock();   // keep the try-lock fallback current (P19)
    }
    invalidateCapture();  // Invalidate UI data; twView decides if revalidation needed
    // Reader rebuild deferred to ensureReader() on playback access (demand-driven)
    emit durationChanged( dur );
}

void SCut::setWindow( const Fraction &srcStart, ClipLen duration,
                      WarpedLen loopLength, const Fraction &stretch )
{
    {
        std::lock_guard<std::mutex> lock(mutex());
        srcStart_    = srcStart;
        cutDuration_ = duration;
        loopLength_  = loopLength;
        grainParams_.stretch = stretch;
        buildSnapshot_nolock();   // keep the try-lock fallback current (P19)
    }
    invalidateCapture();  // Invalidate UI data; twView decides if revalidation needed
    // Reader rebuild deferred to ensureReader() on playback access (demand-driven)
    emit durationChanged( duration.frames() );
}

WarpedPos SCut::getLoopStart() const
{
    return loopStart_;
}

length_t SCut::getDuration() const
{
    // Use snapshot for consistent reads (multithreading policy: Phase 1).
    // Audio thread may be reading duration during rendering; ensure we get
    // a consistent value taken together with other window parameters.
    return getSnapshot().cutDuration.frames();
}

length_t SCut::getDurationBlocking() const
{
    // Blocking read for the edit/signal path (see header) — never the stale
    // try-lock fallback.
    return getSnapshotBlocking().cutDuration.frames();
}

void SCut::setGrainParams( const twGrainParams &p )
{
    SCutSnapshot snap;

    // All window parameter modifications must be protected by mutex()
    // (formal concurrency guidelines: Contract B, Snapshot Pattern).
    // Read oldStretch INSIDE lock to avoid TOCTOU race (formal verification).
    {
        std::lock_guard<std::mutex> lock(mutex());
        Fraction oldStretch = grainParams_.stretch;  // ← Read inside lock
        grainParams_ = p;
        if( oldStretch > Fraction(0) && p.stretch > Fraction(0)
            && p.stretch != oldStretch ) {
            // Preserve-span rescale of the timeline duration (the source
            // span duration/stretch stays put). The SOURCE ANCHOR is
            // authoritative and simply does not move under a stretch edit;
            // the old warped-offset rescale, and its per-edit rounding,
            // are gone (proposal 18 Phase 3).
            Fraction k = p.stretch / oldStretch;
            cutDuration_ = ClipLen( ( Fraction( cutDuration_.frames() ) * k ).floorToInt() );
        }
        // Build the snapshot under the held lock — identical fields to the old
        // manual construction, and it refreshes lastGoodSnapshot_ so the
        // try-lock fallback reflects this mutation (P19).
        snap = buildSnapshot_nolock();
    }

    // A grained cut's capture BAKES the grain params in (buildCapture_ reads
    // the whole clip through a twGrainSource built from them) and
    // buildCapture_ early-returns while a capture exists — so without this, a
    // stretch/pitch edit leaves the WAVEFORM PREVIEW drawing the previous
    // transform forever (playback is unaffected: it builds its own grain over
    // the raw source below). setWindow() invalidates for the same reason.
    invalidateCapture();

    rebuildReader( snap );   // pre-build off the audio thread (caller is the UI thread)
    emit durationChanged( snap.cutDuration.frames() );
}

void SCut::setStretch( double s )
{
    twGrainParams p;
    {
        std::lock_guard<std::mutex> lock(mutex());  // Read under lock
        p = grainParams_;  // Snapshot params
    }
    // UI double -> exact rational once, at creation (denominator capped).
    p.stretch = doubleToFractionWithLookup( s ).limitedTo( (uint64_t)1 << 20 );
    setGrainParams( p );  // Pass modified copy
}

void SCut::setPitchCents( double cents )
{
    twGrainParams p;
    {
        std::lock_guard<std::mutex> lock(mutex());  // Read under lock
        p = grainParams_;  // Snapshot params
    }
    // Clamp outside the lock. Beyond ±2 octaves the fixed-grain transposition
    // is no longer musically useful (and an extreme ratio makes each grain read
    // a huge input span), so the same range the pitch dialog offers is the
    // model's range too — every entry point clamps identically.
    p.pitchCents = clampPitchCents( cents );
    setGrainParams( p );  // Pass modified copy
}

SCut::~SCut()
{
    // FIRST, before tearing down any member: retire from the revalidator. The
    // reval queue holds a BORROWED pointer to this cut, and removeRef()'s
    // deleteLater() is a one-way trip — a preview job scheduled (e.g. from a
    // repaint's getCapture()) AFTER our ref hit zero cannot keep us alive, so a
    // worker could otherwise run SCut::buildCapture_() on freed memory and lock a
    // destroyed captureBuildMutex_ (std::mutex::lock() throws → terminate). This
    // drops our queued jobs and blocks until no worker still touches us, while
    // every member below is still intact. (Grain-backed cuts hit this most: they
    // build a capture on the worker; repro = split a grained cut, delete the tail.)
    if( revalidator_ ) {
        revalidator_->retireObject( this );
    }

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

SCut::SCut( SProject *parentProject, SObject &content )
    : SObject( parentProject ),
      srcStart_( 0 ),
      loopLength_( 0 ),
      inlineRenderer_( NULL ),
      readerTried_( false )
{
    // Get revalidator from project (Phase 4, async capture model)
    revalidator_ = (parentProject != nullptr) ? parentProject->getRevalidator() : nullptr;

    // Own content link. Construct with parent=NULL, then setParent (slink.h
    // rule): a parent passed into the ctor delivers ChildAdded — and our
    // childEvent's gotChild() — while the SLink is still only a QObject.
    content_ = new SLink( content, NULL );
    content_->setParent( this );
    if( content_->getSObject().hasDuration() ) {
        cutDuration_ = ClipLen( content_->getSObject().getDuration() );
    } else {
        // Default to 0.5 seconds, calculated from project sample rate
        int srate = (parentProject != nullptr) ? parentProject->getSRate() : 48000;
        cutDuration_ = ClipLen( srate / 2 );
    }
    // Loop start defaults to no loop.
    loopStart_ = WarpedPos( cutDuration_.frames() );

    // Register as dependent of the content object (lazy invalidation, proposal 06).
    // When the content's audio state changes (mute, solo, volume), this cut will be
    // notified to invalidate only affected aspects (Playback, Metadata), not the
    // entire arrangement.
    content_->getSObject().addDependentLink(content_);

    // Initialize the try-lock fallback snapshot from the CONSTRUCTED state
    // (see the SLink ctor for the rationale — never the default-zeros struct).
    {
        std::lock_guard<std::mutex> lock(mutex());
        buildSnapshot_nolock();
    }

    // Transparent capture invalidation: any applied action drops the snapshot
    // so the next pull re-captures the edited arrangement. Connected HERE (main
    // thread) — it used to happen lazily inside buildCapture_(), which runs on
    // the revalidator worker (THREADING.md Rule 1: no Qt off the main thread).
    // The slot gates on everHadCapture_, preserving the lazy connect's
    // semantics: plain sample cuts (no capture, ever) stay out of it.
    if( parentProject ) {
        QObject::connect( parentProject, SIGNAL( arrangementChanged() ),
                          this, SLOT( onArrangementChanged() ) );
    }
}

void SCut::onArrangementChanged()
{
    // Only a cut that ever materialized a capture (container-backed/grained)
    // must re-capture after an applied action. Without this gate every cut
    // dropped its pages and rescheduled Preview|Playback|Metadata on EVERY
    // action — an invalidation storm that stalled offline renders (the
    // workers=8 takes_group_broadcast hang).
    if( !everHadCapture_.load() ) return;
    invalidateCapture();
}

int SCut::serializeSelfAttributes( QTextStream &o )
{
    Fraction cutDurationFrac(cutDuration_.frames(), 1);
    Fraction loopLengthFrac(loopLength_.frames(), 1);

    // srcStart is the exact, authoritative anchor; startOffset is the
    // derived warped-domain value kept for older builds reading this file.
    o << " srcStart='" << QString::fromStdString(srcStart_.toString()) << "'"
      << " startOffset='" << QString::fromStdString(Fraction(getStartOffset().frames(), 1).toString()) << "'"
      << " cutDuration='" << QString::fromStdString(cutDurationFrac.toString()) << "'"
      << " loopLength='" << QString::fromStdString(loopLengthFrac.toString()) << "'"
      << " stretch='" << QString::fromStdString( grainParams_.stretch.toString() ) << "'"
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

    // The stretch must be parsed BEFORE the anchor: the legacy startOffset
    // migration divides by it. Exact "n/d" parses losslessly; legacy
    // decimals recover once via lookup/continued fractions.
    grainParams_.stretch = parseFractionOrDouble(
        element.attribute( "stretch", "1" ).toStdString() );

    QString data = element.attribute( "srcStart" );
    if( !data.isEmpty() ) {
        // Exact anchor (proposal 18 Phase 3 format)
        srcStart_ = parseFractionOrDouble( data.toStdString() );
    } else {
        // Legacy warped-domain offset: migrate by exact division. The
        // result may be rational (the legacy rounding made visible);
        // keeping it exact avoids re-rounding on every load.
        Fraction warped = parseFractionOrDouble(
            element.attribute( "startOffset", "0" ).toStdString() );
        srcStart_ = grainParams_.stretch > Fraction(0)
                  ? warped / grainParams_.stretch : warped;
    }

    // Load cutDuration. If missing, use a sensible default based on project sample rate
    // (0.5 seconds). This matches the constructor default and ensures consistency
    // across different project sample rates.
    data = element.attribute( "cutDuration" );
    if( data.isEmpty() ) {
        int srate = 48000;  // default fallback
        if( parent() ) srate = getProject().getSRate();
        cutDuration_ = ClipLen( srate / 2 );
    } else {
        Fraction cutDurationFrac = parseFractionOrDouble( data.toStdString() );
        cutDuration_ = ClipLen( (length_t)cutDurationFrac.toDouble() );
    }
    data = element.attribute( "loopLength", "0" );
    Fraction loopLengthFrac = parseFractionOrDouble( data.toStdString() );
    loopLength_ = WarpedLen( (length_t)loopLengthFrac.toDouble() );

    // Grain params: pitchCents is dimensionless (stretch was parsed above,
    // before the anchor); grainSize and crossfade are stored as milliseconds
    // (rate-independent) and converted to samples at the project rate.
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

    // Loaded fields were written directly above (load thread): refresh the
    // try-lock fallback snapshot so it reflects the LOADED window, not the
    // construction-time state (P19 — the fallback must never lag the model).
    {
        std::lock_guard<std::mutex> lock(mutex());
        buildSnapshot_nolock();
    }

    // Pre-build the playback chain now (load thread) so a stretched or looping
    // clip restored from disk does not materialise on the first realtime block.
    if( !grainParams_.isIdentity()
        || ( loopLength_ > WarpedLen(0)
          && loopLength_ < warpedFromClip(cutDuration_) ) )
        rebuildReader( getSnapshotBlocking() );

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

    // Construct with parent=NULL, then setParent (slink.h rule): the parent's
    // childEvent must never see a half-constructed SLink.
    SLink *cutLink = new SLink( *cut, NULL );
    if( parent ) cutLink->setParent( parent );
    return cutLink;
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
                setStartOffsetRaw( WarpedPos( (int64_t) event.value ) );
                needsCaptureBuild = true;
                break;
            case DURATION_CHANGE:
                cutDuration_ = ClipLen( (length_t) event.value );
                needsCaptureBuild = true;
                break;
            case LOOP_LENGTH_CHANGE:
                loopLength_ = WarpedLen( (length_t) event.value );
                needsCaptureBuild = true;
                needsReaderBuild = true;
                break;
            case STRETCH_CHANGE:
                grainParams_.stretch = doubleToFractionWithLookup( event.value )
                                           .limitedTo( (uint64_t)1 << 20 );
                needsCaptureBuild = true;
                needsReaderBuild = true;
                break;
            }
        }
        // Manually construct snapshot without re-acquiring lock (we already hold it)
        snap.startOffset = getStartOffset();
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
