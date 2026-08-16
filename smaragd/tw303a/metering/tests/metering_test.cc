// tw/metering module test (proposal 34): the span scan, the ballistics, and the
// page probe. The load-bearing assertion is FRAME-RATE INDEPENDENCE — the whole
// reason ballistics live on the UI thread and are driven by wall-clock dt rather
// than by tick count.
#include "tw/metering/tw_level_scan.h"
#include "tw/metering/tw_meter_ballistics.h"
#include "tw/metering/tw_level_probe.h"

#include "tw/graph/twcomponent.h"
#include "tw/graph/tw303aenv.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

static int failures = 0;
#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (cond) { printf("ok   %s\n", msg); }                             \
        else      { printf("FAIL %s\n", msg); ++failures; }                 \
    } while (0)

static bool near_( double a, double b, double eps ) { return std::fabs(a-b) <= eps; }

// A never-silent, position-identifying signal, so a wrong offset is visible as a
// wrong level rather than as silence. Mirrors mix_test's val().
static float val( long long p ) { return (float)((p % 977) + 1) / 1000.0f; }

// Scripted source: emits val(position) and advances.
class RampComponent : public twComponent {
public:
    explicit RampComponent( tw303aEnvironment &e ) : twComponent( e ) {}
    offset_t pos = 0;

    bool isSeekable() const override { return true; }
    int  seekTo( offset_t p ) override { pos = p; return 0; }
    void reset() override { pos = 0; }
    length_t renderFrames( sample_t *out, length_t n, const sample_t *,
                           length_t, idx_t ) override {
        for( length_t i = 0; i < n; ++i ) out[i] = val( (long long)(pos + i) );
        pos += (offset_t)n;
        return n;
    }
    void createOutputLatches() override {}
    idx_t getNInputs() const override { return 0; }
    idx_t getNOutputs() const override { return 1; }
    const char *getInputName( idx_t ) const override { return nullptr; }
    const char *getOutputName( idx_t ) const override { return "ramp"; }
};

// Emits a settable constant, so a re-freeze can be given a different level.
class ConstComponent : public twComponent {
public:
    ConstComponent( tw303aEnvironment &e, float v ) : twComponent( e ), v_( v ) {}
    void setValue( float v ) { v_ = v; }
    bool isSeekable() const override { return true; }
    int  seekTo( offset_t ) override { return 0; }
    void reset() override {}
    length_t renderFrames( sample_t *out, length_t n, const sample_t *,
                           length_t, idx_t ) override {
        for( length_t i = 0; i < n; ++i ) out[i] = v_;
        return n;
    }
    void createOutputLatches() override {}
    idx_t getNInputs() const override { return 0; }
    idx_t getNOutputs() const override { return 1; }
    const char *getInputName( idx_t ) const override { return nullptr; }
    const char *getOutputName( idx_t ) const override { return "const"; }
private:
    float v_;
};

// A tap whose DECLARED width can change between freezes — the shape a project
// width change gives every component (proposal 36 §4.5). It renders wide
// properly, so a miss can only ever be the width rule, never a refusal.
class WidthShiftComponent : public twComponent {
public:
    explicit WidthShiftComponent( tw303aEnvironment &e ) : twComponent( e ) {}
    void setWidth( idx_t w ) { width_ = w; }
    idx_t getOutputChannels() const override { return width_; }
    bool isSeekable() const override { return true; }
    int  seekTo( offset_t ) override { return 0; }
    void reset() override {}
    length_t renderFrames( sample_t *out, length_t n, const sample_t *,
                           length_t, idx_t ) override {
        for( length_t i = 0; i < n; ++i ) out[i] = 0.5f;
        return n;
    }
    length_t renderPageWide( twOutputPage &page, length_t frames,
                             const sample_t *, length_t ) override {
        if( frames > (length_t)page.channelFrames() )
            frames = (length_t)page.channelFrames();
        for( idx_t c = 0; c < (idx_t)page.channels(); ++c )
            for( length_t i = 0; i < frames; ++i )
                page.channelPtr( c )[i] = 0.5f;
        return frames;
    }
    void createOutputLatches() override {}
    idx_t getNInputs() const override { return 0; }
    idx_t getNOutputs() const override { return 1; }
    const char *getInputName( idx_t ) const override { return nullptr; }
    const char *getOutputName( idx_t ) const override { return "shift"; }
private:
    idx_t width_ = 1;
};

