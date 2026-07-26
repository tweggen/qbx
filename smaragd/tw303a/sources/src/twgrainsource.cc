
#include <math.h>
#include <string.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include <cstdlib>

#include "tw/core/twlog.h"
#include "tw/sidecar/twaspects.h"
#include "tw/sidecar/twsidecarstore.h"
#include "tw/sources/twgrainsource.h"
#include "tw/sources/twpagedvocoder.h"

// twGrainSource realises a time-stretched / pitch-shifted view of another
// twRandomSource. Two synthesis backends live here, selected at runtime:
//
//   * twPagedVocoder (proposal 27, THE DEFAULT since M5) — in-house phase
//     vocoder: streaming block renders (no materialization) when the source
//     is resident-planar, offline whole-signal otherwise.
//   * TW_STRETCH_BACKEND=ola — the legacy fixed-hop time-slice overlap-add
//     (proposal 06), kept verbatim as a dependency-free reference (audible
//     warble on tonal material; not a quality path).
//
// Rubber Band (proposal 26) was REMOVED 2026-07-26 on the requester's
// decision: the vocoder had been load-bearing since M5, and dropping the
// GPL-licensed dependency exercises the relicensing freedom recorded in
// proposal 26/29. The warp.pcm backend byte 1 stays RESERVED for the
// retired Rubber Band path so historical cache keys never alias.
//
// Whatever the backend, the output length is EXACT (proposal 18 Phase 2):
// nFrames_ = floor(inLen * stretch), computed rationally — the render-boundary
// rounding rule the cut window (WarpedLen domain) depends on.

namespace {

// Proposal 27 M5 — runtime synthesis-backend selection. THE IN-HOUSE VOCODER
// IS THE DEFAULT since the M5 switchover (streaming, O(blocks) memory,
// transient-preserving; quality gates + requester listening sign-off in
// STATE.md). TW_STRETCH_BACKEND=ola selects the legacy overlap-add per run.
// Read once per process (deterministic within a run). Backend byte 1 is
// RESERVED: it was the removed Rubber Band path — the value must never be
// reused, so historical warp.pcm cache keys cannot alias a new backend.
enum { kBackendRubberBandRetired = 1, kBackendOla = 2, kBackendVocoder = 3 };

int stretchBackend()
{
    static const int backend = []() {
        const char *e = ::getenv( "TW_STRETCH_BACKEND" );
        if( e && ::strcmp( e, "ola" ) == 0 ) return (int) kBackendOla;
        return (int) kBackendVocoder;   // M5 default
    }();
    return backend;
}

// Proposal 27 M2 — the warp.pcm cache key params (canonical LE blob; field
// order is normative, see twaspects.h). Everything that affects the output
// BYTES is in here: the synthesis backend (RUNTIME-selected since M3), the
// rate/channel geometry, the EXACT stretch fraction as clamped by the ctor,
// the pitch, and the OLA engine knobs (vestigial under Rubber Band, but a
// duplicate entry is harmless where a wrong hit would not be).
void warpParamsBlob( uint8_t backend, int rate, idx_t channels,
                     const Fraction &stretchFrac, double pitchCents,
                     length_t grainSize, length_t crossfade,
                     uint64_t onsetsHash, uint64_t anchorsHash,
                     bool preserveFormants,
                     std::vector<uint8_t> &out )
{
    out.clear();
    out.reserve( 1 + 4 + 4 + 8 + 8 + 8 + 4 + 4 + 8 + 8 + 1 );
    auto put32 = [&]( uint32_t v ) {
        for( int i = 0; i < 4; i++ ) out.push_back( (uint8_t)( v >> ( 8 * i ) ) );
    };
    auto put64 = [&]( uint64_t v ) {
        for( int i = 0; i < 8; i++ ) out.push_back( (uint8_t)( v >> ( 8 * i ) ) );
    };
    out.push_back( backend );
    put32( (uint32_t) rate );
    put32( (uint32_t) channels );
    put64( (uint64_t) stretchFrac.numerator );
    put64( (uint64_t) stretchFrac.denominator );
    uint64_t centsBits;
    memcpy( &centsBits, &pitchCents, sizeof( centsBits ) );
    put64( centsBits );
    put32( (uint32_t) grainSize );
    put32( (uint32_t) crossfade );
    // M4: fingerprint of the onset keyframes baked into the warp (0 = none).
    // A warp built before the onsets sidecar existed and one built after
    // occupy DIFFERENT cache keys — availability can never alias bytes.
    put64( onsetsHash );
    // W1: fingerprint of the exact user warp anchors (0 = none) — a warp
    // with markers can never alias one without.
    put64( anchorsHash );
    // W4: the formant-preservation flag changes output bytes whenever the
    // pitch stage runs, so it is key material (v4 of the blob).
    out.push_back( preserveFormants ? 1 : 0 );
}

} // namespace

