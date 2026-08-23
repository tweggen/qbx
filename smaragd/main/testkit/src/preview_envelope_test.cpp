// Gate for the COLLECT SEAM (proposal 39 M1, D1/D2): the probes a renderer
// hands out through SObjectRenderer::collectEnvelope() are the probes it DRAWS.
//
// The seam exists because two consumers now want the same envelope — the
// arranger's painter, and everything that must READ a waveform without a
// QPainter (the assert-envelope verb here, the folder-sum overlay in M3). The
// only way that stays honest is one probe-producing path, and the only way to
// KEEP it honest is a gate that measures the two ends against each other.
//
// So the properties pinned here are exactly the ones a second implementation of
// the mapping would get wrong, and that a screenshot could never localise:
//
//   1. THE LINK-START FOLD. A window is expressed in the caller's own time
//      domain; the object's origin is lk.getStartTime(). Getting that wrong
//      draws the right waveform in the wrong place — and, for a probe-reading
//      caller, silently answers about a different part of the clip.
//   2. THE CUT'S SLIP. SCut owns the clip->source map (srcStart, stretch, warp
//      anchors). It is asserted twice: against the PAINTED result, and against
//      a closed form (at unity stretch, slipping a clip by X is the same as
//      asking an unslipped one about a window X later).
//   3. LOOP TILING. Each repetition must fill ITS OWN pixel span. The classic
//      failure is a collect that returns tile 0's probes for the whole width —
//      which is periodic-looking nonsense, so the gate asserts (a) the painted
//      array column for column, (b) that the array is PERIODIC in pixel space
//      with the tile period, and (c) that it is NOT the single-linear-pass
//      array a broken implementation produces. (c) is the direct discriminator;
//      (b) alone would pass on a flat array, so the material inside one tile is
//      asserted to vary as well.
//   4. THE DEFAULT IS false AND WRITES NOTHING. An object with no waveform (an
//      event clip) contributes nothing — asserted with a sentinel, not assumed,
//      because "returns false" and "returns false having already scribbled" are
//      the same thing to a caller that ignores the bool.
//
// HOW THE PAINTED SIDE IS READ BACK. drawObjectWaveform draws, per column,
// one vertical line between
//     y1 = top + ((127 - min) * height) / 256
//     y2 = top + ((127 - max) * height) / 256
// so at top = 0, height = 256 the transform is exactly y = 127 - value. Paint
// into a 256-tall QImage in a colour nothing else in the renderer uses, and the
// topmost / bottom-most pixel of that colour in a column ARE max and min. That
// is a genuinely independent read of the draw path, not a re-run of the collect.
// The fixture keeps |value| <= 100 so the loop-marker GRAB HANDLES, which the
// cut renderer paints over the top ~13 rows of every loop boundary, can never
// eat a wave pixel.
//
// WHY THE LEAF RENDERER IS A FIXTURE AND NOT SPlainWaveRendererInline. A real
// SPlainWave needs SPlainWave::setWave(), which needs an SAppContext with a
// CURRENT PROJECT and a decodable file on disk. The fixture leaf below is a
// structural clone — the same two one-line bodies — so what this test gates is
// the SEAM and the SCut mapping above it. The shipped leaf renderers are
// exercised end to end by the qxa case envelope_probe.qxa.
//
// No project, no audio device, no display.

#include "app/model/sappcontext.h"
#include "app/model/sclipwindow.h"
#include "app/model/sexternfile.h"
#include "app/model/sobject.h"
#include "app/model/sobjectrenderer.h"
#include "app/model/slink.h"
#include "app/model/sproject.h"
#include "app/objects/cut/scut.h"
#include "app/objects/cut/scutrndrinline.h"
#include "app/objects/track/strack.h"
#include "app/objects/track/strackrndrinline.h"
#include "app/objects/wave/swaveformdraw.h"

#include "tw/core/twtypes.h"
#include "tw/graph/tw303aenv.h"
#include "tw/sources/twrandomsource.h"

#include <QApplication>
#include <QColor>
#include <QFontMetrics>
#include <QImage>
#include <QPainter>
#include <QRect>

#include <cstdio>
#include <memory>
#include <vector>

namespace {

int g_failures = 0;

void check( bool ok, const char *what )
{
    if( ok ) { std::printf( "ok   %s\n", what ); return; }
    std::printf( "FAIL %s\n", what );
    ++g_failures;
}

// --------------------------------------------------------------- the fixture

// A position-identifying probe: one plateau per 1000 source frames, 101
// distinct values, kept inside +-100 so the loop-marker handles the cut
// renderer paints over the top rows can never clip a wave pixel (see header).
preview_t probeAt( offset_t pos, length_t duration )
{
    preview_t p;
    p.min = 0;
    p.max = 0;
    if( pos < 0 || pos >= (offset_t) duration ) return p;   // past the material
    const long long k = (long long)( pos / 1000 );
    const int a = (int)( k % 81 );                          // 0..80
    p.max = (previewPart_t)( 20 + a );                      // +20 .. +100
    p.min = (previewPart_t)( -20 - a );                     // -20 .. -100
    return p;
}

// The minimum a twRandomSource has to be for SCut to treat its content as
// SAMPLE-backed rather than as a container capture (cutIsContainer() null-checks
// it and nothing on the paint path dereferences it).
class StubSource : public twRandomSource
{
public:
    explicit StubSource( length_t len ) : len_( len ) {}
    length_t read( offset_t, sample_t *dest, length_t n, idx_t ) const override
    {
        for( length_t i = 0; i < n; ++i ) dest[i] = 0.0f;
        return n;
    }
    length_t length()     const override { return len_; }
    idx_t    channels()   const override { return 1; }
    int      sampleRate() const override { return 48000; }
    bool     isReproducible() const override { return true; }
private:
    length_t len_;
};

class LeafRenderer;

// The CONTENT: a duration, a random source, a synthetic preview whose value
// identifies the position it was read at, and an inline renderer.
class SourceObject : public SObject
{
public:
    SourceObject( length_t duration );
    ~SourceObject() override;

    std::shared_ptr<twComponent> getRootComponent() override { return nullptr; }
    bool      hasDuration() const override { return true; }
    length_t  getDuration() const override { return duration_; }
    bool      hasPreview()  const override { return true; }
    twRandomSource *getRandomSource() override { return &source_; }

    // Overridden so the whole test is a closed form: no page freeze, no
    // sidecar, and a probe that names the source position it came from. The
    // per-column position rule is getStraightPreview()'s own, so a caller that
    // folds the window correctly gets exactly these bytes.
    int getPreview( preview_t *dest, offset_t start, length_t length,
                    offset_t nProbes ) override
    {
        if( nProbes <= 0 || !dest ) return -1;
        if( length < 1 ) length = 1;
        for( offset_t i = 0; i < nProbes; ++i )
            dest[i] = probeAt( start + ( i * length ) / nProbes, duration_ );
        return 0;
    }

