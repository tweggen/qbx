
#include <iostream>
#include <math.h>
#include <cstdlib>
#include <vector>
#include <chrono>

#include <QDebug>

#include "twfraction.h"
#include "twcomponent.h"
#include "twrandomsource.h"
#include "twsamplereader.h"
#include "twloopreader.h"
#include "twgrainsource.h"
#include "twcapturingsource.h"
#include "sapplication.h"
#include "sproject.h"
#include "scut.h"
#include "slink.h"
#include "strack.h"
#include "scutrndrinline.h"
#include "sprojectloader.h"
#include "capture_revalidator.h"

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
    twSampleReader *newReader = NULL;
    twGrainSource *newGrain = NULL;
    bool newLooping = false;

    twRandomSource *rs = content_->getSObject().getRandomSource();
    // TODO: Phase 4 - integrate async capture model with reader rebuild
    // if( !rs ) rs = ensureCapture();  // OLD: sync capture fallback
    if( !rs ) goto swap_complete;   // no-op, keep current

    {
        twRandomSource *view = rs;
        if( !snap.grainParams.isIdentity() ) {
            newGrain = new twGrainSource( *rs, snap.grainParams );
            view = newGrain;
        }
        tw303aEnvironment &env = *(SApplication::app().get303aEnvironment());
        if( snap.loopLength > 0 && snap.loopLength < snap.cutDuration ) {
            twLoopReader *lr = new twLoopReader( env, *view, snap.startOffset, snap.loopLength );
            lr->init();
            newReader = lr;
            newLooping = true;
        } else {
            newReader = view->acquireReader( env );
            newLooping = false;
        }
    }