// M5 streaming state — see the threading note at readInto's definition.
struct twGrainSource::StreamState {
    std::shared_ptr<const twRandomSource> srcRef;   // co-owns the source PCM
    std::vector<const float *>            chans;    // borrowed planar views
    std::unique_ptr<twPagedVocoder>       voc;

    static constexpr uint64_t kBlock     = 65536;   // output frames per block
    static constexpr size_t   kMaxBlocks = 4;

    struct Block {
        uint64_t           start = (uint64_t) -1;
        std::vector<float> data;                    // planar channels × len
        uint64_t           lru = 0;
    };

    std::mutex         m;
    std::vector<Block> blocks;
    uint64_t           lruTick  = 0;
    uint32_t           channels = 0;
    uint64_t           outLen   = 0;

    void readInto( sample_t *dest, uint64_t start, uint64_t n, uint32_t ch );
};

twGrainSource::twGrainSource( const twRandomSource &src, const twGrainParams &p )
    : rate_( src.sampleRate() ),
      channels_( src.channels() ),
      nFrames_( 0 ),
      reproducible_( src.isReproducible() )
{
    length_t inLen = src.length();
    if( channels_ <= 0 || inLen <= 0 ) {
        if( channels_ < 0 ) channels_ = 0;
        return;
    }

    Fraction stretchFrac = ( p.stretch > Fraction(0) ) ? p.stretch
                                                       : Fraction(1, 1000000);
    double   stretch = stretchFrac.approxDouble();
    if( stretch < 1e-6 ) stretch = 1e-6;
    double   r       = pow( 2.0, p.pitchCents / 1200.0 );   // pitch ratio

    // Proposal 28 W1: THE conversion authority. No anchors -> pure scalar,
    // bit-identical to the historic floor(inLen*stretch) (twwarpmap_test
    // pins this); anchors -> piecewise map, total length from the map.
    const twWarpMap warpMap( p.warpAnchors, stretchFrac );
    nFrames_ = (length_t) warpMap.srcToWarpedFloor( inLen );
    // Anchors are a VOCODER capability: their presence forces that backend
    // regardless of TW_STRETCH_BACKEND (the RB escape hatch governs scalar
    // clips only — documented in CLAUDE.md).
    const int backend = p.warpAnchors.empty() ? stretchBackend()
                                              : (int) kBackendVocoder;
    if( nFrames_ <= 0 ) {
        nFrames_ = 0;
        return;
    }

    // --- Proposal 27 M2: durable warp cache -------------------------------
    // If the source is content-addressable and the store holds this exact
    // warp (same content, backend, geometry, exact stretch, pitch), load the
    // finished PCM and skip synthesis entirely — the load-stall fix. This is
    // strictly a byte cache: synthesis below is deterministic, so a stored
    // warp and a recomputed warp are identical (the M2 cmp gate).
    const twContentHash warpContent = src.contentHash();

    // M4: onset keyframes for the vocoder backend — read the M1 "onsets"
    // aspect (params-agnostic: the import job chose the detector params) and
    // map source-rate positions into this warp's input rate. Absence is
    // fine (fixed keyframe grid only); the onsets fingerprint is key
    // material below, so availability can never alias cached bytes.
    std::vector<uint64_t> onsetPos;
    uint64_t onsetsHash = 0;
    if( backend == kBackendVocoder && !warpContent.isNull() ) {
        auto onsetReader = twSidecarStore::instance().loadAny(
            warpContent, twAspect::Onsets, twAspect::OnsetsVersion );
        if( onsetReader && onsetReader->info().recordCount > 0
            && onsetReader->info().sourceRate > 0
            && onsetReader->info().recordStride == 12 ) {
            // v3 records: packed 12-byte {u64 pos, f32 salience} LE. The
            // vocoder's keyframes take EVERY entry (salience is the marker
            // UI's filter, not ours — dense keyframes are cheap and the
            // min-gap defense bounds them).
            const uint64_t n = onsetReader->info().recordCount;
            std::vector<uint8_t> raw( (size_t) n * 12 );
            if( onsetReader->readRecords( raw.data(), 0, n ) ) {
                const double scale =
                    (double) rate_ / (double) onsetReader->info().sourceRate;
                onsetPos.reserve( (size_t) n );
                for( uint64_t i = 0; i < n; i++ ) {
                    uint64_t pos = 0;
                    for( int b = 7; b >= 0; b-- )
                        pos = ( pos << 8 ) | raw[(size_t) i * 12 + b];
                    onsetPos.push_back( (uint64_t) std::llround( pos * scale ) );
                }
                onsetsHash = twHashBuffer(
                    onsetPos.data(),
                    onsetPos.size() * sizeof( uint64_t ) ).lo;
            }
        }
    }

    // W1: the vocoder's base map in the INTERNAL stretched domain —
    // (warped_i * pitchRatio, src_i) per anchor. Empty = uniform stretch.
    std::vector<std::pair<double, double>> userMapInternal;
    uint64_t anchorsHash = 0;
    if( !p.warpAnchors.empty() ) {
        userMapInternal.reserve( p.warpAnchors.size() );
        for( const twWarpAnchor &a : p.warpAnchors )
            userMapInternal.push_back(
                { (double) a.warped * r, (double) a.src } );
        anchorsHash = twHashBuffer(
            p.warpAnchors.data(),
            p.warpAnchors.size() * sizeof( twWarpAnchor ) ).lo;
    }

    // --- Proposal 27 M5: STREAMING mode ------------------------------------
    // Vocoder backend over a resident-planar, shared_ptr-owned source: no
    // materialized warp at all. The vocoder renders aligned output blocks on
    // demand inside read() (worker/freeze context — the RT thread reads
    // frozen pages, never this object), and a small LRU keeps recent blocks.
    // Memory is O(blocks), not O(clip × variants) — the Reaper-scale
    // property proposal 27 exists for. The shared handle co-owns the PCM, so
    // clip-content teardown during a queued freeze cannot dangle us (the
    // materialize paths were safe by copying; streaming must own instead).
    if( backend == kBackendVocoder ) {
        std::shared_ptr<const twRandomSource> ref = src.sharedRef();
        bool planar = ( ref != nullptr );
        for( idx_t c = 0; planar && c < channels_; ++c )
            planar = ( src.channelData( c ) != nullptr );
        if( planar ) {
            stream_.reset( new StreamState() );
            stream_->srcRef   = ref;
            stream_->channels = (uint32_t) channels_;
            stream_->outLen   = (uint64_t) nFrames_;
            stream_->chans.resize( (size_t) channels_ );
            for( idx_t c = 0; c < channels_; ++c )
                stream_->chans[(size_t) c] = src.channelData( c );
            twPagedVocoder::Config vc;
            vc.fftSize     = 2048;
            vc.analysisHop = 512;
            vc.rate        = rate_;
            vc.channels    = (uint32_t) channels_;
            vc.userMap     = userMapInternal;
            vc.preserveFormants = p.preserveFormants;   // W4 opt-in
            stream_->voc.reset( new twPagedVocoder(
                stream_->chans.data(), (uint64_t) inLen, vc, stretch, r,
                onsetPos.empty() ? nullptr : onsetPos.data(),
                onsetPos.size() ) );
            TW_LOGD( "sources",
                     "twGrainSource: streaming warp (%lld out frames, no residency)",
                     (long long) nFrames_ );
            return;   // no data_, no warp.pcm — pages render on demand
        }
    }

    data_.assign( (size_t) channels_ * nFrames_, 0.0f );

    std::vector<uint8_t> warpParams;
    uint64_t warpParamsHash = 0;
    const bool warpCacheable = !warpContent.isNull()
                            && src.isReproducible()
                            && twSidecarStore::instance().enabled();
    if( warpCacheable ) {
        warpParamsBlob( (uint8_t) backend, rate_, channels_, stretchFrac,
                        p.pitchCents, p.grainSize, p.crossfade, onsetsHash,
                        anchorsHash, p.preserveFormants, warpParams );
        warpParamsHash = twSidecarStore::hashParams( warpParams.data(),
                                                     warpParams.size() );
        auto reader = twSidecarStore::instance().load(
            warpContent, twAspect::WarpPcm, twAspect::WarpPcmVersion,
            warpParamsHash );
        if( reader ) {
            const twQafInfo &qi = reader->info();
            if( qi.sourceRate == (uint32_t) rate_
                && qi.channels == (uint32_t) channels_
                && qi.sourceFrames == (uint64_t) nFrames_
                && qi.payloadLen == (uint64_t) data_.size() * sizeof( sample_t )
                && reader->readPayload( data_.data(), 0, qi.payloadLen ) ) {
                TW_LOGD( "sources", "twGrainSource: warp.pcm hit (%lld frames)",
                         (long long) nFrames_ );
                return;   // finished warp adopted; no synthesis
            }
            // Geometry mismatch or short read: fall through and recompute
            // (the fresh store below overwrites the bad entry).
        }
    }

    if( backend == kBackendVocoder ) {
        // --- In-house phase vocoder (proposal 27 M3, opt-in) -----------------
        // Offline whole-signal mode; identity phase-locking, cross-channel
        // rotation, exact-outLen contract identical to the paths below. data_
        // is planar channels_ × nFrames_ contiguous — exactly warpOffline's
        // output layout.
        std::vector<std::vector<float>> inCh( (size_t) channels_ );
        std::vector<const float*>       inPtr( (size_t) channels_ );
        for( idx_t c = 0; c < channels_; ++c ) {
            inCh[(size_t) c].resize( (size_t) inLen );
            src.read( 0, inCh[(size_t) c].data(), inLen, c );
            inPtr[(size_t) c] = inCh[(size_t) c].data();
        }
        twPagedVocoder::Config vc;
        vc.fftSize     = 2048;
        vc.analysisHop = 512;
        vc.rate        = rate_;
        vc.channels    = (uint32_t) channels_;
        vc.userMap     = userMapInternal;
        vc.preserveFormants = p.preserveFormants;   // W4 opt-in
        twPagedVocoder::warpOffline( inPtr.data(), (uint64_t) inLen,
                                     data_.data(), (uint64_t) nFrames_,
                                     vc, stretch, r,
                                     onsetPos.empty() ? nullptr : onsetPos.data(),
                                     onsetPos.size() );
    } else {
    // --- Legacy time-slice overlap-add fallback (proposal 06) ----------------
    // Fixed grain size / crossfade, one stretch, one pitch. The grain
    // scheduling (hop spacing, per-sample interpolation) is synthesis-internal
    // double math and never feeds back into position math.
    length_t G = ( p.grainSize > 0 ) ? p.grainSize : 2048;
    length_t C = p.crossfade;
    if( C < 0 ) C = 0;
    if( C > G / 2 ) C = G / 2;            // need G >= 2C for unity crossfade
    length_t Ho = G - C;                  // output hop
    if( Ho <= 0 ) Ho = 1;
    double   Hi = (double) Ho / stretch;  // input hop (frames)

    std::vector<sample_t> in( (size_t) inLen );
    // Window-weight accumulator, reused per channel: normalising by it makes the
    // overlap-add unity-gain everywhere there is coverage, regardless of how the
    // fades line up at clip edges.
    std::vector<float> wsum( (size_t) nFrames_ );

    for( idx_t c = 0; c < channels_; ++c ) {
        src.read( 0, in.data(), inLen, c );
        sample_t *out = data_.data() + (size_t) c * nFrames_;
        memset( wsum.data(), 0, sizeof( float ) * (size_t) nFrames_ );

        for( length_t g = 0; ; ++g ) {
            length_t outPos = g * Ho;
            if( outPos >= nFrames_ ) break;
            double inPos = (double) g * Hi;

            for( length_t j = 0; j < G; ++j ) {
                length_t op = outPos + j;
                if( op >= nFrames_ ) break;

                double   sp = inPos + (double) j * r;   // input position for this grain sample
                if( sp < 0.0 ) continue;
                length_t k    = (length_t) sp;
                double   frac = sp - (double) k;
                sample_t a = ( k < inLen )       ? in[(size_t) k]         : 0.0f;
                sample_t b = ( k + 1 < inLen )   ? in[(size_t) ( k + 1 )] : a;
                sample_t s = (sample_t) ( a + ( b - a ) * frac );

                double w;
                if( C > 0 && j < C )            w = (double) j / (double) C;          // fade in
                else if( C > 0 && j >= G - C )  w = (double) ( G - j ) / (double) C;  // fade out
                else                            w = 1.0;

                out[op]  += (sample_t) ( s * w );
                wsum[op] += (float) w;
            }
        }

        for( length_t i = 0; i < nFrames_; ++i ) {
            if( wsum[i] > 1e-4f ) out[i] /= (sample_t) wsum[i];
        }
    }
    }   // backend dispatch (vocoder vs Rubber Band / legacy OLA)

    // Proposal 27 M2: persist the freshly synthesized warp for the next load.
    // A failed store just means the next session recomputes — never fatal.
    if( warpCacheable ) {
        twQafInfo qi;
        qi.aspectId      = twAspect::WarpPcm;
        qi.aspectVersion = twAspect::WarpPcmVersion;
        qi.contentHash   = warpContent;
        qi.sourceRate    = (uint32_t) rate_;
        qi.channels      = (uint32_t) channels_;
        qi.sourceFrames  = (uint64_t) nFrames_;    // OUTPUT frames (exact length)
        qi.recordStride  = sizeof( sample_t );
        qi.recordCount   = (uint64_t) data_.size();
        qi.hopFrames     = 0;
        qi.params        = warpParams;
        twSidecarStore::instance().store(
            qi, data_.data(), (uint64_t) data_.size() * sizeof( sample_t ) );
    }
}