    QWidget *getDetailEditWidget( QWidget * ) override { return nullptr; }
    QWidget *getInlineEditWidget( QWidget * ) override { return nullptr; }
    SObjectRenderer *getInlineRenderer() override;

private:
    length_t     duration_;
    StubSource   source_;
    LeafRenderer *renderer_ = nullptr;
};

// A structural clone of SPlainWaveRendererInline's two bodies (see header for
// why the shipped one cannot be instantiated here): draw is the shared draw
// loop, collect is the shared collect, and nothing else.
const QColor WAVE_COLOR( 0, 255, 0 );

class LeafRenderer : public SObjectRenderer
{
public:
    explicit LeafRenderer( SObject &o ) : SObjectRenderer( o ) {}
    void draw( SLink &lk, SRenderContext &ctx ) override
    { drawObjectWaveform( getObject(), lk, ctx, WAVE_COLOR ); }
    bool collectEnvelope( SLink &lk, const SEnvelopeWindow &win,
                          preview_t *out ) override
    { return collectObjectEnvelope( getObject(), lk, win, out ); }
};

SourceObject::SourceObject( length_t duration )
    : SObject( nullptr ), duration_( duration ), source_( duration )
{
}
SourceObject::~SourceObject() { delete renderer_; }

SObjectRenderer *SourceObject::getInlineRenderer()
{
    if( !renderer_ ) renderer_ = new LeafRenderer( *this );
    return renderer_;
}

// AC M1.4: an object whose renderer draws but does NOT implement the collect —
// the shape of an event clip. It must inherit the base default.
class DrawOnlyRenderer : public SObjectRenderer
{
public:
    explicit DrawOnlyRenderer( SObject &o ) : SObjectRenderer( o ) {}
    void draw( SLink &, SRenderContext & ) override {}
};

class DrawOnlyObject : public SObject
{
public:
    DrawOnlyObject() : SObject( nullptr ) {}
    ~DrawOnlyObject() override { delete renderer_; }
    std::shared_ptr<twComponent> getRootComponent() override { return nullptr; }
    bool     hasDuration() const override { return true; }
    length_t getDuration() const override { return 48000; }
    QWidget *getDetailEditWidget( QWidget * ) override { return nullptr; }
    QWidget *getInlineEditWidget( QWidget * ) override { return nullptr; }
    SObjectRenderer *getInlineRenderer() override
    {
        if( !renderer_ ) renderer_ = new DrawOnlyRenderer( *this );
        return renderer_;
    }
private:
    DrawOnlyRenderer *renderer_ = nullptr;
};

// ------------------------------------------------------- the render context

// A linear pixel -> time map, which is what every arranger context is: the draw
// path probes it at exactly two x positions and interpolates between them.
class FlatContext : public SRenderContext
{
public:
    FlatContext( QPainter &p, int x0, offset_t t0, double sppFrames )
        : SRenderContext( p ), x0_( x0 ), t0_( t0 ), spp_( sppFrames ) {}
    offset_t getTimeOf( int x ) const override
    { return t0_ + (offset_t) ( (double)( x - x0_ ) * spp_ ); }
private:
    int      x0_;
    offset_t t0_;
    double   spp_;
};

// ------------------------------------------------------- the painted read-back

// Paint `rndr` over `win` and recover the probes column by column out of the
// pixels (see the header for why y = 127 - value here). `covered[i]` says
// whether column i had any wave pixel at all.
void paintAndRecover( SObjectRenderer &rndr, SLink &lk, int x0, offset_t t0,
                      double spp, int width,
                      std::vector<preview_t> &out, std::vector<bool> &covered )
{
    const int H = 256;
    QImage img( x0 + width, H, QImage::Format_RGB32 );
    img.fill( QColor( 0, 0, 0 ) );
    {
        QPainter p( &img );
        FlatContext ctx( p, x0, t0, spp );
        ctx.setVisibRect( QRect( x0, 0, width, H ) );
        rndr.draw( lk, ctx );
    }

    out.assign( (size_t) width, preview_t{ 0, 0 } );
    covered.assign( (size_t) width, false );
    const QRgb want = WAVE_COLOR.rgb();
    for( int i = 0; i < width; ++i ) {
        int top = -1, bottom = -1;
        for( int y = 0; y < H; ++y ) {
            if( ( img.pixel( x0 + i, y ) & 0x00ffffff ) != ( want & 0x00ffffff ) )
                continue;
            if( top < 0 ) top = y;
            bottom = y;
        }
        if( top < 0 ) continue;
        covered[i] = true;
        out[i].max = (previewPart_t)( 127 - top );
        out[i].min = (previewPart_t)( 127 - bottom );
    }
}

bool sameProbes( const std::vector<preview_t> &a, const std::vector<preview_t> &b )
{
    if( a.size() != b.size() ) return false;
    for( size_t i = 0; i < a.size(); ++i )
        if( a[i].min != b[i].min || a[i].max != b[i].max ) return false;
    return true;
}

int firstDifference( const std::vector<preview_t> &a,
                     const std::vector<preview_t> &b, int from = 0 )
{
    const size_t n = a.size() < b.size() ? a.size() : b.size();
    for( size_t i = (size_t) from; i < n; ++i )
        if( a[i].min != b[i].min || a[i].max != b[i].max ) return (int) i;
    return a.size() == b.size() ? -1 : (int) n;
}

std::vector<preview_t> collectThrough( SObjectRenderer &rndr, SLink &lk,
                                       offset_t left, offset_t right, int width,
                                       bool &ok )
{
    std::vector<preview_t> out( (size_t) width, preview_t{ 0, 0 } );
    SEnvelopeWindow win;
    win.leftTime  = left;
    win.rightTime = right;
    win.width     = width;
    ok = rndr.collectEnvelope( lk, win, out.data() );
    return out;
}

}  // namespace

