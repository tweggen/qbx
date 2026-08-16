// Gate for the CONTAINER branch of SObject::straightCalcPreviewData() — the
// preview of an object whose content is a live sub-arrangement (a track/mixer
// subtree) rather than random-access sample data.
//
// That branch used to seek the live component and pull calcOutputTo() once per
// preview window, writing a shared play cursor from the paint path. It now
// reads the component's FROZEN PAGES. The properties worth pinning are exactly
// the ones a page rewrite can get wrong and a screenshot would never localise:
//
//   1. VALUES: one preview probe is still the signed min/max envelope of its
//      whole `previewSkip_` window, scaled to [-128,127] — unchanged by the
//      rewrite. The fixture emits a position-identifying signal, so a page
//      read at the wrong offset shows up as a wrong peak, not as silence.
//   2. PAGE BOUNDARIES: probes are checked inside page 0, straddling the
//      page 0/1 boundary, and deep inside pages 1 and 2 — the classic bug is
//      indexing a page with an absolute position.
//   3. STATE CHAINING: the pages are frozen as a sequential chain (page N-1 is
//      handed to page N as previousPage), so a stateful container resumes
//      instead of restarting at every page. The fixture counts its reset()s:
//      exactly one, for page 0's discontinuity.
//   4. ONE REPOSITION PER PAGE: the fixture counts its seekTo()s. The freeze
//      path positions it once per PAGE (4 here); the retired live-cursor path
//      seeked it once per preview WINDOW (~772), so the count is a direct
//      discriminator between the two.
//
// No project, no audio device, no display: an SObject parented to no project
// falls back to 48 kHz, which is all the freeze needs here.

#include "app/model/sobject.h"

#include "tw/graph/tw303aenv.h"
#include "tw/graph/twcomponent.h"
#include "tw/pages/tw_output_page.h"

#include <QApplication>

#include <cmath>
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

// A never-silent, position-identifying signal in [-1,1]: reading the wrong
// frame is a wrong VALUE, not silence. Mirrors metering_test's val().
float val( long long p )
{
    return (float) ( ( ( p % 977 ) + 1 ) / 977.0 * 2.0 - 1.0 );
}

// The container fixture. Renders val(position) from the position it was seeked
// to, counts its reset()s (state chaining) and refuses external seeks.
class ContainerComponent : public twComponent {
public:
    explicit ContainerComponent( tw303aEnvironment &e ) : twComponent( e ) {}

    offset_t pos = 0;
    int resets = 0;
    int seeks = 0;

    bool isSeekable() const override { return true; }
    int  seekTo( offset_t p ) override { ++seeks; pos = p; return 0; }
    void reset() override { ++resets; pos = 0; }

    length_t renderFrames( sample_t *out, length_t n, const sample_t *,
                           length_t, idx_t ) override
    {
        for( length_t i = 0; i < n; ++i ) out[i] = val( (long long)( pos + i ) );
        pos += (offset_t) n;
        return n;
    }

    // Chained pages restore this instead of reset()ing.
    std::any captureInternalState() const override { return pos; }
    void restoreInternalState( const std::any &s ) override
    {
        if( s.has_value() && s.type() == typeid( offset_t ) )
            pos = std::any_cast<offset_t>( s );
    }

    void createOutputLatches() override {}
    idx_t getNInputs() const override { return 0; }
    idx_t getNOutputs() const override { return 1; }
    const char *getInputName( idx_t ) const override { return nullptr; }
    const char *getOutputName( idx_t ) const override { return "container"; }
};

// The model object: has a duration, has NO random source — the branch under
// test — and hands out the component above as its render root.
class ContainerObject : public SObject {
public:
    ContainerObject( tw303aEnvironment &env, length_t duration )
        : SObject( nullptr ),
          comp_( std::make_shared<ContainerComponent>( env ) ),
          duration_( duration )
    {}

    std::shared_ptr<twComponent> getRootComponent() override { return comp_; }
    bool hasDuration() const override { return true; }
    length_t getDuration() const override { return duration_; }
    bool hasPreview() const override { return true; }

    QWidget *getDetailEditWidget( QWidget * ) override { return nullptr; }
    QWidget *getInlineEditWidget( QWidget * ) override { return nullptr; }
    SObjectRenderer *getInlineRenderer() override { return nullptr; }

    ContainerComponent &component() { return *comp_; }

private:
    std::shared_ptr<ContainerComponent> comp_;
    length_t duration_;
};