twGrainSource::~twGrainSource()
{
}

length_t twGrainSource::read( offset_t srcOffset, sample_t *dest,
                              length_t len, idx_t channel ) const
{
    if( len <= 0 ) return 0;
    if( nFrames_ <= 0 || channels_ <= 0 ) {
        memset( dest, 0, sizeof( sample_t ) * len );
        return 0;
    }

    idx_t ch = channel;
    if( ch < 0 ) ch = 0;
    if( ch >= channels_ ) ch = channels_ - 1;

    // Leading silence for a read that starts before the material — same rule
    // (and same latent out-of-bounds without it) as twSampleSource::read.
    if( srcOffset < 0 ) {
        length_t lead = (length_t)( -srcOffset );
        if( lead > len ) lead = len;
        memset( dest, 0, sizeof( sample_t ) * lead );
        dest += lead;
        len  -= lead;
        srcOffset = 0;
        if( len <= 0 ) return 0;
    }

    length_t avail = 0;
    if( srcOffset < (offset_t) nFrames_ ) {
        avail = nFrames_ - (length_t) srcOffset;
    }
    length_t n = len;
    if( n > avail ) n = avail;
    if( n < 0 ) n = 0;

    if( n > 0 ) {
        if( stream_ ) {
            stream_->readInto( dest, (uint64_t) srcOffset, (uint64_t) n,
                               (uint32_t) ch );
        } else {
            const sample_t *src = data_.data() + (size_t) ch * nFrames_ + srcOffset;
            memcpy( dest, src, sizeof( sample_t ) * n );
        }
    }
    if( n < len ) {
        memset( dest + n, 0, sizeof( sample_t ) * ( len - n ) );
    }
    return n;
}