swap_complete:
    // STEP 2: Atomic swap (Unix page cache model)
    // Audio thread sees oldReader_ become available for deletion, currentReader_ updated
    {
        std::lock_guard<std::mutex> lock( readerSwapLock_ );

        // Clean up previous old reader (oldReader_.captureRef shared_ptr released automatically)
        if( oldReader_.reader ) { delete oldReader_.reader; oldReader_.reader = NULL; }
        if( oldReader_.grain )  { delete oldReader_.grain;  oldReader_.grain = NULL; }

        // Move current to old (for deferred deletion)
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
static void renderObjectInto( SObject &obj, sample_t *dest, length_t len,
                              tw303aEnvironment &env, int rate, int depth )
{
    if( len <= 0 ) return;
    if( depth > 64 ) return;            // defensive: a container must not contain itself

    // Leaf: a real random source. Read [0, min(len, srcLen)).
    if( twRandomSource *rs = obj.getRandomSource() ) {
        length_t n = rs->length();
        if( n > len ) n = len;
        if( n > 0 ) rs->read( 0, dest, n, 0 );
        return;
    }

    // SCut: the windowed (and optionally grain-stretched) view of its content.
    if( SCut *cut = dynamic_cast<SCut *>( &obj ) ) {
        SObject &content = cut->getContent();
        offset_t off = cut->getStartOffset();
        twGrainParams gp = cut->getGrainParams();

        twRandomSource *contentRs = content.getRandomSource();   // borrowed if non-null
        twCapturingSource *ownedContent = NULL;                  // temp if we rendered
        if( !contentRs ) {
            length_t clen = content.hasDuration() ? (length_t) content.getDuration() : 0;
            if( clen <= 0 ) return;
            std::vector<sample_t> buf( (size_t) clen, 0.0f );
            renderObjectInto( content, buf.data(), clen, env, rate, depth + 1 );
            ownedContent = new twCapturingSource( std::move( buf ), clen, 1, rate );
            contentRs = ownedContent;
        }

        twRandomSource *view = contentRs;
        twGrainSource *grain = NULL;
        if( !gp.isIdentity() ) {
            grain = new twGrainSource( *contentRs, gp );         // output (stretched) domain
            view = grain;
        }
        // startOffset and len are in the (post-grain) output domain — the same
        // domain `view` is addressed in. read() zero-fills past end.
        view->read( off, dest, len, 0 );

        delete grain;
        delete ownedContent;
        return;
    }

    // Container: sum children at their start times.
    for( SLink *lk : obj.childLinks() ) {
        if( !lk ) continue;
        SObject &child = lk->getSObject();
        offset_t start = lk->getStartTime();
        if( start >= len ) continue;
        length_t childLen = child.hasDuration() ? (length_t) child.getDuration() : 0;
        length_t avail = len - (length_t) start;
        length_t n = childLen < avail ? childLen : avail;
        if( n <= 0 ) continue;
        std::vector<sample_t> tmp( (size_t) n, 0.0f );
        renderObjectInto( child, tmp.data(), n, env, rate, depth + 1 );
        for( length_t i = 0; i < n; ++i ) dest[(size_t)start + i] += tmp[(size_t) i];
    }

    // Apply this container's own gain/mute, matching the live mixer.
    double factor = obj.isMuted() ? 0.0 : pow( 10.0, obj.getVolume() / 20.0 );
    if( factor != 1.0 )
        for( length_t i = 0; i < len; ++i ) dest[(size_t) i] *= (sample_t) factor;
}

// Internal: build the capture (container render) if needed.
// Initializes capture_ by rendering the container's audio.
// Should only be called when capture_ is null.
void SCut::buildCapture_()
{
    if( capture_ ) return;  // Already built

    SObject &c = content_->getSObject();
    if( c.getRandomSource() ) return;  // real source -> sample path, no capture needed
    if( !c.hasDuration() ) return;

    // Use snapshot to read window parameters consistently (multithreading policy: Phase 1).
    SCutSnapshot snap = getSnapshot();
    length_t need = (length_t) snap.startOffset + snap.cutDuration;
    length_t dur  = (length_t) c.getDuration();
    length_t n = dur > need ? dur : need;
    if( n <= 0 ) return;

    tw303aEnvironment &env = *(SApplication::app().get303aEnvironment());

    // Proposal 10 Phase 1: build the capture by RECURSIVELY summing the
    // container's children read random-access, instead of streaming the live
    // twTrackMix (whose per-buffer child re-seeks lose a sub-track for a
    // track-of-tracks). Materialise once into a resident buffer.
    std::vector<sample_t> buf( (size_t) n, 0.0f );
    renderObjectInto( c, buf.data(), n, env, env.getSRate(), 0 );
    capture_ = std::make_shared<twCapturingSource>( std::move( buf ), n, 1, env.getSRate() );

    if( !captureConnected_ ) {
        // Transparent invalidation: any applied action drops the snapshot so the
        // next pull re-captures the edited arrangement.
        QObject::connect( &getProject(), SIGNAL( arrangementChanged() ),
                          this, SLOT( invalidateCapture() ) );
        captureConnected_ = true;
    }
}

// Old API: ensure the capture (container render) is built.
// TODO: Phase 4 - replace with getCapture() / getPreviewCapture() async API.
// For now, commented out to avoid conflicting with the new getCapture(aspectsMask) method.
/*
twRandomSource *SCut::ensureCapture()
{
    // Use new aspect-based API
    ensureCapture(Preview | Playback | Metadata);

    if( capture_ ) return capture_.get();
    return NULL;
}
*/

void SCut::invalidateCapture()
{
    // Drop the cached render (async model).
    // Use reset() not delete: the shared_ptr releases SCut's reference, but readers
    // (audio thread) may still hold references via currentPage().
    {
        std::lock_guard<std::mutex> lock(mutex());
        currentPage_.reset();
        nextPage_.reset();
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
    ensureReader();
    // Use snapshot to get double-buffered reader state (Unix page cache model).
    // currentReader_ is always complete & valid; never becomes NULL while in use.
    SCutSnapshot snap = getSnapshot();
    if( snap.reader.reader ) return *snap.reader.reader;
    // Content is not a random-access source: fall back to its shared component.
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
    if( snap.reader.reader ) return snap.reader.reader->seekTo( snap.reader.looping ? off : off + snap.startOffset );
    return content_->getSObject().seekTo( off + snap.startOffset );
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
    // FIXME: clip.
    {
        std::lock_guard<std::mutex> lock(mutex());
        cutDuration_ = dur;
    }
    invalidateCapture();  // Window change requires new capture (formal guidelines)
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
    SCutSnapshot snap;
    {
        std::lock_guard<std::mutex> lock(mutex());
        loopLength_ = l;
        // Capture snapshot while holding lock for consistent rebuild
        snap = getSnapshot();
    }
    invalidateCapture();  // Window change requires new capture (formal guidelines)
    rebuildReader( snap );   // loop on/off changes the playback chain
    emit durationChanged( snap.cutDuration );
}

void SCut::setWindow( offset_t startOffset, length_t duration,
                      length_t loopLength, double stretch )
{
    SCutSnapshot snap;
    {
        std::lock_guard<std::mutex> lock(mutex());
        startOffset_ = startOffset;
        cutDuration_ = duration;
        loopLength_  = loopLength;
        grainParams_.stretch = stretch;
        // Manually construct snapshot without re-acquiring lock
        snap.startOffset = startOffset_;
        snap.loopLength = loopLength_;
        snap.cutDuration = cutDuration_;
        snap.grainParams = grainParams_;
        snap.reader = currentReader_;
    }
    invalidateCapture();  // Window change requires new capture (formal guidelines)
    rebuildReader( snap );   // one rebuild for the whole window change (UI thread)
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

    // Clean up all reader states (double-buffer model)
    if( currentReader_.reader ) { delete currentReader_.reader; currentReader_.reader = NULL; }
    if( currentReader_.grain )  { delete currentReader_.grain;  currentReader_.grain = NULL; }
    if( oldReader_.reader ) { delete oldReader_.reader; oldReader_.reader = NULL; }
    if( oldReader_.grain )  { delete oldReader_.grain;  oldReader_.grain = NULL; }
    if( nextReader_.reader ) { delete nextReader_.reader; nextReader_.reader = NULL; }
    if( nextReader_.grain )  { delete nextReader_.grain;  nextReader_.grain = NULL; }

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

    fprintf(stderr, ">>> SCut::readPostChildrenAttributes END: final cutDuration=%lld\n", (long long)cutDuration_);
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