// The WIDE container fixture (proposal 36 B8). Channel 0 carries a QUARTER of
// channel 1's signal, so a preview that folds only channel 0 — the pre-B8 rule —
// draws a waveform a quarter the height of the audio it describes. That is the
// exact lie B8's all-channel fold removes, and the fixtures in tests/ cannot
// show it: test_sawtooth's channels are identical and test_stereo /
// test_channels4 are descending ladders whose channel 0 dominates, so for all
// three the fold is invisible. Hence a fixture built the other way up.
class WideContainerComponent : public twComponent {
public:
    explicit WideContainerComponent( tw303aEnvironment &e ) : twComponent( e ) {}

    offset_t pos = 0;

    idx_t getOutputChannels() const override { return 2; }
    bool  isSeekable() const override { return true; }
    int   seekTo( offset_t p ) override { pos = p; return 0; }
    void  reset() override { pos = 0; }

    // Narrow degradation (proposal 36 trap 18): a wide-only component handed a
    // mono scratch page walks into base renderFrames/calcOutputTo recursion.
    length_t renderFrames( sample_t *out, length_t n, const sample_t *,
                           length_t, idx_t ) override
    {
        for( length_t i = 0; i < n; ++i )
            out[i] = 0.25f * val( (long long)( pos + i ) );
        pos += (offset_t) n;
        return n;
    }

    length_t renderPageWide( twOutputPage &page, length_t frames,
                             const sample_t *, length_t ) override
    {
        if( frames > (length_t) page.channelFrames() )
            frames = (length_t) page.channelFrames();
        const idx_t nc = (idx_t) page.channels();
        for( idx_t c = 0; c < nc; ++c ) {
            sample_t *d = page.channelPtr( c );
            const float gain = ( c == 0 ) ? 0.25f : 1.0f;
            for( length_t i = 0; i < frames; ++i )
                d[i] = gain * val( (long long)( pos + i ) );
        }
        // ONE cursor advance for the whole page, whatever its width (§4.3).
        pos += (offset_t) frames;
        return frames;
    }

    std::any captureInternalState() const override { return pos; }
    void restoreInternalState( const std::any &s ) override
    {
        if( s.has_value() && s.type() == typeid( offset_t ) )
            pos = std::any_cast<offset_t>( s );
    }

    void createOutputLatches() override {}
    idx_t getNInputs() const override { return 0; }
    idx_t getNOutputs() const override { return 1; }
    const char *getInputName( idx_t ) const override { return nullptr; }
    const char *getOutputName( idx_t ) const override { return "wide"; }
};

class WideContainerObject : public SObject {
public:
    WideContainerObject( tw303aEnvironment &env, length_t duration )
        : SObject( nullptr ),
          comp_( std::make_shared<WideContainerComponent>( env ) ),
          duration_( duration )
    {}

    std::shared_ptr<twComponent> getRootComponent() override { return comp_; }
    bool hasDuration() const override { return true; }
    length_t getDuration() const override { return duration_; }
    bool hasPreview() const override { return true; }

    QWidget *getDetailEditWidget( QWidget * ) override { return nullptr; }
    QWidget *getInlineEditWidget( QWidget * ) override { return nullptr; }
    SObjectRenderer *getInlineRenderer() override { return nullptr; }

private:
    std::shared_ptr<WideContainerComponent> comp_;
    length_t duration_;
};

// The preview's own scaling, restated here so the test asserts the CONTRACT
// (signed envelope in [-128,127]) rather than re-running the implementation.
// `gain` scales the source before the envelope is taken, so the same helper
// expresses "what channel 0 alone would have produced".
preview_t expectedProbe( offset_t windowStart, offset_t skip, double gain = 1.0 )
{
    double mn = SAMPLE_NORM_MAX, mx = SAMPLE_NORM_MIN;
    for( offset_t j = 0; j < skip; ++j ) {
        double a = gain * val( (long long)( windowStart + j ) );
        if( a < mn ) mn = a;
        if( a > mx ) mx = a;
    }
    double mxs = ( mx * 127. ) / SAMPLE_NORM_MAX;
    if( mxs > 127. ) mxs = 127.;
    if( mxs < -128. ) mxs = -128.;
    double mns = ( mn * -127. ) / SAMPLE_NORM_MIN;
    if( mns > 127. ) mns = 127.;
    if( mns < -128. ) mns = -128.;
    preview_t p;
    p.min = (char) mns;
    p.max = (char) mxs;
    return p;
}

} // namespace

