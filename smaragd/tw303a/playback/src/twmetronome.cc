#include "tw/playback/twmetronome.h"

#include "tw/core/twlog.h"

#include <algorithm>
#include <cmath>

namespace {

// Floored division for a 128-bit numerator over a positive denominator. The
// count-in runs BEFORE the record position and may run before frame 0, so
// every conversion here has to be correct for negatives — C's truncating
// division would put beat -1 and beat 0 on the same frame.
inline std::int64_t floorDiv( __int128 a, std::int64_t b )
{
    __int128 q = a / b;
    if( ( a % b ) != 0 && ( ( a < 0 ) != ( b < 0 ) ) ) --q;
    return (std::int64_t) q;
}

}  // namespace

twMetronomeSource::twMetronomeSource( const twMetronomeConfig &cfg )
    : cfg_( cfg )
{
    if( cfg_.sampleRate <= 0 ) cfg_.sampleRate = 48000;

    // THE BEAT, from the ONE tempo authority. A beat is one note of the time
    // signature's denominator, so a quarter in 4/4 and an eighth in 6/8 —
    // which is what every DAW's click does and what makes "beats per bar" the
    // numerator without further arithmetic.
    const Fraction quarter = cfg_.tempo.quarterNoteFrames( cfg_.sampleRate );
    const int32_t  den     = cfg_.tempo.denominator() > 0 ? cfg_.tempo.denominator() : 4;
    const Fraction beat    = quarter * Fraction( 4, den );
    beatNum_ = beat.numerator;
    beatDen_ = beat.denominator;
    if( beatNum_ <= 0 || beatDen_ <= 0 ) {
        // A tempo map cannot produce this, but a zero beat length would be an
        // infinite loop below rather than a wrong noise.
        TW_LOGW( "playback", "[metronome] degenerate beat length %lld/%lld; "
                             "falling back to one second",
                 (long long) beatNum_, (long long) beatDen_ );
        beatNum_ = cfg_.sampleRate;
        beatDen_ = 1;
    }
    beatsPerBar_ = cfg_.tempo.numerator() > 0 ? cfg_.tempo.numerator() : 4;

    clickFrames_ = (std::size_t) std::max<double>(
        1.0, cfg_.clickMs * cfg_.sampleRate / 1000.0 );
    // Never longer than one beat: at 300 BPM the clicks would otherwise run
    // into each other and the onset detector would see one continuous tone.
    const std::int64_t oneBeat = frameOfBeat( 1 ) - frameOfBeat( 0 );
    if( oneBeat > 0 && (std::int64_t) clickFrames_ > oneBeat )
        clickFrames_ = (std::size_t) oneBeat;

    buildClick_( accentWave_, cfg_.accentHz,
                 cfg_.accentBars ? cfg_.accentLevel : cfg_.beatLevel );
    buildClick_( beatWave_,   cfg_.beatHz,   cfg_.beatLevel );
}

// A decaying sine, rendered ONCE. Precomputing it is what makes the click
// identical however a block boundary falls across it, and what makes pull()
// allocation-free and free of transcendental calls on the pump thread.
void twMetronomeSource::buildClick_( std::vector<float> &dst, double hz,
                                     float level ) const
{
    dst.assign( clickFrames_, 0.0f );
    if( clickFrames_ == 0 ) return;
    const double w = 2.0 * 3.14159265358979323846 * hz / (double) cfg_.sampleRate;
    // -60 dB by the end of the click, so the tail is inaudible and the shape
    // does not depend on the length the caller chose.
    const double tau = (double) clickFrames_ / 6.9077552789821;   // ln(1000)
    for( std::size_t i = 0; i < clickFrames_; ++i ) {
        const double env = std::exp( -(double) i / tau );
        dst[i] = (float) ( level * env * std::sin( w * (double) i ) );
    }
}

offset_t twMetronomeSource::frameOfBeat( std::int64_t k ) const
{
    return (offset_t) ( cfg_.gridOrigin
                        + floorDiv( (__int128) k * beatNum_, beatDen_ ) );
}

std::int64_t twMetronomeSource::beatAtOrBefore( offset_t pos ) const
{
    const std::int64_t rel = (std::int64_t) pos - (std::int64_t) cfg_.gridOrigin;
    return floorDiv( (__int128) rel * beatDen_, beatNum_ );
}

bool twMetronomeSource::isAccent( std::int64_t k ) const
{
    if( !cfg_.accentBars || beatsPerBar_ <= 1 ) return false;
    std::int64_t m = k % beatsPerBar_;
    if( m < 0 ) m += beatsPerBar_;
    return m == 0;
}

std::size_t twMetronomeSource::pull( float *const *out, std::size_t channels,
                                     std::size_t frames, offset_t pos )
{
    if( !out || channels == 0 || frames == 0 ) return frames;
    if( clickFrames_ == 0 ) return frames;

    // EVERY beat whose click OVERLAPS this block, which includes the one that
    // started before it: a click is `clickFrames_` long and the block grid does
    // not know about it. Starting at `pos - clickFrames_ + 1` is what makes a
    // straddling click continue seamlessly without any cursor state.
    const std::int64_t first = beatAtOrBefore(
        (offset_t) ( (std::int64_t) pos - (std::int64_t) clickFrames_ + 1 ) );
    const offset_t blockEnd = (offset_t) ( (std::int64_t) pos + (std::int64_t) frames );

    for( std::int64_t k = first;; ++k ) {
        const offset_t at = frameOfBeat( k );
        if( at >= blockEnd ) break;
        if( at >= cfg_.rangeEnd ) break;
        if( at < cfg_.rangeStart ) continue;
        const std::int64_t rel = (std::int64_t) at - (std::int64_t) pos;
        if( rel + (std::int64_t) clickFrames_ <= 0 ) continue;   // wholly behind

        const std::vector<float> &wave = isAccent( k ) ? accentWave_ : beatWave_;
        const std::size_t srcFrom = ( rel < 0 ) ? (std::size_t) ( -rel ) : 0;
        const std::size_t dstFrom = ( rel > 0 ) ? (std::size_t) rel : 0;
        if( srcFrom >= wave.size() || dstFrom >= frames ) continue;
        const std::size_t n = std::min( wave.size() - srcFrom, frames - dstFrom );

        for( std::size_t c = 0; c < channels; ++c ) {
            float *d = out[c];
            if( !d ) continue;
            for( std::size_t i = 0; i < n; ++i ) d[dstFrom + i] += wave[srcFrom + i];
        }
        if( rel >= 0 ) clicks_.fetch_add( 1, std::memory_order_relaxed );
    }
    return frames;
}
