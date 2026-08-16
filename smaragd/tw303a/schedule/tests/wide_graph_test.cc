// Proposal 36 B2 — components declare their width, and the wide render path
// exists. AC B2.1, B2.2 and B2.3 live here.
//
// WHAT THIS GATE IS FOR. B1b gave the page a channel dimension and proved it at
// UNIT level: a page built four channels wide round-trips through channelPtr().
// B2 builds the machinery that could make a page wider — getOutputChannels()
// (§4.2), renderPageWide() (§4.3), the plug channel rule (§4.4 rule 1) and the
// direct bound-page read (rule 2) — so what has to be proven here is that a wide
// page survives THE REAL ENGINE: allocated by twComponent::freezePage at its
// component's declared width, filled by freezePage_nolock's width fork, executed
// on the real CaptureRevalidator's worker threads, and read back through the
// latch seam by consumers of a different width.
//
// It lives in schedule/tests because that is the module whose test may link
// tw_schedule; the synthetic component itself stays in graph/tests, where B1b
// deliberately put it in a HEADER so B2 could promote it without moving it.
//
// NOTHING IN THE PRODUCTION GRAPH IS WIDE AT B2. Every component in the app
// still returns the default getOutputChannels() == 1, so the byte-exactness
// goldens are green by construction — this file is the only place in the tree
// where a page wider than one channel exists at all.

#include "tw/schedule/capture_revalidator.h"
#include "tw/pages/capture_page_pool.h"
#include "tw/graph/twcomponent.h"
#include "tw/graph/twlatch.h"
#include "tw/graph/tw303aenv.h"
#include "tw/graph/tw_frozen_inputs.h"

#include "tw_synthetic_wide_source.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

static int failures = 0;
#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (cond) { printf("ok   %s\n", msg); }                             \
        else      { printf("FAIL %s\n", msg); ++failures; }                 \
    } while (0)

static const offset_t CAP = (offset_t)twOutputPage::FRAME_CAPACITY;

// ---------------------------------------------------------------------------
// A FOUR-PORT, ONE-CHANNEL source — exactly the shape twSampleReader and
// twWavInput have today: getNOutputs() reports 4 and four latches exist, while
// the frozen page is one channel wide. Reading any of its four plugs must yield
// channel 0, which is §4.4 rule (1)'s clamp and is what the engine has always
// done (twSampleSource::read: "mono plays on every channel").
//
// Its signal is dyadic (exactly 0.25 or 0.75, flipping every 16 frames) and its
// value range is disjoint from all four of twSyntheticWideSource's, so a single
// sample says which producer it came from as well as which channel.
// ---------------------------------------------------------------------------
class NarrowFourPortSource : public twComponent
{
public:
    explicit NarrowFourPortSource( tw303aEnvironment &e ) : twComponent( e ) {}

    static float value( offset_t frame )
    {
        return ( ( frame / 16 ) & 1 ) ? 0.25f : 0.75f;
    }

    bool isSeekable() const override { return true; }
    int seekTo( offset_t p ) override { pos_ = p; return 0; }
    void reset() override { pos_ = 0; }
    bool usesSerialCursor() const override { return true; }

    length_t renderFrames( sample_t *out, length_t n, const sample_t *,
                           length_t, idx_t ) override
    {
        for( length_t i = 0; i < n; ++i ) out[i] = value( pos_ + (offset_t)i );
        pos_ += (offset_t)n;
        return n;
    }

    void createOutputLatches() override
    {
        for( idx_t i = 0; i < 4; ++i )
            pOutputLatches_[i] =
                std::make_shared<twStreamingLatch>( shared_from_this(), i, 0 );
    }
    idx_t getNInputs() const override { return 0; }
    idx_t getNOutputs() const override { return 4; }          // FOUR PORTS…
    // …and ONE CHANNEL. The default; spelled out because the whole point of
    // this fixture is that the two numbers are independent (§7 trap 8).
    idx_t getOutputChannels() const override { return 1; }
    const char *getInputName( idx_t ) const override { return nullptr; }
    const char *getOutputName( idx_t ) const override { return "out"; }

private:
    offset_t pos_ = 0;
};