int main( int argc, char **argv )
{
    QApplication app( argc, argv );

    // ================================================== 1. the plain object
    //
    // The window is expressed in TIMELINE frames and the object's origin is the
    // link start, so a collect over [S+5000, S+5000+L) must be the object's own
    // preview of [5000, 5000+L).
    {
        const length_t DUR = 240000;              // 5 s at 48 k
        const offset_t S   = 100000;              // a NON-zero link start
        const int      W   = 64;
        const length_t L   = 64000;
        const offset_t OFF = 5000;

        SourceObject obj( DUR );
        SLink lk( obj, nullptr );
        lk.setStartTime( S );

        bool ok = false;
        std::vector<preview_t> got = collectThrough(
            *obj.getInlineRenderer(), lk, S + OFF, S + OFF + L, W, ok );
        check( ok, "a plain object's renderer collects an envelope" );

        std::vector<preview_t> want( (size_t) W, preview_t{ 0, 0 } );
        check( obj.getPreview( want.data(), OFF, L, W ) >= 0,
               "…and the fixture answers the same question directly" );
        check( sameProbes( got, want ),
               "the collect folds the LINK START: [S+off, S+off+L) is the "
               "object's own [off, off+L)" );

        // The material really does vary over the window, or the equality above
        // would be satisfied by two identical flat arrays.
        int distinct = 0;
        for( int i = 1; i < W; ++i )
            if( got[i].max != got[i-1].max ) ++distinct;
        check( distinct >= 8,
               "…over material that varies (so the comparison is not vacuous)" );

        // And the SAME window, drawn: collect and draw cannot drift.
        std::vector<preview_t> painted;
        std::vector<bool> covered;
        paintAndRecover( *obj.getInlineRenderer(), lk, 7, S + OFF,
                         (double) L / (double) W, W, painted, covered );
        check( sameProbes( painted, got ),
               "the DRAWN pixels carry exactly the collected probes" );
    }

    // ============================================ 2. an SCut with a slip
    //
    // At unity stretch the clip->source map is `src = srcStart + rel`, so
    // slipping a clip by X must be indistinguishable from asking an unslipped
    // one about a window X later. Asserted against that closed form AND against
    // the painted pixels, because the two can fail independently: the closed
    // form catches a wrong map, the pixels catch a collect that maps correctly
    // and then hands the caller a differently-shaped array.
    {
        const length_t DUR  = 480000;
        const offset_t S    = 100000;
        const offset_t SLIP = 33000;
        const int      W    = 64;
        const length_t L    = 96000;

        SourceObject obj( DUR );

        SCut slipped( nullptr, obj );
        slipped.setSrcStartRaw( Fraction( (int64_t) SLIP ) );
        slipped.setDurationRaw( ClipLen( 200000 ) );
        SLink lkA( slipped, nullptr );
        lkA.setStartTime( S );

        SCut plain( nullptr, obj );
        plain.setDurationRaw( ClipLen( 200000 ) );
        SLink lkB( plain, nullptr );
        lkB.setStartTime( S );

        check( !slipped.isLooping() && !plain.isLooping(),
               "the slip fixtures are NOT looping" );

        bool okA = false, okB = false;
        std::vector<preview_t> gotSlipped = collectThrough(
            *slipped.getInlineRenderer(), lkA, S, S + L, W, okA );
        std::vector<preview_t> gotShifted = collectThrough(
            *plain.getInlineRenderer(), lkB, S + SLIP, S + SLIP + L, W, okB );
        check( okA && okB, "both cuts collect an envelope" );
        check( sameProbes( gotSlipped, gotShifted ),
               "a cut's SLIP is folded: slip X == an unslipped cut X later" );

        // The slip is doing something: the same window on the unslipped cut is
        // a different envelope.
        bool okC = false;
        std::vector<preview_t> gotUnslipped = collectThrough(
            *plain.getInlineRenderer(), lkB, S, S + L, W, okC );
        check( okC && !sameProbes( gotSlipped, gotUnslipped ),
               "…and the slip is not a no-op (the unslipped window differs)" );

        std::vector<preview_t> painted;
        std::vector<bool> covered;
        paintAndRecover( *slipped.getInlineRenderer(), lkA, 7, S,
                         (double) L / (double) W, W, painted, covered );
        check( sameProbes( painted, gotSlipped ),
               "a slipped cut DRAWS exactly what it collects" );
    }

    // ==================================================== 3. a LOOPED SCut
    //
    // Geometry chosen so the tiling is exact in pixels: 500 frames per pixel,
    // a 10000-frame loop segment = 20 px per tile, and a clip that starts 10 px
    // into the window. Tiles therefore occupy columns 10-29, 30-49, 50-69, …
    // and columns 0-9 are covered by nothing at all.
    {
        const length_t DUR     = 480000;
        const offset_t T0      = 200000;         // window start, timeline
        const double   SPP     = 500.0;          // frames per pixel
        const int      W       = 120;
        const length_t L       = (length_t)( SPP * W );
        const offset_t S       = T0 + 5000;      // clip start -> column 10
        const length_t SEGLEN  = 10000;          // -> 20 px per tile
        const int      SEGPX   = 20;
        const int      LEFTPX  = 10;

        SourceObject obj( DUR );
        SCut cut( nullptr, obj );
        cut.setDurationRaw( ClipLen( 5 * SEGLEN ) );
        cut.setLoopLengthRaw( WarpedLen( (int64_t) SEGLEN ) );
        SLink lk( cut, nullptr );
        lk.setStartTime( S );

        check( cut.isLooping(), "the loop fixture IS looping" );

        bool ok = false;
        std::vector<preview_t> got = collectThrough(
            *cut.getInlineRenderer(), lk, T0, T0 + L, W, ok );
        check( ok, "a looping cut collects an envelope" );

        // (a) column for column against the PAINT.
        std::vector<preview_t> painted;
        std::vector<bool> covered;
        paintAndRecover( *cut.getInlineRenderer(), lk, 7, T0, SPP, W,
                         painted, covered );
        int mismatch = -1;
        for( int i = 0; i < W; ++i ) {
            if( !covered[i] ) continue;          // no tile paints here
            if( painted[i].min == got[i].min && painted[i].max == got[i].max )
                continue;
            mismatch = i;
            break;
        }
        if( mismatch >= 0 ) {
            std::printf( "FAIL a looping cut DRAWS exactly what it collects "
                         "(column %d: drawn %d/%d, collected %d/%d)\n",
                         mismatch, (int) painted[mismatch].min,
                         (int) painted[mismatch].max,
                         (int) got[mismatch].min, (int) got[mismatch].max );
            ++g_failures;
        } else {
            check( true, "a looping cut DRAWS exactly what it collects" );
        }

        // Columns before the clip belong to no tile: the collect leaves them
        // silent rather than inheriting the caller's buffer.
        bool leftSilent = true;
        for( int i = 0; i < LEFTPX; ++i )
            if( got[i].min != 0 || got[i].max != 0 ) leftSilent = false;
        check( leftSilent,
               "columns no tile covers are SILENT, not the caller's junk" );

        // (b) PERIODIC in pixel space with the tile period — which is what
        // "every repetition filled its own span, correctly anchored" means.
        bool periodic = true;
        for( int j = 0; j < SEGPX; ++j ) {
            const int c1 = LEFTPX + SEGPX + j;        // tile 1
            const int c2 = LEFTPX + 2 * SEGPX + j;    // tile 2
            if( got[c1].min != got[c2].min || got[c1].max != got[c2].max )
                periodic = false;
        }
        check( periodic, "tile 1 and tile 2 hold the SAME 20 columns" );

        // …and the material inside one tile varies, so (b) is not satisfied by
        // a flat array.
        int distinct = 0;
        for( int j = 1; j < SEGPX; ++j ) {
            const int c = LEFTPX + SEGPX + j;
            if( got[c].max != got[c-1].max ) ++distinct;
        }
        check( distinct >= 5,
               "…over material that varies inside a tile" );

        // (c) THE DIRECT DISCRIMINATOR. What a collect that ignored the tiling
        // and made ONE linear pass over the whole width would produce — i.e.
        // "tile 0 for everything". It must not be what we got.
        bool okNaive = false;
        SCut naive( nullptr, obj );
        naive.setDurationRaw( ClipLen( 5 * SEGLEN ) );   // NOT looping
        SLink lkN( naive, nullptr );
        lkN.setStartTime( S );
        check( !naive.isLooping(), "the single-pass control is not looping" );
        std::vector<preview_t> single = collectThrough(
            *naive.getInlineRenderer(), lkN, T0, T0 + L, W, okNaive );
        // Compared INSIDE the tiled region: outside it the two trivially
        // differ (the loop leaves it silent, the single pass clamps into it),
        // which would make the discriminator agree for the wrong reason.
        const int diffAt = firstDifference( got, single, LEFTPX );
        check( okNaive && diffAt >= 0,
               "the looped envelope is NOT the single-linear-pass one, "
               "INSIDE the tiled region (the tiling happened)" );
        std::printf( "     (looped vs single-pass first differ at column %d)\n",
                     diffAt );
    }

    // ============================== 4. a renderer that does not implement it
    {
        DrawOnlyObject obj;
        SLink lk( obj, nullptr );
        lk.setStartTime( 0 );

        const int W = 16;
        const preview_t SENTINEL = { -7, 42 };
        std::vector<preview_t> buf( (size_t) W, SENTINEL );

        SEnvelopeWindow win;
        win.leftTime  = 0;
        win.rightTime = 48000;
        win.width     = W;
        const bool ok = obj.getInlineRenderer()->collectEnvelope(
            lk, win, buf.data() );
        check( !ok, "a renderer with no collect returns FALSE" );

        bool untouched = true;
        for( int i = 0; i < W; ++i )
            if( buf[i].min != SENTINEL.min || buf[i].max != SENTINEL.max )
                untouched = false;
        check( untouched, "…and writes NOTHING into the output buffer" );
    }

    // =========== 5. THE LANE'S FADER NEVER SCALES THE DRAWN WAVEFORM (M2)
    //
    // THE RULE (proposal 39): a drawn waveform describes the audio its object
    // PRODUCES; the lane it is drawn on never scales it. drawObjectWaveform
    // used to read the containing track's fader - dynamic_cast<SObject*> on
    // lk.parent(), which is the STrack in every path that creates a clip link -
    // and multiply every probe by it, so pulling a fader down redrew the
    // arrangement thinner and at -40 dB emptied it.
    //
    // THIS IS THE GATE THAT BITES, and it has to be a PIXEL gate. The collect
    // half never carried the multiply (M1 left it in the draw half alone,
    // deliberately, so that M1 was verifiable as behaviour-preserving), so
    // everything that reads through collectEnvelope - assert-envelope,
    // preview_volume_independent.qxa, M3's folder walk - is blind to it by
    // construction. Only the painted pixels can see it, and the read-back above
    // is a genuinely independent look at them.
    //
    // Verified to FAIL before the deletion, in both directions: at -20 dB the
    // painted column read 2/-2 where the collect said 20/-20, and at +6 dB the
    // tall material clamped at 127.
    {
        const length_t DUR = 240000;
        const offset_t S   = 100000;
        const int      W   = 64;
        const length_t L   = 64000;

        // The CONTAINER: what an STrack is to a clip link as far as the draw
        // path is concerned - the link's QObject parent, dynamic_cast to
        // SObject for its fader. Attached with setParent() rather than through
        // the ctor, per slink.h's construction rule.
        SourceObject container( DUR );
        SourceObject obj( DUR );
        SLink lk( obj, nullptr );
        lk.setStartTime( S );
        lk.setParent( &container );
        check( dynamic_cast<SObject *>( lk.parent() ) == &container,
               "the fixture link's parent IS the container the draw path read" );

        bool ok = false;
        std::vector<preview_t> got = collectThrough(
            *obj.getInlineRenderer(), lk, S, S + L, W, ok );
        check( ok, "the contained object collects an envelope" );

        // The material is TALL, so any gain but unity has to move it: a probe
        // of 20..100 scaled by -20 dB lands on 2..10, and by +6 dB on 40..200
        // (which then clamps). Without this the comparison could pass on a flat
        // array that no gain could have changed.
        int tall = 0;
        for( int i = 0; i < W; ++i ) if( got[i].max >= 20 ) ++tall;
        check( tall >= W / 2,
               "...over material tall enough that a gain would be visible" );

        const double DBS[] = { 0.0, -20.0, -6.0, 6.0, -60.0 };
        for( double db : DBS ) {
            container.setVolume( db );
            std::vector<preview_t> painted;
            std::vector<bool> covered;
            paintAndRecover( *obj.getInlineRenderer(), lk, 7, S,
                             (double) L / (double) W, W, painted, covered );
            char what[160];
            std::snprintf( what, sizeof( what ),
                           "the DRAWN pixels are the collected probes with the "
                           "container's fader at %+g dB", db );
            const bool same = sameProbes( painted, got );
            check( same, what );
            if( !same ) {
                const int at = firstDifference( painted, got );
                if( at >= 0 )
                    std::printf( "     (first differs at column %d: painted "
                                 "%d/%d, collected %d/%d)\n", at,
                                 (int) painted[at].min, (int) painted[at].max,
                                 (int) got[at].min, (int) got[at].max );
            }
        }

        // Detach before the fixtures unwind. Declaration order already makes it
        // safe; saying it is cheaper than relying on it.
        lk.setParent( nullptr );
    }

    // ===== 6. PROPOSAL 41 M5 — DETERMINISTIC Z-ORDER AND MATERIAL PAINTING
    //
    // D11: paint order is start-time ASCENDING (latest start ON TOP),
    // tiebreak child index then object id — never childLinks()' insertion
    // order, which is arbitrary. D10: a clip's BODY (the opaque grey rect
    // STrackRendererInline used to fill for the WHOLE window before the
    // inner renderer draws) is now clipped to where
    // SObjectRenderer::collectEnvelope() reports MATERIAL (min!=0 || max!=0
    // — the same "silence draws nothing" proxy drawChildSumOverlay() already
    // uses, proposal 39 M3's discipline), so a later clip's GAP leaves an
    // earlier clip's material on screen instead of erasing it.
    //
    // THIS HAS TO BE A PIXEL GATE (proposal 41 trap 6, mirroring proposal 39
    // M2's finding): a script-level gate through collectEnvelope sits BELOW
    // the paint and cannot see a paint-order or paint-clip bug, so this
    // section calls the REAL STrackRendererInline::draw() into a QImage and
    // reads colours back out of it — the same technique paintAndRecover()
    // above uses, generalized to more than one colour so "which clip's
    // pixels survived" is a plain RGB search. It runs over a REAL STrack
    // with REAL SLink children, because AC5.1/5.2/5.3 are about that
    // renderer's ACTUAL paint order and ACTUAL clipping, not a
    // reimplementation of them.
    //
    // No SLaneFragment placement exists yet to build a real disjoint
    // fragment (proposal 41 M2 is a parallel, not-yet-landed milestone), so
    // — exactly as this milestone's own task text prescribes — a clip with
    // an internal silent stretch stands in for the "gap" a container-backed
    // cut over a partially-filled folder track would show. Verified to FAIL
    // on the pre-fix code below (see the two comments marked "FAILS
    // PRE-FIX"); this is not a gate that has never seen red.
    {
        // A colour per clip, so "which clip's pixels survived" is a plain
        // RGB search over the painted image.
        class RangedRenderer : public SObjectRenderer {
        public:
            RangedRenderer( SObject &o, QColor c )
                : SObjectRenderer( o ), color_( c ) {}
            void draw( SLink &lk, SRenderContext &ctx ) override
            { drawObjectWaveform( getObject(), lk, ctx, color_ ); }
            bool collectEnvelope( SLink &lk, const SEnvelopeWindow &win,
                                  preview_t *out ) override
            { return collectObjectEnvelope( getObject(), lk, win, out ); }
        private:
            QColor color_;
        };

        // A fixture whose preview is EXPLICIT DIGITAL ZERO (min==max==0)
        // outside [matStart, matEnd) of its own duration, and a fixed
        // +-level plateau inside it. matStart=0, matEnd=duration means
        // "fully covered", used where no gap is wanted at all.
        class RangedObject : public SObject {
        public:
            RangedObject( length_t duration, offset_t matStart, offset_t matEnd,
                         previewPart_t level, QColor color )
                : SObject( nullptr ), duration_( duration ),
                  matStart_( matStart ), matEnd_( matEnd ), level_( level ),
                  source_( duration ), color_( color ) {}
            ~RangedObject() override { delete renderer_; }
            std::shared_ptr<twComponent> getRootComponent() override
            { return nullptr; }
            bool     hasDuration() const override { return true; }
            length_t getDuration() const override { return duration_; }
            bool     hasPreview()  const override { return true; }
            twRandomSource *getRandomSource() override { return &source_; }
            int getPreview( preview_t *dest, offset_t start, length_t length,
                            offset_t nProbes ) override
            {
                if( nProbes <= 0 || !dest ) return -1;
                if( length < 1 ) length = 1;
                for( offset_t i = 0; i < nProbes; ++i ) {
                    const offset_t pos = start + ( i * length ) / nProbes;
                    if( pos >= matStart_ && pos < matEnd_ ) {
                        dest[i].min = (previewPart_t)( -level_ );
                        dest[i].max = level_;
                    } else {
                        dest[i].min = 0;
                        dest[i].max = 0;
                    }
                }
                return 0;
            }
            QWidget *getDetailEditWidget( QWidget * ) override { return nullptr; }
            QWidget *getInlineEditWidget( QWidget * ) override { return nullptr; }
            SObjectRenderer *getInlineRenderer() override
            {
                if( !renderer_ ) renderer_ = new RangedRenderer( *this, color_ );
                return renderer_;
            }
        private:
            length_t       duration_;
            offset_t       matStart_, matEnd_;
            previewPart_t  level_;
            StubSource     source_;
            QColor         color_;
            RangedRenderer *renderer_ = nullptr;
        };

        // The narrow app context STrack's construction reaches
        // (STrack::setChannels builds a REAL twTrackMix/twPluginChain/
        // twRewire against the environment) — the same stub fragment_test.
        // cpp uses, including its setBufferSize(4096) fix for a documented
        // latent crash in tw303aEnvironment::bufferSize (uninitialized
        // until set).
        class StubAppContext : public SAppContext {
        public:
            StubAppContext() { env_.setBufferSize( 4096 ); }
            SProject *getCurrentProject() const override { return nullptr; }
            tw303aEnvironment *get303aEnvironment() const override { return &env_; }
            void rewireSpeaker() override {}
            bool isSLinkSelected( SLink * ) const override { return false; }
            void setSelectionFromPaths( const QList<QList<int>> & ) override {}
            void addSelectionFromPaths( const QList<QList<int>> & ) override {}
            void removeSelectionFromPaths( const QList<QList<int>> & ) override {}
            void toggleSelectionFromPaths( const QList<QList<int>> & ) override {}
            QList<QList<int>> getCurrentSelectionPaths() const override { return {}; }
            QString testOutputDir() const override { return QString(); }
            bool ensureOutputDirExists() const override { return false; }
            void startRender( const audio::RenderParams & ) override {}
            bool isRenderingActive() const override { return false; }
            void setPlaybackRunning( bool ) override {}
            offset_t getGlobalLocatorPos() const override { return 0; }
        private:
            mutable tw303aEnvironment env_;
        };

        StubAppContext ctx;
        SAppContext::setInstance( &ctx );

        std::unique_ptr<SProject> project( new SProject );
        project->setChannels( 1 );

        // Paint `track` into a fresh 1-pixel-per-frame canvas (FlatContext
        // with spp=1.0 makes screen pixels and timeline frames coincide, so
        // fixture start times can be chosen as pixel positions directly).
        auto paintTrack = []( STrack &track, int w, int h ) {
            QImage img( w, h, QImage::Format_RGB32 );
            img.fill( QColor( 0, 0, 0 ) );
            QPainter painter( &img );
            FlatContext rc( painter, 0, 0, 1.0 );
            rc.setVisibRect( QRect( 0, 0, w, h ) );
            STrackRendererInline rndr( track );
            SLink dummySelf( track, nullptr );   // draw()'s 1st arg is unused
            rndr.draw( dummySelf, rc );
            return img;
        };
        auto columnHasColor = []( const QImage &img, int x, QColor c ) {
            for( int y = 0; y < img.height(); ++y )
                if( ( img.pixel( x, y ) & 0x00ffffff ) == ( c.rgb() & 0x00ffffff ) )
                    return true;
            return false;
        };

        // --- AC5.1: deterministic z-order, latest start on top -------------
        //
        // Q starts LATER (1200) than P (1000) but is added as CHILD 0 (P is
        // child 1) — child order CONTRADICTS start-time order on purpose,
        // which is the only way this sub-test can tell "sorted by start
        // time" apart from "drawn in childOrder_" (the behaviour before
        // this milestone).
        {
            std::unique_ptr<STrack> track( new STrack( project.get() ) );
            RangedObject *q = new RangedObject(
                300, 0, 300, 80, QColor( 0, 0, 255 ) );
            SLink *qLink = new SLink( *q, nullptr );
            qLink->setStartTime( 1200 );
            qLink->setParent( track.get() );

            RangedObject *p = new RangedObject(
                300, 0, 300, 80, QColor( 255, 0, 0 ) );
            SLink *pLink = new SLink( *p, nullptr );
            pLink->setStartTime( 1000 );
            pLink->setParent( track.get() );

            check( track->indexOfChild( qLink ) == 0
                      && track->indexOfChild( pLink ) == 1,
                   "AC5.1 fixture: Q is child 0, P is child 1 (contradicts "
                   "start-time order)" );

            QImage img = paintTrack( *track, 1400, 200 );
            const int OVERLAP_X = 1250;   // inside [1200,1300), the overlap
            // FAILS PRE-FIX: the unfixed code paints childLinks() in child
            // order (Q then P), so P — child 1 but the EARLIER start — ends
            // up on top and this reads false.
            check( columnHasColor( img, OVERLAP_X, QColor( 0, 0, 255 ) ),
                   "AC5.1: Q (the LATER start, 1200) is ON TOP in the "
                   "overlap, though it is child 0" );
            // FAILS PRE-FIX: for the same reason, P's red is still visible
            // there on the unfixed code.
            check( !columnHasColor( img, OVERLAP_X, QColor( 255, 0, 0 ) ),
                   "AC5.1: ...and P (the earlier start, 1000) is fully "
                   "covered there, not merely partially" );
        }

        // --- AC5.2 / AC5.3: material clipping, a gap paints what is beneath
        //
        // E is fully covered [0,300); L starts at 100 with a GAP over its
        // own relative [0,200) (timeline [100,300)) and material only in
        // relative [200,300) (timeline [300,400)). L's start (100) is LATER
        // than E's (0), so D11 already puts L on top — this sub-test
        // isolates D10 by construction.
        {
            std::unique_ptr<STrack> track( new STrack( project.get() ) );
            RangedObject *e = new RangedObject(
                300, 0, 300, 80, QColor( 255, 0, 0 ) );
            SLink *eLink = new SLink( *e, nullptr );
            eLink->setStartTime( 0 );
            eLink->setParent( track.get() );

            RangedObject *l = new RangedObject(
                300, 200, 300, 80, QColor( 0, 0, 255 ) );
            SLink *lLink = new SLink( *l, nullptr );
            lLink->setStartTime( 100 );
            lLink->setParent( track.get() );

            QImage img = paintTrack( *track, 500, 200 );

            // Inside L's gap AND E's material (timeline [150,250), clear of
            // both the 1px inset and the matEnd/window edges by 50+ px).
            // FAILS PRE-FIX: the unfixed code fills L's WHOLE window
            // opaquely regardless of L's own material, erasing E's red
            // there entirely.
            check( columnHasColor( img, 220, QColor( 255, 0, 0 ) ),
                   "AC5.2/AC5.3: E's material SURVIVES under L's gap — the "
                   "body was clipped to L's material extent, not painted "
                   "as L's whole window" );

            // Sanity: L still paints normally where it DOES have material,
            // so the fix is a CLIP, not a suppression of L altogether.
            check( columnHasColor( img, 350, QColor( 0, 0, 255 ) ),
                   "...and L's OWN material still paints where it exists" );
        }
    }

    // ===== 7. PROPOSAL 41 M6 — THE TAG CHIP (D12-D14)
    //
    // AC6.1 (a tag exists, the old bottom-right container label is gone),
    // AC6.2 (D11's occlusion invariant, including the equal-start
    // tiebreak) and AC6.4 (the colour RELATION, across every
    // STrackColorModifier state) all have to be PIXEL gates (trap 6,
    // mirroring M5's own finding in section 6 above): a script-level check
    // through collectEnvelope, or through the pure tagDensityText()/
    // tagChipColor() functions alone, cannot see whether draw() actually
    // CALLS them, still less in what order or at what position. So —
    // exactly as section 6 does — this paints a REAL STrack with REAL
    // SLink children off screen via the actual
    // STrackRendererInline::draw() and recovers the chip's colour and
    // extent from the rendered QImage by exact-RGB search: fillRect() is
    // opaque and NOT antialiased, so an exact match is sound for the chip
    // itself (this section deliberately never searches for TEXT pixels,
    // which are antialiased and would need a fuzzy, unreliable match —
    // the chip's measured WIDTH is what tells full/elided/chip-only/
    // nothing apart, and that is exact geometry).
    //
    // Verified to FAIL on the pre-fix code (git stash of the M6 diff to
    // strackrndrinline.{h,cpp} and scutrndrinline.cpp, rebuilt, rerun):
    // every check in this section failed — there was no chip to find at
    // all, and the old bottom-right container label was still there. See
    // the PR body for the exact before/after run.
    {
        // A minimal clip content object: just enough SObject to be a
        // windowed clip (hasDuration/getDuration) and carry an identifying
        // SName — the tag's SECOND text source (D12), the fallback every
        // clip that is neither a SClipWindow nor file-backed still gets.
        class TagFixtureObject : public SObject {
        public:
            explicit TagFixtureObject( length_t duration )
                : SObject( nullptr ), duration_( duration ) {}
            std::shared_ptr<twComponent> getRootComponent() override { return nullptr; }
            bool     hasDuration() const override { return true; }
            length_t getDuration() const override { return duration_; }
            QWidget *getDetailEditWidget( QWidget * ) override { return nullptr; }
            QWidget *getInlineEditWidget( QWidget * ) override { return nullptr; }
            SObjectRenderer *getInlineRenderer() override { return nullptr; }
        private:
            length_t duration_;
        };

        // A minimal SClipWindow (D12's CONTAINER-BACKED text source): the
        // tag reads the WINDOW's own name via SClipWindow::windowContent(),
        // never a concrete SCut/SMidiCut/SLaneFragment — objects/track may
        // not depend on any of those (tools/check_layering.py), which is
        // exactly what tagFullText() has to get right and this fixture
        // exists to prove. Every method besides asObject()/windowContent()
        // is unreachable from tagFullText() and is a stub for that reason.
        class TagWindowFixture : public SObject, public SClipWindow {
        public:
            TagWindowFixture( SObject &content )
                : SObject( nullptr ), content_( content ) {}
            SObject &asObject() override { return *this; }
            SObject &windowContent() const override { return content_; }
            length_t duration() const override { return content_.getDuration(); }
            length_t durationBlocking() const override { return duration(); }
            length_t loopLength() const override { return 0; }
            offset_t startOffset() const override { return 0; }
            Fraction stretchOrRate() const override { return Fraction( 1 ); }
            Fraction contentAnchorExact() const override { return Fraction( 0 ); }
            Fraction timelineToSourceExact( const Fraction &rel ) const override
            { return rel; }
            void setDurationFromTimeline( length_t ) override {}
            void setStartOffsetFromTimeline( offset_t ) override {}
            void setWindowFromTimeline( offset_t, length_t, length_t,
                                       const Fraction & ) override {}
            void setWindowExact( const Fraction &, length_t, length_t,
                                 const Fraction & ) override {}
            void setContentAnchorExact( const Fraction & ) override {}
            SClipWindow *cloneWindowOverContent( SProject *, SObject & ) const override { return nullptr; }
            std::shared_ptr<twComponent> getRootComponent() override { return nullptr; }
            bool     hasDuration() const override { return true; }
            length_t getDuration() const override { return content_.getDuration(); }
            QWidget *getDetailEditWidget( QWidget * ) override { return nullptr; }
            QWidget *getInlineEditWidget( QWidget * ) override { return nullptr; }
            SObjectRenderer *getInlineRenderer() override { return nullptr; }
        private:
            SObject &content_;
        };

        // A minimal SExternFile (D12's FIRST text source: the FILE name).
        class TagFileFixture : public SExternFile {
        public:
            TagFileFixture( const QString &path, length_t duration )
                : SExternFile( nullptr ), path_( path ), duration_( duration ) {}
            QString getFileName() const override { return path_; }
            std::shared_ptr<twComponent> getRootComponent() override { return nullptr; }
            bool     hasDuration() const override { return true; }
            length_t getDuration() const override { return duration_; }
            QWidget *getDetailEditWidget( QWidget * ) override { return nullptr; }
            QWidget *getInlineEditWidget( QWidget * ) override { return nullptr; }
            SObjectRenderer *getInlineRenderer() override { return nullptr; }
        private:
            QString  path_;
            length_t duration_;
        };

        // Same stub app context as section 6, and for the same reason
        // (STrack::setChannels reaches tw303aEnvironment; see its comment
        // there for the bufferSize(4096) crash fix).
        class StubAppContext : public SAppContext {
        public:
            StubAppContext() { env_.setBufferSize( 4096 ); }
            SProject *getCurrentProject() const override { return nullptr; }
            tw303aEnvironment *get303aEnvironment() const override { return &env_; }
            void rewireSpeaker() override {}
            bool isSLinkSelected( SLink * ) const override { return false; }
            void setSelectionFromPaths( const QList<QList<int>> & ) override {}
            void addSelectionFromPaths( const QList<QList<int>> & ) override {}
            void removeSelectionFromPaths( const QList<QList<int>> & ) override {}
            void toggleSelectionFromPaths( const QList<QList<int>> & ) override {}
            QList<QList<int>> getCurrentSelectionPaths() const override { return {}; }
            QString testOutputDir() const override { return QString(); }
            bool ensureOutputDirExists() const override { return false; }
            void startRender( const audio::RenderParams & ) override {}
            bool isRenderingActive() const override { return false; }
            void setPlaybackRunning( bool ) override {}
            offset_t getGlobalLocatorPos() const override { return 0; }
        private:
            mutable tw303aEnvironment env_;
        };

        StubAppContext ctx;
        SAppContext::setInstance( &ctx );

        std::unique_ptr<SProject> project( new SProject );
        project->setChannels( 1 );

        auto paintTrack = []( STrack &track, int w, int h ) {
            QImage img( w, h, QImage::Format_RGB32 );
            img.fill( QColor( 0, 0, 0 ) );
            QPainter painter( &img );
            FlatContext rc( painter, 0, 0, 1.0 );
            rc.setVisibRect( QRect( 0, 0, w, h ) );
            STrackRendererInline rndr( track );
            SLink dummySelf( track, nullptr );   // draw()'s 1st arg is unused
            rndr.draw( dummySelf, rc );
            return img;
        };
        // Exact-colour search: fillRect() is opaque and not antialiased, so
        // an exact RGB match locates the chip precisely.
        auto columnHasExact = []( const QImage &img, int x, QColor c ) {
            for( int y = 0; y < img.height(); ++y )
                if( ( img.pixel( x, y ) & 0x00ffffff ) == ( c.rgb() & 0x00ffffff ) )
                    return true;
            return false;
        };
        // The chip's own measured horizontal extent within [xa,xb) — read
        // back independently of anything this test computed, exactly what
        // section 6's columnHasColor generalises to "how wide", not just
        // "present".
        auto chipWidth = [&]( const QImage &img, int xa, int xb, QColor c ) {
            int lo = -1, hi = -1;
            for( int x = xa; x < xb; ++x ) {
                if( !columnHasExact( img, x, c ) ) continue;
                if( lo < 0 ) lo = x;
                hi = x + 1;
            }
            return ( lo < 0 ) ? 0 : ( hi - lo );
        };

        // --- 7d (checked first, no paint needed). AC6.1/D12: the tag's
        // TWO text sources, resolved through the type-agnostic
        // SClipWindow/SExternFile interfaces rather than any concrete
        // SCut/SMidiCut/SLaneFragment — the whole reason tagFullText()
        // exists rather than a dynamic_cast<SCut*> in objects/track, which
        // may not depend on objects/cut (tools/check_layering.py).
        {
            // The WINDOW is what the paint loop passes tagFullText() -- it is
            // lk->getSObject(), an SCut over the file, never the file itself.
            // Handing it the bare SExternFile makes SClipWindow::of() answer
            // null and the function correctly falls through to getSName(),
            // which is what this check used to assert against and fail.
            TagFileFixture file( QStringLiteral( "/some/dir/MyFile.wav" ), 100 );
            TagWindowFixture fileClip( file );
            check( STrackRendererInline::tagFullText( fileClip )
                      == QStringLiteral( "MyFile.wav" ),
                   "D12 source 1: a plain sample-backed clip's tag is the "
                   "FILE name (basename only, directory stripped)" );

            TagFixtureObject windowContent( 100 );
            TagWindowFixture asset( windowContent );
            asset.setSName( QStringLiteral( "Gappy" ) );
            check( STrackRendererInline::tagFullText( asset )
                      == QStringLiteral( "Gappy" ),
                   "D12 source 2: a container-backed clip's (asset or "
                   "fragment) tag is the WINDOW's own name — exactly what "
                   "the old bottom-right label read as cut.getSName()" );

            TagFixtureObject noNameFallback( 100 );
            check( STrackRendererInline::tagFullText( noNameFallback )
                      == QString( SObject::DEFAULT_SNAME ),
                   "…and anything that is neither a file nor explicitly "
                   "named falls back to its own (default) SName, never "
                   "crashes or returns empty" );
        }

        // --- 7a. AC6.4: the colour RELATION across every
        // STrackColorModifier state, tied to the ACTUAL painted pixel —
        // not merely to tagChipColor()/laneFillColor() in isolation, which
        // could hold even if draw() painted something else entirely.
        {
            std::unique_ptr<STrack> track( new STrack( project.get() ) );
            TagFixtureObject *obj = new TagFixtureObject( 300 );
            obj->setSName( QStringLiteral( "Kick" ) );
            SLink *lk = new SLink( *obj, nullptr );
            lk->setStartTime( 0 );
            lk->setParent( track.get() );

            const int lumBody = qGray( QColor( 160, 160, 160 ).rgb() );
            struct State { bool muted, solo, armed; const char *label; };
            const State states[] = {
                { false, false, false, "default" },
                { true,  false, false, "muted" },
                { false, true,  false, "solo" },
                { false, false, true,  "armed" },
                { true,  true,  false, "muted+solo" },
                { true,  false, true,  "muted+armed" },
                { false, true,  true,  "solo+armed" },
                { true,  true,  true,  "muted+solo+armed" },
            };
            for( const State &s : states ) {
                track->setMuted( s.muted );
                track->setSolo( s.solo );
                track->setArmedForRecording( s.armed );

                const QColor fill = STrackRendererInline::laneFillColor( *track );
                const QColor chip = STrackRendererInline::tagChipColor( fill );
                const int lumFill = qGray( fill.rgb() );
                const int lumChip = qGray( chip.rgb() );

                char what[192];
                std::snprintf( what, sizeof( what ),
                    "AC6.4 [%s]: tag chip colour is strictly DARKER than "
                    "the lane fill it derives from (%d < %d)",
                    s.label, lumChip, lumFill );
                check( lumChip < lumFill, what );
                std::snprintf( what, sizeof( what ),
                    "AC6.4 [%s]: ...and darker than the fixed clip-body "
                    "grey (%d < %d), which is what makes white chip text "
                    "readable on it", s.label, lumChip, lumBody );
                check( lumChip < lumBody, what );

                QImage img = paintTrack( *track, 200, 100 );
                std::snprintf( what, sizeof( what ),
                    "AC6.4 [%s]: the PAINTED chip uses exactly the derived "
                    "colour (trap 6 — the relation alone does not prove "
                    "draw() used it)", s.label );
                check( columnHasExact( img, 3, chip ), what );
            }
            track->setMuted( false );
            track->setSolo( false );
            track->setArmedForRecording( false );
            lk->setParent( nullptr );
        }

        // --- 7b. AC6.1 + AC6.2: a tag exists, and D11's occlusion
        // invariant holds for overlapping placements with DIFFERENT start
        // times — a clip's tag sits at ITS OWN left edge, which a
        // later-starting clip (on top, per D11) can never reach because it
        // paints nothing left of ITS OWN start.
        {
            std::unique_ptr<STrack> track( new STrack( project.get() ) );

            TagFixtureObject *p = new TagFixtureObject( 300 );
            p->setSName( QStringLiteral( "AlphaClip" ) );
            SLink *pLink = new SLink( *p, nullptr );
            pLink->setStartTime( 0 );
            pLink->setParent( track.get() );

            TagFixtureObject *q = new TagFixtureObject( 300 );
            q->setSName( QStringLiteral( "BetaClip" ) );
            SLink *qLink = new SLink( *q, nullptr );
            qLink->setStartTime( 150 );        // starts LATER -> on top (D11)
            qLink->setParent( track.get() );

            const QColor fill = STrackRendererInline::laneFillColor( *track );
            const QColor chip = STrackRendererInline::tagChipColor( fill );
            QImage img = paintTrack( *track, 500, 100 );

            check( columnHasExact( img, 1, chip ),
                   "AC6.1: a plain clip carries a tag chip at its "
                   "bottom-left corner" );
            check( columnHasExact( img, 151, chip ),
                   "AC6.2: the LATER clip (on top per D11) also carries "
                   "its own tag, at ITS OWN left edge" );
            check( columnHasExact( img, 1, chip ),
                   "AC6.2: ...and the EARLIER clip's tag (fully underneath "
                   "the later one everywhere from x=150 on) still survives "
                   "at ITS OWN corner, left of x=150" );
        }

        // --- 7b continued: THE EQUAL-START TIEBREAK. D11 itself says an
        // exact start-time tie "is the only case that can hide a [tag]" —
        // the tiebreak (child index) exists to make WHICH one is hidden
        // DETERMINISTIC, not to make both survive, which is physically
        // impossible once both share the identical left edge and the
        // winner paints its own opaque chip over it. What is gated here is
        // exactly that: (startTime, childIndex) — never insertion order
        // read some other way, never the allocator's addresses — decides,
        // reproducibly, which clip's tag is left standing.
        {
            std::unique_ptr<STrack> track( new STrack( project.get() ) );

            TagFixtureObject *loser = new TagFixtureObject( 300 );
            loser->setSName( QStringLiteral( "LoserClip" ) );
            SLink *loserLink = new SLink( *loser, nullptr );
            loserLink->setStartTime( 200 );
            loserLink->setParent( track.get() );          // child 0

            TagFixtureObject *winner = new TagFixtureObject( 300 );
            winner->setSName( QStringLiteral( "WinnerClip" ) );
            SLink *winnerLink = new SLink( *winner, nullptr );
            winnerLink->setStartTime( 200 );               // TIED with loser
            winnerLink->setParent( track.get() );          // child 1 -> wins

            check( track->indexOfChild( loserLink ) == 0
                      && track->indexOfChild( winnerLink ) == 1,
                   "AC6.2 tiebreak fixture: loser is child 0, winner is "
                   "child 1, identical start times" );

            const QColor fill = STrackRendererInline::laneFillColor( *track );
            const QColor chip = STrackRendererInline::tagChipColor( fill );
            QImage img = paintTrack( *track, 600, 100 );

            check( columnHasExact( img, 201, chip ),
                   "AC6.2 tiebreak: the higher-childIndex clip's tag "
                   "survives at the tied corner, deterministically per "
                   "D11's (startTime, childIndex) order" );
        }

        // --- 7c. AC6.3: the density ladder at four widths, and D14's
        // "the cap is announced" tooltip flag — checked against the SAME
        // shared functions draw() calls (tagDensityText/kTagMaxChars), and
        // tied to the ACTUAL painted chip WIDTH so a wiring bug (draw()
        // forgetting to call tagDensityText at all) cannot pass silently.
        {
            const QString longName =
                QStringLiteral( "VeryLongClipNameHere" );   // 21 chars > 12
            check( longName.length() > STrackRendererInline::kTagMaxChars,
                   "AC6.3 fixture: the name exceeds the 12-char cap" );

            QFont tagFont = QGuiApplication::font();   // matches draw()'s own
            tagFont.setPointSize( 7 );                 // font exactly
            const QFontMetrics fm( tagFont );
            const int kTagPadX = 3;   // must match strackrndrinline.cpp

            auto widthFor = []( std::unique_ptr<STrack> &track, length_t dur,
                                const QString &name ) {
                TagFixtureObject *obj = new TagFixtureObject( dur );
                obj->setSName( name );
                SLink *lk = new SLink( *obj, nullptr );
                lk->setStartTime( 0 );
                lk->setParent( track.get() );
            };

            struct Rung {
                length_t    duration;
                const char *label;
            };
            // Chosen so vr.width() (== duration - 2, the 1px inset each
            // side, spp=1.0 makes pixels and frames coincide) lands in each
            // rung: FULL needs plenty of room for the 12-char cap; ELIDED a
            // little room, enough for one character plus an ellipsis but
            // not the whole cap; CHIP ONLY exactly the fixed minimum chip
            // width (6, matching strackrndrinline.cpp's kTagMinChipW) and
            // no more; NOTHING less than that.
            const Rung rungs[] = {
                { 250, "full" }, { 40, "elided" }, { 8, "chip-only" },
                { 4, "nothing" },
            };

            int prevWidth = -1;
            for( const Rung &r : rungs ) {
                std::unique_ptr<STrack> track( new STrack( project.get() ) );
                widthFor( track, r.duration, longName );

                const QColor fill = STrackRendererInline::laneFillColor( *track );
                const QColor chip = STrackRendererInline::tagChipColor( fill );
                QImage img = paintTrack( *track, 300, 100 );
                const int measured = chipWidth( img, 0, 300, chip );

                const int vrWidth = (int) r.duration - 2;
                bool cut = false;
                const QString drawn = STrackRendererInline::tagDensityText(
                    longName, fm, vrWidth - 2 * kTagPadX, &cut );

                char what[192];
                if( vrWidth >= 6 && drawn.isEmpty() ) {
                    // CHIP ONLY: the fixed minimum, no text.
                    std::snprintf( what, sizeof( what ),
                        "AC6.3 [%s]: chip-only rung paints the fixed "
                        "minimum width (measured %d)", r.label, measured );
                    check( measured == qMin( 6, vrWidth ), what );
                } else if( vrWidth < 6 ) {
                    std::snprintf( what, sizeof( what ),
                        "AC6.3 [%s]: NOTHING is painted (too narrow even "
                        "for the chip; measured %d)", r.label, measured );
                    check( measured == 0, what );
                } else {
                    const int expected =
                        fm.horizontalAdvance( drawn ) + 2 * kTagPadX;
                    std::snprintf( what, sizeof( what ),
                        "AC6.3 [%s]: text-bearing chip width matches the "
                        "shared tagDensityText() decision exactly "
                        "(measured %d, expected %d, text \"%s\")",
                        r.label, measured, expected, qPrintable( drawn ) );
                    check( measured == expected, what );
                }

                std::snprintf( what, sizeof( what ),
                    "AC6.3 [%s]: the tooltip's `cut` flag is set whenever "
                    "the drawn text is not the true full name (here: "
                    "always, since the name exceeds the 12-char cap)",
                    r.label );
                check( cut, what );

                if( prevWidth >= 0 && measured > 0 ) {
                    std::snprintf( what, sizeof( what ),
                        "AC6.3: rung \"%s\" is no WIDER than the previous "
                        "rung (%d <= %d) — the ladder narrows monotonically",
                        r.label, measured, prevWidth );
                    check( measured <= prevWidth, what );
                }
                if( measured > 0 ) prevWidth = measured;
            }
        }
    }

    if( g_failures ) {
        std::printf( "\n%d check(s) failed\n", g_failures );
        return 1;
    }
    std::printf( "\nall envelope-collect checks passed\n" );
    return 0;
}