int main( int argc, char **argv )
{
    QApplication app( argc, argv );

    tw303aEnvironment env;
    env.setSRate( 48000 );

    const offset_t CAP  = (offset_t) twOutputPage::FRAME_CAPACITY;
    // Long enough that straightCalcPreviewData keeps its default 256-frame
    // window (it halves only below 256*128 frames) and spans four pages.
    const length_t DUR  = (length_t)( 3 * CAP + 1000 );
    const offset_t SKIP = 256;

    ContainerObject obj( env, DUR );

    // Probe positions: inside page 0, the last window of page 0, the first
    // window of page 1 (the boundary), mid page 1, mid page 2, and the tail
    // page. A page indexed by absolute position fails everything past page 0.
    const offset_t probes[] = {
        0,
        SKIP * 3,
        CAP - SKIP,
        CAP,
        CAP + SKIP * 7,
        2 * CAP + SKIP * 11,
        3 * CAP + 512,
    };

    for( offset_t start : probes ) {
        preview_t got;
        got.min = 0;
        got.max = 0;
        const int res = obj.getPreview( &got, start, SKIP, 1 );
        char what[128];
        std::snprintf( what, sizeof( what ),
                       "container preview probe at frame %lld",
                       (long long) start );
        if( res < 0 ) {
            std::printf( "FAIL %s (getPreview returned %d)\n", what, res );
            ++g_failures;
            continue;
        }
        const preview_t want = expectedProbe( start, SKIP );
        if( got.min == want.min && got.max == want.max ) {
            std::printf( "ok   %s (min=%d max=%d)\n", what,
                         (int) got.min, (int) got.max );
        } else {
            std::printf( "FAIL %s: expected min=%d max=%d, got min=%d max=%d\n",
                         what, (int) want.min, (int) want.max,
                         (int) got.min, (int) got.max );
            ++g_failures;
        }
    }

    // A container preview is a real signal, not a flat line: if the whole thing
    // read as silence every probe above would still agree with a broken
    // expectedProbe(), so pin the envelope's amplitude once.
    {
        preview_t got;
        got.min = 0;
        got.max = 0;
        obj.getPreview( &got, CAP + SKIP * 7, SKIP, 1 );
        // .min is the LOWER (negative) edge, .max the upper one — the signed
        // envelope convention swaveformdraw draws and SCut::ensureCapturePeaks
        // matches. This window straddles the fixture's wrap, so it holds both.
        check( got.min < -100 && got.max > 100,
               "the container preview carries a full-scale signed envelope" );
    }

    // The whole point of the rewrite: the pages were frozen as a chain, so the
    // stateful container reset exactly ONCE (page 0's discontinuity), and
    // nothing seeked it from outside a freeze.
    check( obj.component().resets == 1,
           "the preview freezes pages as a chain (one reset, not one per page)" );
    // 4 pages cover DUR; the retired path seeked once per 256-frame window.
    const int windows = (int)( DUR / SKIP );
    check( obj.component().seeks <= 8 && obj.component().seeks < windows / 4,
           "the component is repositioned per PAGE, not per preview window" );
    std::printf( "     (seekTo calls: %d; the live-cursor path made ~%d)\n",
                 obj.component().seeks, windows );

    // ================================================ proposal 36 B8: the fold
    // A preview probe is the signed envelope over ALL CHANNELS. The fixture puts
    // the loud material on channel 1, so the pre-B8 channel-0 rule would draw a
    // quarter-height waveform for full-scale audio.
    {
        WideContainerObject wide( env, DUR );

        const offset_t at = CAP + SKIP * 7;      // deep inside page 1
        preview_t got;
        got.min = 0;
        got.max = 0;
        const int res = wide.getPreview( &got, at, SKIP, 1 );
        check( res >= 0, "the wide container previews at all" );

        const preview_t all = expectedProbe( at, SKIP, 1.0 );    // ch1 dominates
        const preview_t ch0 = expectedProbe( at, SKIP, 0.25 );   // the old rule

        check( got.min == all.min && got.max == all.max,
               "a preview probe folds EVERY channel (the loud one is channel 1)" );
        check( !( got.min == ch0.min && got.max == ch0.max ),
               "…and is NOT what channel 0 alone would have given" );
        std::printf( "     (all-channel min=%d max=%d; channel 0 only would be "
                     "min=%d max=%d; got min=%d max=%d)\n",
                     (int) all.min, (int) all.max, (int) ch0.min, (int) ch0.max,
                     (int) got.min, (int) got.max );

        // The two really are different, or the check above proves nothing.
        // .max is the upper edge and .min the lower (negative) one, both signed,
        // so a quarter-scale envelope is strictly INSIDE the full-scale one.
        check( ch0.max < all.max && ch0.min > all.min,
               "the fixture can tell the two folds apart" );
    }

    if( g_failures ) {
        std::printf( "\n%d check(s) failed\n", g_failures );
        return 1;
    }
    std::printf( "\nall container-preview checks passed\n" );
    return 0;
}