// ---------------------------------------------------------------------------
// A consumer whose PAGE WIDTH and PLUG COUNT are set independently, and which
// can read its input either through the plugs (§4.4 rule 1 — the clamp happens
// inside twStreamingLatch::copyData, invisibly to the component) or by picking
// channels out of the bound input PAGE itself (rule 2 — no channel argument is
// threaded through the read API for this; the component already has the page).
// ---------------------------------------------------------------------------
class WideConsumer : public twComponent
{
public:
    enum class Read { Plugs, BoundPage };

    WideConsumer( tw303aEnvironment &e, idx_t width, idx_t nPlugs, Read mode )
        : twComponent( e ), width_( width ), nPlugs_( nPlugs ), mode_( mode ) {}

    void setProducer( std::shared_ptr<twComponent> p ) { producer_ = std::move( p ); }

    idx_t getOutputChannels() const override { return width_; }

    // WIDTH 1 — the pre-B2 path, untouched: freezePage_nolock calls
    // renderFrames() with channelPtr(0) exactly as it always did.
    length_t renderFrames( sample_t *out, length_t n, const sample_t *,
                           length_t, idx_t ) override
    {
        ++narrowRenders_;
        return readPlug_( 0, out, n );
    }

    // WIDTH > 1 — one pass, every channel, one advance per input cursor.
    length_t renderPageWide( twOutputPage &page, length_t frames,
                             const sample_t *, length_t ) override
    {
        ++wideRenders_;
        if( frames > (length_t)page.channelFrames() )
            frames = (length_t)page.channelFrames();

        if( mode_ == Read::BoundPage && renderFromBoundPage_( page, frames ) )
            return frames;

        // Read each PLUG exactly once. A plug carries a cursor
        // (twLatchOutput::offset), so reading one twice would fill the second
        // channel with the NEXT page's audio — §4.3's warning, applied to the
        // input side of the same component.
        std::vector<std::vector<float>> got( (size_t)nPlugs_ );
        for( idx_t p = 0; p < nPlugs_; ++p ) {
            got[(size_t)p].assign( (size_t)frames, 0.0f );
            readPlug_( p, got[(size_t)p].data(), frames );
        }
        for( idx_t c = 0; c < (idx_t)page.channels(); ++c ) {
            const idx_t p = ( c < nPlugs_ ) ? c : nPlugs_ - 1;
            std::memcpy( page.channelPtr( c ), got[(size_t)p].data(),
                         (size_t)frames * sizeof( float ) );
        }
        return frames;
    }

    void createOutputLatches() override
    {
        pOutputLatches_[0] =
            std::make_shared<twStreamingLatch>( shared_from_this(), 0, 0 );
    }
    void reset() override {}
    idx_t getNInputs() const override { return nPlugs_; }
    idx_t getNOutputs() const override { return 1; }
    const char *getInputName( idx_t ) const override { return "in"; }
    const char *getOutputName( idx_t ) const override { return "out"; }

    int wideRenders() const { return wideRenders_; }
    int narrowRenders() const { return narrowRenders_; }
    int boundReads() const { return boundReads_; }

private:
    length_t readPlug_( idx_t p, sample_t *dst, length_t n )
    {
        if( p < 0 || (size_t)p >= pInputPlugs_.size() || !pInputPlugs_[(size_t)p] )
            return 0;
        return static_cast<twLatchStreamingOutput *>( pInputPlugs_[(size_t)p].get() )
            ->readStreamingData( dst, n );
    }

    // §4.4 RULE (2): a wide component reads its bound input PAGES directly and
    // picks channels itself. The clamp it applies is its OWN policy — the plug
    // rule does not reach here, and no channel argument had to be threaded
    // through readStreamingData/copyData for this to work.
    bool renderFromBoundPage_( twOutputPage &page, length_t frames )
    {
        const twFrozenInputs *fi = twFrozenInputScope::active();
        if( !fi || !producer_ ) return false;
        std::shared_ptr<twOutputPage> src =
            fi->find( producer_.get(), page.startPosition );
        if( !src || src->validAspects == 0 ) return false;

        ++boundReads_;
        // The width acted on is the width of the PAGE IN HAND (src->channels()),
        // never the producer's declared width — the tree launders pages between
        // components, so a declared width is a promise about future pages.
        const idx_t srcCh = (idx_t)src->channels();
        length_t n = frames;
        if( n > (length_t)src->validFrames ) n = (length_t)src->validFrames;
        for( idx_t c = 0; c < (idx_t)page.channels(); ++c ) {
            const idx_t sc = ( c < srcCh ) ? c : srcCh - 1;
            std::memcpy( page.channelPtr( c ), src->channelPtr( sc ),
                         (size_t)n * sizeof( float ) );
            for( length_t i = n; i < frames; ++i ) page.channelPtr( c )[i] = 0.0f;
        }
        return true;
    }