// A wide component whose channels carry DIFFERENT, position-independent levels
// (proposal 36 B8): channel c holds LADDER[c], a 6 dB ladder. A lane read from
// the wrong channel is therefore a wrong LEVEL, not silence, and a probe that
// quietly folded the channels together would land between the rungs.
static const float LADDER[8] = { 0.8f, 0.4f, 0.2f, 0.1f,
                                 0.05f, 0.025f, 0.0125f, 0.00625f };

class LadderComponent : public twComponent {
public:
    LadderComponent( tw303aEnvironment &e, idx_t w ) : twComponent( e ), width_( w ) {}
    void  setWidth( idx_t w ) { width_ = w; }
    idx_t getOutputChannels() const override { return width_; }
    bool isSeekable() const override { return true; }
    int  seekTo( offset_t ) override { return 0; }
    void reset() override {}
    // Narrow degradation (trap 18): a wide-only component handed a mono scratch
    // page must still render something rather than recurse into the base pair.
    length_t renderFrames( sample_t *out, length_t n, const sample_t *,
                           length_t, idx_t ) override {
        for( length_t i = 0; i < n; ++i ) out[i] = LADDER[0];
        return n;
    }
    length_t renderPageWide( twOutputPage &page, length_t frames,
                             const sample_t *, length_t ) override {
        if( frames > (length_t)page.channelFrames() )
            frames = (length_t)page.channelFrames();
        for( idx_t c = 0; c < (idx_t)page.channels(); ++c ) {
            sample_t *d = page.channelPtr( c );
            const float v = LADDER[ c < 8 ? c : 7 ];
            for( length_t i = 0; i < frames; ++i ) d[i] = v;
        }
        return frames;
    }
    void createOutputLatches() override {}
    idx_t getNInputs() const override { return 0; }
    idx_t getNOutputs() const override { return 1; }
    const char *getInputName( idx_t ) const override { return nullptr; }
    const char *getOutputName( idx_t ) const override { return "ladder"; }
private:
    idx_t width_;
};

