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

    printf( failures ? "\n%d FAILURE(S)\n" : "\nall metering tests passed\n",
            failures );
    return failures ? 1 : 0;
}