    const idx_t width_;
    const idx_t nPlugs_;
    const Read  mode_;
    std::shared_ptr<twComponent> producer_;
    int wideRenders_ = 0;
    int narrowRenders_ = 0;
    int boundReads_ = 0;
};

// ---------------------------------------------------------------------------
// AC B2.3's subject: a component that DECLARES width 4 and does not override
// renderPageWide(). Its renderFrames() writes an unmistakable 1.0 so that "the
// base refused" and "the base quietly rendered channel 0 anyway" cannot be
// confused for one another.
// ---------------------------------------------------------------------------
class BadWideSource : public twComponent
{
public:
    explicit BadWideSource( tw303aEnvironment &e ) : twComponent( e ) {}

    idx_t getOutputChannels() const override { return 4; }
    length_t renderFrames( sample_t *out, length_t n, const sample_t *,
                           length_t, idx_t ) override
    {
        ++narrowRenders;
        for( length_t i = 0; i < n; ++i ) out[i] = 1.0f;
        return n;
    }
    void reset() override {}
    void createOutputLatches() override
    {
        pOutputLatches_[0] =
            std::make_shared<twStreamingLatch>( shared_from_this(), 0, 0 );
    }
    idx_t getNInputs() const override { return 0; }
    idx_t getNOutputs() const override { return 1; }
    const char *getInputName( idx_t ) const override { return nullptr; }
    const char *getOutputName( idx_t ) const override { return "out"; }

    int narrowRenders = 0;
};

// --- helpers ---------------------------------------------------------------

// Does channel c of `page` carry the synthetic source's channel `expect`
// everywhere it is sampled? Checked at both ends and across a period boundary,
// so a page displaced by any amount fails.
static bool channelMatches( const std::shared_ptr<twOutputPage> &page, idx_t c,
                            idx_t expect, offset_t pagePos )
{
    static const offset_t probes[] = { 0, 1, 7, 8, 15, 16, 100, 4095, 32768,
                                       CAP - 2, CAP - 1 };
    for( offset_t f : probes ) {
        const float want = twSyntheticWideSource::value( expect, pagePos + f );
        if( page->channelPtr( c )[f] != want ) return false;
    }
    return true;
}

static bool channelIsConstantSignal( const std::shared_ptr<twOutputPage> &page,
                                     idx_t c, offset_t pagePos )
{
    static const offset_t probes[] = { 0, 1, 15, 16, 100, 4095, CAP - 1 };
    for( offset_t f : probes ) {
        if( page->channelPtr( c )[f] != NarrowFourPortSource::value( pagePos + f ) )
            return false;
    }
    return true;
}