int main()
{
    const offset_t CAP = (offset_t)twOutputPage::FRAME_CAPACITY;

    // ---------------------------------------------------------------- scan
    {
        std::vector<float> buf( 1000, 0.0f );
        buf[10]  =  0.5f;
        buf[900] = -0.75f;          // the max is NEGATIVE — |s|, not s
        twLevelSample s = twScanSpan( buf.data(), 1000 );
        CHECK( near_( s.peak, 0.75, 1e-6 ), "scan peak takes the absolute value" );
        CHECK( s.frames == 1000, "scan reports the frames it scanned" );
        CHECK( !s.clipped, "scan does not cry clip below the threshold" );
        const double expectMs = (0.25 + 0.5625) / 1000.0;
        CHECK( near_( s.meanSquare, expectMs, 1e-9 ), "scan mean-square is exact" );

        std::vector<float> full( 100, 1.0f );
        twLevelSample c = twScanSpan( full.data(), 100 );
        CHECK( c.clipped, "scan latches clip at 1.0" );
        CHECK( near_( c.meanSquare, 1.0, 1e-9 ), "scan mean-square of unity is 1" );

        std::vector<float> pcm( 10, 32767.0f / 32768.0f );
        CHECK( twScanSpan( pcm.data(), 10 ).clipped,
               "scan counts a 16-bit full-scale sample as a clip" );

        twLevelSample empty = twScanSpan( buf.data(), 0 );
        CHECK( empty.frames == 0, "an empty span is 'no measurement', not silence" );
        CHECK( twScanSpan( nullptr, 100 ).frames == 0, "a null span is a non-measurement" );
    }

    // ---------------------------------------------------------- ballistics
    {
        twMeterBallistics b;
        CHECK( near_( b.peakDb(), -60.0, 1e-6 ), "a fresh meter sits at the floor" );

        twLevelSample s;
        s.peak = 0.5f; s.meanSquare = 0.25f; s.frames = 100;   // -6.02 dB
        b.push( s, 0.0 );
        CHECK( near_( b.peakDb(), -6.0206, 1e-3 ), "attack is instantaneous" );
        CHECK( near_( b.holdDb(), -6.0206, 1e-3 ), "the hold tick follows the attack up" );

        // 1 s of silence at 20 dB/s: -6.02 -> -26.02
        b.idle( 1.0 );
        CHECK( near_( b.peakDb(), -26.0206, 1e-3 ), "peak falls at 20 dB/s" );
        CHECK( near_( b.holdDb(), -6.0206, 1e-3 ), "the hold tick holds for 1.5 s" );

        // 0.5 s later the hold expires exactly; 0.5 s past that it has fallen 6 dB
        b.idle( 2.0 );
        CHECK( near_( b.holdDb(), -12.0206, 1e-3 ),
               "the hold tick falls at 12 dB/s once expired, from the deadline" );

        // The floor is a floor
        for( int i = 0; i < 20; ++i ) b.idle( 3.0 + i );
        CHECK( near_( b.peakDb(), -60.0, 1e-6 ), "peak cannot fall below the floor" );
        CHECK( near_( b.holdDb(), -60.0, 1e-6 ), "the hold tick cannot either" );
    }

    // ------------------------------------------------- clip latch semantics
    {
        twMeterBallistics b;
        twLevelSample loud; loud.peak = 1.0f; loud.meanSquare = 1.0f;
        loud.frames = 100; loud.clipped = true;
        b.push( loud, 0.0 );
        CHECK( b.clipped(), "clip latches" );

        twLevelSample quiet; quiet.peak = 0.01f; quiet.meanSquare = 0.0001f;
        quiet.frames = 100;
        for( int i = 1; i <= 200; ++i ) b.push( quiet, i * 0.033 );
        CHECK( b.clipped(), "clip survives quiet material — it is not time-based" );
        b.clearClip();
        CHECK( !b.clipped(), "clearClip clears it" );

        b.reset();
        CHECK( !b.clipped() && near_( b.peakDb(), -60.0, 1e-6 ),
               "reset returns to floor with the clip cleared" );
    }

    // ------------------------------------- frame-rate independence (the point)
    {
        twLevelSample s;
        s.peak = 0.5f; s.meanSquare = 0.25f; s.frames = 100;

        twMeterBallistics one, many;
        one.push( s, 0.0 );
        many.push( s, 0.0 );

        one.idle( 1.0 );                                        // one 1 s step
        for( int i = 1; i <= 100; ++i ) many.idle( i * 0.01 );  // 100 x 10 ms

        CHECK( near_( one.peakDb(), many.peakDb(), 1e-4 ),
               "peak decay is independent of tick rate" );
        CHECK( near_( one.rmsDb(), many.rmsDb(), 1e-4 ),
               "the RMS integrator is independent of tick rate" );
        CHECK( near_( one.holdDb(), many.holdDb(), 1e-4 ),
               "the hold tick is independent of tick rate" );
    }

    // ------------------------------------------------ RMS integrator target
    {
        twMeterBallistics b;
        twLevelSample s;
        s.peak = 0.5f; s.meanSquare = 0.25f; s.frames = 100;    // RMS 0.5 = -6.02 dB
        // Drive it for 10 tau; a one-pole is within 0.005 % by then.
        for( int i = 0; i <= 300; ++i ) b.push( s, i * 0.01 );
        CHECK( near_( b.rmsDb(), -6.0206, 1e-2 ),
               "the RMS integrator converges to the true RMS, not to the power" );
    }

    // ----------------------------------------------- a non-measurement idles
    {
        twMeterBallistics b;
        twLevelSample s;
        s.peak = 0.5f; s.meanSquare = 0.25f; s.frames = 100;
        b.push( s, 0.0 );
        twLevelSample none;                      // frames == 0
        b.push( none, 1.0 );
        CHECK( near_( b.peakDb(), -26.0206, 1e-3 ),
               "pushing a non-measurement decays instead of reading as silence-at-0" );
    }

    // ------------------------------------------------------------ the probe
    tw303aEnvironment env;
    {
        // The probe's only requirement on a tap is that the base
        // twComponent::freezePage CACHES its pages, which is exactly why the app
        // taps a track's twRewire root and not twTrackMix (fresh page per call)
        // or twPluginChain (forwards, renders nothing). Any plain twComponent
        // subclass exercises that property, and using one keeps this test
        // linking tw_metering ONLY — per the module-test convention, a test that
        // stops linking should itself be the layering regression.
        auto ramp = std::make_shared<RampComponent>( env );
        ramp->init();

        twLevelProbe probe;
        twLevelSample s;

        CHECK( !probe.advanceTo( 1000, s ), "a probe with no tap misses" );
        probe.setTap( ramp );

        CHECK( !probe.advanceTo( 1000, s ),
               "no frozen page at the position is a miss, not silence" );
        CHECK( probe.missCount() == 2, "misses are counted" );

        // Freeze page 0 and probe inside it.
        auto p0 = ramp->freezePage( 0, nullptr, 0, CAP, env.getSRate(), nullptr );
        CHECK( p0 && p0->validAspects != 0, "the tap froze and cached page 0" );

        probe.reset();
        CHECK( probe.advanceTo( 5000, s ), "a frozen page mid-page reads" );
        // No history -> MIN_WINDOW: the span is [5000-256, 5000).
        {
            twLevelSample expect = twScanSpan( &p0->channelPtr(0)[5000 - 256], 256 );
            CHECK( near_( s.peak, expect.peak, 1e-6 ) && s.frames == 256,
                   "the first read measures MIN_WINDOW ending at the position" );
        }

        // Advancing continues where the last read stopped.
        CHECK( probe.advanceTo( 6600, s ), "a continued read succeeds" );
        CHECK( s.frames == 1600, "the window is exactly what elapsed" );
        {
            twLevelSample expect = twScanSpan( &p0->channelPtr(0)[5000], 1600 );
            CHECK( near_( s.peak, expect.peak, 1e-6 ),
                   "the continued window starts where the previous one ended" );
        }

        // A jump longer than MAX_WINDOW is capped, not honoured.
        CHECK( probe.advanceTo( 40000, s ), "a long jump still reads" );
        CHECK( s.frames == (uint32_t)twLevelProbe::MAX_WINDOW,
               "a starved pump measures at most MAX_WINDOW" );

        // A backwards seek falls back to MIN_WINDOW.
        CHECK( probe.advanceTo( 20000, s ), "a backwards seek still reads" );
        CHECK( s.frames == (uint32_t)twLevelProbe::MIN_WINDOW,
               "a backwards seek measures MIN_WINDOW" );

        // A static position (stopped transport) also reads MIN_WINDOW, and does
        // not report a growing window forever.
        CHECK( probe.advanceTo( 20000, s ) && s.frames == (uint32_t)twLevelProbe::MIN_WINDOW,
               "a static position measures MIN_WINDOW" );

        // Position 0 has nothing behind it.
        probe.reset();
        CHECK( !probe.advanceTo( 0, s ), "position 0 has no elapsed window" );

        // A page boundary: the window is clamped to the page holding its start,
        // so it never reads across into a page that may not exist.
        probe.reset();
        probe.advanceTo( CAP - 1000, s );
        CHECK( probe.advanceTo( CAP + 500, s ),
               "a window straddling a page boundary still reads" );
        CHECK( s.frames == 1000,
               "a straddling window is clamped to the page holding its start" );

        // Beyond the last frozen page: a miss (the next page was never frozen).
        probe.reset();
        CHECK( !probe.advanceTo( CAP + 5000, s ),
               "a position in an unfrozen page is a miss" );

        // Ladder step 2: a stale-but-frozen page is ACCEPTED, because that is
        // what playback is serving while the re-freeze is in flight.
        ramp->bumpContentEpoch();
        CHECK( p0->contentEpoch.load() < ramp->contentEpochNow(),
               "the page is now stale" );
        probe.reset();
        CHECK( probe.advanceTo( 5000, s ),
               "a stale-but-frozen page is still read (proposal 16 parity)" );
    }

    // ---------------------------- the probe re-reads; it caches no measurement
    {
        auto src = std::make_shared<ConstComponent>( env, 0.5f );
        src->init();

        twLevelProbe probe;
        probe.setTap( src );

        src->freezePage( 0, nullptr, 0, CAP, env.getSRate(), nullptr );
        twLevelSample loud;
        CHECK( probe.advanceTo( 5000, loud ) && near_( loud.peak, 0.5, 1e-4 ),
               "the probe reads the tap's actual level" );

        // Change the audio and re-freeze: the next read must reflect the new
        // page, not the one the probe was holding. (End-to-end, this is what
        // makes a fader move visible on the meter — asserted in meter_levels.qxa
        // via the -6 dB halving, since the app's tap sits downstream of the
        // track gain.)
        src->setValue( 0.25f );
        src->bumpContentEpoch();
        src->freezePage( 0, nullptr, 0, CAP, env.getSRate(), nullptr );
        twLevelSample quiet;
        probe.reset();
        CHECK( probe.advanceTo( 5000, quiet ) && near_( quiet.peak, 0.25, 1e-4 ),
               "a re-frozen page is re-read, not served from the held page" );
    }

    // ------------------------------------ §4.5: WIDTH MISMATCH IS A MISS
    // Proposal 36 §4.5, wired by B2. The probe deliberately accepts STALE pages
    // (the block above), because playback is serving exactly those. A page from
    // before a project WIDTH change is a different thing: not older audio, a
    // different geometry. Reading it would mean reading channel 1 of a page that
    // has only channel 0 — an out-of-bounds read, and on the RT path an
    // out-of-bounds read on the audio thread. So it is a MISS, which decays the
    // meter, which is the correct reading for a position being re-frozen.
    //
    // (The RT-path half of this rule is wired in AudioEngine::updateFrozenPage
    // and is B4's AC B4.5 to prove; nothing in the production graph is wider
    // than one channel at B2, so there is no width change to force there yet.)
    {
        auto src = std::make_shared<WidthShiftComponent>( env );
        src->init();

        twLevelProbe probe;
        probe.setTap( src );

        auto page = src->freezePage( 0, nullptr, 0, CAP, env.getSRate(), nullptr );
        CHECK( page && page->channels() == 1,
               "the page was frozen at the tap's width at the time" );

        twLevelSample s;
        CHECK( probe.advanceTo( 5000, s ),
               "…and reads normally while the widths agree" );

        const uint64_t missesBefore = probe.missCount();

        // THE WIDTH CHANGE. The cached page keeps its geometry — channels is
        // immutable after allocation — so the two now disagree.
        src->setWidth( 2 );
        probe.reset();
        CHECK( !probe.advanceTo( 5000, s ),
               "a cached page whose width no longer matches its producer is a "
               "MISS, not audio" );
        CHECK( probe.missCount() > missesBefore,
               "…and it is counted as a miss, so the meter decays" );

        // Not a permanent poisoning: re-freezing at the new width restores the
        // reading (the epoch bump is what makes freezePage allocate again).
        src->bumpContentEpoch();
        auto wide = src->freezePage( 0, nullptr, 0, CAP, env.getSRate(), nullptr );
        CHECK( wide && wide->channels() == 2,
               "the re-freeze allocates at the NEW declared width" );
        probe.reset();
        CHECK( probe.advanceTo( 5000, s ),
               "…and the position reads again once the widths agree" );
    }

    // ============================================================ proposal 36 B8
    // N-LANE METERING. twLevelSample and twScanSpan stay scalar; a set is N of
    // them, and the probe is the only thing that knows which channel is which.

    // ------------------------------------------- the probe reads N real lanes
    {
        auto src = std::make_shared<LadderComponent>( env, 4 );
        src->init();
        src->freezePage( 0, nullptr, 0, CAP, env.getSRate(), nullptr );

        twLevelProbe probe;
        probe.setTap( src );

        twLevelSampleSet set;
        CHECK( probe.advanceTo( 5000, set, 4 ), "a wide tap reads" );
        CHECK( set.lanes == 4, "four channels give four lanes" );

        bool ladderOk = true;
        for( int c = 0; c < 4; ++c ) {
            if( !near_( set.lane[c].peak, LADDER[c], 1e-4 ) ) ladderOk = false;
            if( !near_( std::sqrt( (double)set.lane[c].meanSquare ),
                        LADDER[c], 1e-4 ) ) ladderOk = false;
            if( set.lane[c].frames != (uint32_t)twLevelProbe::MIN_WINDOW )
                ladderOk = false;
        }
        CHECK( ladderOk,
               "each lane reads ITS OWN channel — a 6 dB ladder, in order" );

        // A folded or duplicated read would make the lanes equal; say so
        // separately, because the ladder check above could pass on a fold that
        // happened to land on rung 0.
        CHECK( set.lane[0].peak > set.lane[1].peak &&
               set.lane[1].peak > set.lane[2].peak &&
               set.lane[2].peak > set.lane[3].peak,
               "the lanes are genuinely different, strictly descending" );

        // The scalar overload and lane 0 are ONE implementation: they cannot
        // disagree, and this is what pins that.
        twLevelSample scalar;
        probe.reset();
        CHECK( probe.advanceTo( 5000, scalar ) &&
               near_( scalar.peak, set.lane[0].peak, 1e-6 ) &&
               near_( scalar.meanSquare, set.lane[0].meanSquare, 1e-9 ),
               "the scalar advanceTo is exactly lane 0 of the set form" );

        // §4.4 both ways. Asking for FEWER lanes than the page has stops at
        // what was asked; asking for MORE stops at the page.
        probe.reset();
        CHECK( probe.advanceTo( 5000, set, 1 ) && set.lanes == 1 &&
               near_( set.lane[0].peak, LADDER[0], 1e-4 ),
               "a one-lane caller of a four-channel page reads channel 0 only" );
        probe.reset();
        CHECK( probe.advanceTo( 5000, set, 8 ) && set.lanes == 4,
               "asking for more lanes than the page has yields the page's width" );
        probe.reset();
        CHECK( probe.advanceTo( 5000, set, 0 ) && set.lanes == 1,
               "a non-positive lane request is read as one lane" );
    }

    // ------------------------- §4.4: THE PAGE IN HAND, not the declared width
    {
        // A width-1 producer asked for four lanes yields ONE. This is the rule
        // that keeps channelPtr(1) of a mono page from ever being read: an
        // insert-less twPluginChain forwards its input page verbatim and its
        // silence pages are default-constructed width 1, so a wide consumer
        // legitimately meets a narrow page.
        auto narrow = std::make_shared<ConstComponent>( env, 0.5f );
        narrow->init();
        narrow->freezePage( 0, nullptr, 0, CAP, env.getSRate(), nullptr );

        twLevelProbe probe;
        probe.setTap( narrow );
        twLevelSampleSet set;
        CHECK( probe.advanceTo( 5000, set, 4 ) && set.lanes == 1 &&
               near_( set.lane[0].peak, 0.5, 1e-4 ),
               "a width-1 page gives ONE lane however many were asked for" );
        CHECK( set.lane[1].frames == 0 && set.lane[3].frames == 0,
               "lanes the page does not carry stay 'no measurement', not silence" );
        CHECK( near_( set.first().peak, 0.5, 1e-4 ),
               "first() answers lane 0" );

        twLevelSampleSet empty;
        CHECK( empty.lanes == 0 && empty.first().frames == 0,
               "an empty set's first() is a non-measurement, not silence-at-0" );
    }

    // --------------------- PER-LANE ballistics stay frame-rate independent
    // The load-bearing property of proposal 34, restated per lane: this is the
    // whole reason ballistics live on the UI thread driven by wall-clock dt, and
    // an N-lane meter is N of them stepped together, so it has to hold lane by
    // lane. The lanes are fed DIFFERENT levels on purpose — an implementation
    // that fed lane 0's sample to every lane would pass a same-level version of
    // this test.
    {
        const int N = 4;
        twMeterBallistics one[N], many[N];

        twLevelSampleSet s;
        s.lanes = N;
        for( int c = 0; c < N; ++c ) {
            s.lane[c].peak       = LADDER[c];
            s.lane[c].meanSquare = LADDER[c] * LADDER[c];
            s.lane[c].frames     = 100;
        }

        for( int c = 0; c < N; ++c ) { one[c].push( s.lane[c], 0.0 );
                                       many[c].push( s.lane[c], 0.0 ); }

        for( int c = 0; c < N; ++c ) one[c].idle( 1.0 );            // one 1 s step
        for( int i = 1; i <= 100; ++i )                             // 100 x 10 ms
            for( int c = 0; c < N; ++c ) many[c].idle( i * 0.01 );

        bool peakOk = true, rmsOk = true, holdOk = true, distinct = true;
        for( int c = 0; c < N; ++c ) {
            if( !near_( one[c].peakDb(), many[c].peakDb(), 1e-4 ) ) peakOk = false;
            if( !near_( one[c].rmsDb(),  many[c].rmsDb(),  1e-4 ) ) rmsOk  = false;
            if( !near_( one[c].holdDb(), many[c].holdDb(), 1e-4 ) ) holdOk = false;
            if( c > 0 && !( one[c].peakDb() < one[c-1].peakDb() - 1.0 ) )
                distinct = false;
        }
        CHECK( peakOk, "per-lane peak decay is independent of tick rate" );
        CHECK( rmsOk,  "per-lane RMS integration is independent of tick rate" );
        CHECK( holdOk, "per-lane hold tick is independent of tick rate" );
        CHECK( distinct,
               "the lanes really did carry different levels (the test can fail)" );
    }

    // ------------------------- a wide tap ALSO obeys §4.5 (width is a miss)
    {
        auto src = std::make_shared<LadderComponent>( env, 4 );
        src->init();
        src->freezePage( 0, nullptr, 0, CAP, env.getSRate(), nullptr );

        twLevelProbe probe;
        probe.setTap( src );
        twLevelSampleSet set;
        CHECK( probe.advanceTo( 5000, set, 4 ) && set.lanes == 4,
               "the four-lane read works before the width change" );

        // Widen the PRODUCER without re-freezing. The cached page keeps its
        // geometry (channels is immutable after allocation), so it now predates
        // a width change — a MISS, never audio, on the N-lane path too. Reading
        // channelPtr(4) of a four-channel page is the out-of-bounds read §4.5
        // exists to forbid. The rule lives in resolvePage_, which both overloads
        // share, and this is what pins that sharing.
        src->setWidth( 6 );
        probe.reset();
        CHECK( !probe.advanceTo( 5000, set, 6 ),
               "a page that predates a width change is a MISS in the set form too" );
        CHECK( set.lanes == 0, "…and the set reports no measurement at all" );

        src->bumpContentEpoch();
        auto refrozen = src->freezePage( 0, nullptr, 0, CAP, env.getSRate(), nullptr );
        CHECK( refrozen && refrozen->channels() == 6,
               "the re-freeze allocates at the NEW declared width" );
        probe.reset();
        CHECK( probe.advanceTo( 5000, set, 6 ) && set.lanes == 6 &&
               near_( set.lane[5].peak, LADDER[5], 1e-4 ),
               "…and six lanes read, the sixth carrying rung 5" );
    }

    printf( failures ? "\n%d FAILURE(S)\n" : "\nall metering tests passed\n",
            failures );
    return failures ? 1 : 0;
}