// ---------------------------------------------------------------------------
// M5 streaming state: aligned-block LRU over on-demand vocoder renders.
// Thread-safe (freeze workers race on shared grains); NEVER called on the RT
// thread — RT reads frozen pages only (proposal 16/19 architecture).
// ---------------------------------------------------------------------------
void twGrainSource::StreamState::readInto( sample_t *dest, uint64_t start,
                                           uint64_t n, uint32_t ch )
{
    std::lock_guard<std::mutex> lock( m );
    uint64_t pos = start;
    while( pos < start + n ) {
        const uint64_t bStart = ( pos / kBlock ) * kBlock;
        const uint64_t bLen   = std::min( kBlock, outLen - bStart );

        Block *blk = nullptr;
        for( Block &b : blocks )
            if( b.start == bStart ) { blk = &b; break; }
        if( !blk ) {
            if( blocks.size() >= kMaxBlocks ) {
                // Evict the least-recently-used block.
                size_t victim = 0;
                for( size_t i = 1; i < blocks.size(); i++ )
                    if( blocks[i].lru < blocks[victim].lru ) victim = i;
                blocks[victim] = Block();
                blk = &blocks[victim];
            } else {
                blocks.emplace_back();
                blk = &blocks.back();
            }
            blk->start = bStart;
            blk->data.assign( (size_t) channels * bLen, 0.0f );
            voc->render( bStart, bLen, blk->data.data() );
        }
        blk->lru = ++lruTick;

        const uint64_t off   = pos - bStart;
        const uint64_t avail = bLen - off;
        const uint64_t take  = std::min( avail, start + n - pos );
        memcpy( dest + ( pos - start ),
                blk->data.data() + (size_t) ch * bLen + off,
                sizeof( sample_t ) * (size_t) take );
        pos += take;
    }
}