int main()
{
    // =====================================================================
    // AC B2.1 — the synthetic component's page carries four distinguishable
    // channels, at several positions, THROUGH THE REAL SCHEDULER.
    //
    // Nothing here calls renderWide() by hand: the demand goes to
    // CaptureRevalidator, which plans the node, executes it on a worker via
    // freezePageWithInputs -> freezePage -> freezePage_nolock, and it is
    // freezePage that allocates the page at getOutputChannels() and
    // freezePage_nolock that forks on the width of the page in hand.
    // =====================================================================
    {
        tw303aEnvironment env;
        CapturePagePool pool( 16 );
        CaptureRevalidator reval( &pool, 4 );

        auto src = std::make_shared<twSyntheticWideSource>( env );
        src->init();

        auto d = reval.requestGraphPages( src, 0, 3 );
        d->wait();
        CHECK( d->done(), "B2.1: the scheduler completes a demand on a wide component" );

        bool widthOk = true, contentOk = true, distinctOk = true, framesOk = true;
        for( int i = 0; i < 3; ++i ) {
            const offset_t pos = (offset_t)i * CAP;
            auto p = src->getPageIfExists( pos );
            if( !p || p->validAspects == 0 ) { widthOk = contentOk = false; break; }
            if( p->channels() != 4 ) widthOk = false;
            if( p->validFrames != (uint32_t)CAP ) framesOk = false;
            for( idx_t c = 0; c < 4; ++c ) {
                // Each channel carries ITS OWN signal at THIS page's position —
                // which is also what proves the seek-once/advance-once shape:
                // a per-channel loop over renderFrames() would have filled
                // channel 1 with page i+1's audio, and page 1 with page 3's.
                if( !channelMatches( p, c, c, pos ) ) contentOk = false;
                // …and no other channel's. The four value ranges are pairwise
                // disjoint, so a SINGLE sample names its channel: a swap of two
                // channels cannot pass this.
                for( idx_t other = 0; other < 4; ++other )
                    if( other != c && channelMatches( p, c, other, pos ) )
                        distinctOk = false;
            }
        }
        CHECK( widthOk, "B2.1: every scheduled page is 4 channels wide" );
        CHECK( framesOk, "B2.1: …with a full page of valid frames" );
        CHECK( contentOk,
               "B2.1: each channel carries its own signal at its own position "
               "(3 pages x 4 channels x 11 probes)" );
        CHECK( distinctOk,
               "B2.1: and no channel matches another's signature (a swap fails)" );

        // The width fork left the accounting alone: one page per position, four
        // channels wide, not four pages.
        auto p0 = src->getPageIfExists( 0 );
        CHECK( p0 && p0->sampleCount() == 4 * (size_t)CAP,
               "B2.1: one page of 4 * FRAME_CAPACITY floats, not four pages" );
    }

    // =====================================================================
    // AC B2.2 — the §4.4 clamp, both directions, both paths.
    //
    // "Both paths" is the plug pull (the clamp inside copyData) and the bound
    // page (the consumer picking channels out of the page itself). The plug
    // pull is exercised BOTH through the scheduler (copyData's bound-page
    // branch) and through a direct legacy pull on a second, unscheduled
    // instance (copyData's recursive-freeze branch) — the two branches meet at
    // the same memcpy, and the point of asserting both is that they do.
    // =====================================================================

    // ---- (a) a WIDTH-1 consumer of a WIDTH-4 producer reads channel 0 ----
    {
        tw303aEnvironment env;
        CapturePagePool pool( 16 );
        CaptureRevalidator reval( &pool, 4 );

        auto src = std::make_shared<twSyntheticWideSource>( env );
        src->init();
        auto narrow = std::make_shared<WideConsumer>( env, 1, 1,
                                                      WideConsumer::Read::Plugs );
        narrow->init();
        narrow->setInput( 0, src->linkOutput( 0 ) );

        auto d = reval.requestGraphPages( narrow, 0, 2 );
        d->wait();

        bool ok = true;
        for( int i = 0; i < 2; ++i ) {
            auto p = narrow->getPageIfExists( (offset_t)i * CAP );
            if( !p || p->validAspects == 0 || p->channels() != 1 ) { ok = false; break; }
            if( !channelMatches( p, 0, 0, (offset_t)i * CAP ) ) ok = false;
        }
        CHECK( ok, "B2.2a (bound page): a width-1 consumer of a width-4 producer "
                   "reads channel 0" );
        CHECK( narrow->narrowRenders() > 0 && narrow->wideRenders() == 0,
               "B2.2a: …through renderFrames(), i.e. the untouched pre-B2 path" );

        // The same thing again with NO scheduler in sight: copyData's legacy
        // recursive pull must clamp identically.
        auto narrow2 = std::make_shared<WideConsumer>( env, 1, 1,
                                                       WideConsumer::Read::Plugs );
        narrow2->init();
        narrow2->setInput( 0, src->linkOutput( 0 ) );
        auto lp = narrow2->freezePage( 0, nullptr, 0, (length_t)CAP,
                                       env.getSRate(), nullptr );
        CHECK( lp && lp->validAspects != 0 && channelMatches( lp, 0, 0, 0 ),
               "B2.2a (plug pull): the legacy pull clamps to channel 0 identically" );
    }

    // ---- (b) a WIDTH-4 consumer of a WIDTH-1 producer: that channel on all 4 --
    {
        tw303aEnvironment env;
        CapturePagePool pool( 16 );
        CaptureRevalidator reval( &pool, 4 );

        auto src = std::make_shared<NarrowFourPortSource>( env );
        src->init();
        auto wide = std::make_shared<WideConsumer>( env, 4, 4,
                                                    WideConsumer::Read::Plugs );
        wide->init();
        for( idx_t c = 0; c < 4; ++c ) wide->setInput( c, src->linkOutput( c ) );

        auto d = reval.requestGraphPages( wide, 0, 2 );
        d->wait();

        bool ok = true, allFour = true;
        for( int i = 0; i < 2; ++i ) {
            const offset_t pos = (offset_t)i * CAP;
            auto p = wide->getPageIfExists( pos );
            if( !p || p->validAspects == 0 || p->channels() != 4 ) { ok = false; break; }
            for( idx_t c = 0; c < 4; ++c )
                if( !channelIsConstantSignal( p, c, pos ) ) allFour = false;
            // …and they are all the SAME channel, byte for byte.
            for( idx_t c = 1; c < 4; ++c )
                if( std::memcmp( p->channelPtr( 0 ), p->channelPtr( c ),
                                 (size_t)CAP * sizeof( float ) ) != 0 )
                    allFour = false;
        }
        CHECK( ok && allFour,
               "B2.2b (bound page): a width-4 consumer of a width-1 producer "
               "reads that one channel on all four plugs" );
        CHECK( wide->wideRenders() > 0 && wide->narrowRenders() == 0,
               "B2.2b: …through renderPageWide(), one pass per page" );

        auto wide2 = std::make_shared<WideConsumer>( env, 4, 4,
                                                     WideConsumer::Read::Plugs );
        wide2->init();
        for( idx_t c = 0; c < 4; ++c ) wide2->setInput( c, src->linkOutput( c ) );
        auto lp = wide2->freezePage( 0, nullptr, 0, (length_t)CAP,
                                     env.getSRate(), nullptr );
        bool legacyOk = lp && lp->validAspects != 0 && lp->channels() == 4;
        for( idx_t c = 0; legacyOk && c < 4; ++c )
            legacyOk = channelIsConstantSignal( lp, c, 0 );
        CHECK( legacyOk,
               "B2.2b (plug pull): the legacy pull clamps all four plugs to "
               "channel 0 identically" );
    }

    // ---- (c) a WIDTH-4 consumer of a WIDTH-4, FOUR-PORT producer ----------
    // This is the case that makes twSampleReader's per-channel latches mean
    // something: plug c -> channel c, four genuinely different signals, no
    // clamping anywhere.
    {
        tw303aEnvironment env;
        CapturePagePool pool( 16 );
        CaptureRevalidator reval( &pool, 4 );

        auto src = std::make_shared<twSyntheticWideSource>( env, 4 );  // 4 PORTS
        src->init();
        auto wide = std::make_shared<WideConsumer>( env, 4, 4,
                                                    WideConsumer::Read::Plugs );
        wide->init();
        for( idx_t c = 0; c < 4; ++c ) wide->setInput( c, src->linkOutput( c ) );

        auto d = reval.requestGraphPages( wide, 0, 2 );
        d->wait();

        bool ok = true;
        for( int i = 0; i < 2; ++i ) {
            const offset_t pos = (offset_t)i * CAP;
            auto p = wide->getPageIfExists( pos );
            if( !p || p->validAspects == 0 || p->channels() != 4 ) { ok = false; break; }
            for( idx_t c = 0; c < 4; ++c )
                if( !channelMatches( p, c, c, pos ) ) ok = false;
        }
        CHECK( ok, "B2.2c: plug index c yields channel c of a wide producer — the "
                   "latch index finally means what twSampleReader always implied" );

        // A width-1 consumer hanging off plug 2 gets channel 2, not channel 0:
        // the rule is min(latchIndex, channels-1), not "channel 0 unless wide".
        auto tap2 = std::make_shared<WideConsumer>( env, 1, 1,
                                                    WideConsumer::Read::Plugs );
        tap2->init();
        tap2->setInput( 0, src->linkOutput( 2 ) );
        auto p2 = tap2->freezePage( 0, nullptr, 0, (length_t)CAP,
                                    env.getSRate(), nullptr );
        CHECK( p2 && p2->validAspects != 0 && channelMatches( p2, 0, 2, 0 ),
               "B2.2c: a plug on latch 2 yields channel 2, not channel 0" );
    }

    // ---- (d) §4.4 RULE (2): the wide consumer reads the BOUND PAGE itself --
    {
        tw303aEnvironment env;
        CapturePagePool pool( 16 );
        CaptureRevalidator reval( &pool, 4 );

        auto src = std::make_shared<twSyntheticWideSource>( env );   // ONE port
        src->init();
        auto wide = std::make_shared<WideConsumer>( env, 4, 1,
                                                    WideConsumer::Read::BoundPage );
        wide->init();
        wide->setInput( 0, src->linkOutput( 0 ) );
        wide->setProducer( src );

        auto d = reval.requestGraphPages( wide, 0, 2 );
        d->wait();

        bool ok = true;
        for( int i = 0; i < 2; ++i ) {
            const offset_t pos = (offset_t)i * CAP;
            auto p = wide->getPageIfExists( pos );
            if( !p || p->validAspects == 0 || p->channels() != 4 ) { ok = false; break; }
            for( idx_t c = 0; c < 4; ++c )
                if( !channelMatches( p, c, c, pos ) ) ok = false;
        }
        CHECK( ok && wide->boundReads() > 0,
               "B2.2d (bound page, rule 2): a wide component reads the bound "
               "input page directly and picks all four channels from ONE plug" );

        // The same consumer against a WIDTH-1 producer clamps to that channel on
        // all four — the other direction of the same rule, done by the consumer
        // rather than by the plug.
        auto narrowSrc = std::make_shared<NarrowFourPortSource>( env );
        narrowSrc->init();
        auto wide2 = std::make_shared<WideConsumer>( env, 4, 1,
                                                     WideConsumer::Read::BoundPage );
        wide2->init();
        wide2->setInput( 0, narrowSrc->linkOutput( 0 ) );
        wide2->setProducer( narrowSrc );

        auto d2 = reval.requestGraphPages( wide2, 0, 1 );
        d2->wait();
        auto p = wide2->getPageIfExists( 0 );
        bool clamped = p && p->validAspects != 0 && p->channels() == 4;
        for( idx_t c = 0; clamped && c < 4; ++c )
            clamped = channelIsConstantSignal( p, c, 0 );
        CHECK( clamped && wide2->boundReads() > 0,
               "B2.2d (bound page, rule 2): …and a width-1 bound page is read on "
               "all four channels" );
    }

    // =====================================================================
    // AC B2.3 — a width > 1 component that does NOT override renderPageWide()
    // REFUSES rather than rendering. This test expects the failure.
    // =====================================================================
    {
        tw303aEnvironment env;
        const uint64_t before = twComponent::wideRenderRefusals();

        auto bad = std::make_shared<BadWideSource>( env );
        bad->init();

        auto p = bad->freezePage( 0, nullptr, 0, (length_t)CAP,
                                  env.getSRate(), nullptr );

        CHECK( p && p->channels() == 4,
               "B2.3: the page really was allocated at the declared width" );
        CHECK( twComponent::wideRenderRefusals() == before + 1,
               "B2.3: the base renderPageWide() refused, exactly once" );
        CHECK( p && p->validFrames == 0,
               "B2.3: …and published ZERO valid frames, not a plausible render" );
        CHECK( bad->narrowRenders == 0,
               "B2.3: renderFrames() was never called — the refusal did not fall "
               "back to rendering channel 0 and calling it a wide page" );

        bool silent = true;
        if( p ) {
            for( idx_t c = 0; c < 4; ++c )
                for( offset_t f : { (offset_t)0, (offset_t)1, CAP / 2, CAP - 1 } )
                    if( p->channelPtr( c )[f] != 0.0f ) silent = false;
        }
        CHECK( silent, "B2.3: every channel is silence (renderFrames' 1.0 is "
                       "nowhere in the page)" );

        // A width-1 component never comes near this path, however many pages it
        // freezes — the fork is on the page's width, and the counter proves the
        // refusal is not a global side effect of B2 existing.
        auto good = std::make_shared<NarrowFourPortSource>( env );
        good->init();
        good->freezePage( 0, nullptr, 0, (length_t)CAP, env.getSRate(), nullptr );
        good->freezePage( CAP, nullptr, 0, (length_t)CAP, env.getSRate(), nullptr );
        CHECK( twComponent::wideRenderRefusals() == before + 1,
               "B2.3: a width-1 component never enters the wide path at all" );
    }

    if( failures == 0 ) { printf( "all wide-graph tests passed\n" ); return 0; }
    printf( "%d wide-graph test(s) FAILED\n", failures );
    return 1;
}
