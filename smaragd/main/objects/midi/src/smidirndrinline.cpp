#include "app/objects/midi/smidirndrinline.h"

#include <QPainter>
#include <algorithm>
#include <cstdint>
#include <vector>

#include "app/model/slink.h"
#include "app/objects/midi/smidicut.h"
#include "app/objects/midi/smidisequence.h"
#include "tw/events/twevent.h"
#include "tw/events/tweventseq.h"

namespace {

// A NOTE IS DRAWN IN ITS TRACK'S COLOUR (app/model/sclipcolors.h), handed down
// on the render context by whoever filled the clip body -- exactly as a
// waveform is. These were fixed green and blue, so every event clip in every
// project looked the same and an event clip on a track looked unrelated to the
// audio clips beside it. kMetaColor stays a constant: a meta marker is not
// content, it is an annotation.
const QColor kMetaColor( 200, 190, 110 );

/**
 * Paint one frame-domain sequence into `visib`, mapping FRAME positions
 * through the context's own x<->time map so the drawing lines up with the
 * lane's zoom exactly as a waveform does.
 *
 * `firstFrame`/`lastFrame` bound what the window admits; events outside are
 * skipped rather than clamped, which is what makes a split clip's head and
 * tail draw the two halves of a straddling note correctly.
 */
/**
 * The PRESENT pitch range of `[firstFrame, lastFrame)`, so a clip of a few
 * notes uses the full lane height.
 *
 * Resolved ONCE per draw and handed to every paintSeq call, never recomputed
 * per call. A looping clip's LAST repetition is usually a partial segment, so
 * a range derived per repetition would give it a different vertical scale and
 * the tail of the loop would draw its notes at other heights than the
 * repetitions before it.
 */
void keyRangeOf( const twEventSeq &seq, offset_t firstFrame, offset_t lastFrame,
                 int &lowKey, int &highKey )
{
    lowKey = 127; highKey = 0;
    bool anyNote = false;
    for( const twEvent &e : seq.events() ) {
        if( e.kind != twEventKind::NoteOn || e.key < 0 ) continue;
        if( e.time + e.duration <= firstFrame || e.time >= lastFrame ) continue;
        lowKey = std::min( lowKey, (int) e.key );
        highKey = std::max( highKey, (int) e.key );
        anyNote = true;
    }
    if( !anyNote ) { lowKey = 48; highKey = 72; }
    if( highKey - lowKey < 11 ) {            // never thinner than an octave
        const int mid = ( highKey + lowKey ) / 2;
        lowKey = mid - 6; highKey = mid + 6;
    }
}

void paintSeq( QPainter &p, SRenderContext &ctx, const twEventSeq &seq,
               offset_t originFrame, offset_t firstFrame, offset_t lastFrame,
               int lowKey, int highKey )
{
    const QRect visib = ctx.getVisibRect();
    if( visib.width() <= 0 || visib.height() <= 0 ) return;

    const offset_t tLeft  = ctx.getTimeOf( visib.left() );
    const offset_t tRight = ctx.getTimeOf( visib.right() + 1 );
    const double span = (double) ( tRight - tLeft );
    if( !( span > 0.0 ) ) return;
    const double pxPerFrame = visib.width() / span;

    const int keySpan = highKey - lowKey;
    const int top = visib.top() + 2, bottom = visib.bottom() - 1;
    const int usable = std::max( 4, bottom - top );
    const int noteH = std::max( 1, usable / std::max( 1, keySpan ) );

    auto xOf = [&]( offset_t frame ) {
        return visib.left()
             + (int) ( ( (double) ( frame + originFrame - tLeft ) ) * pxPerFrame );
    };

    p.save();
    p.setClipRect( visib );
    for( const twEvent &e : seq.events() ) {
        if( twEventIsMetadata( e.kind ) ) {
            if( e.time < firstFrame || e.time >= lastFrame ) continue;
            p.fillRect( QRect( xOf( e.time ), visib.top(), 1, 3 ), kMetaColor );
            continue;
        }
        if( e.kind == twEventKind::NoteOn ) {
            if( e.key < 0 ) continue;
            const offset_t a = std::max<offset_t>( e.time, firstFrame );
            const offset_t b = std::min<offset_t>(
                e.time + std::max<int64_t>( e.duration, 1 ), lastFrame );
            if( b <= a ) continue;
            const int x0 = xOf( a );
            const int w = std::max( 1, xOf( b ) - x0 );
            const int y = bottom - noteH
                        - (int) ( ( (double) ( e.key - lowKey ) / keySpan )
                                  * ( usable - noteH ) );
            QColor c = ctx.clipColors().wave;
            // Velocity as opacity: a soft note reads as a soft note.
            const int alpha = 90 + (int) ( 165.0 * std::min( 1.0, e.value / 127.0 ) );
            c.setAlpha( std::max( 60, std::min( 255, alpha ) ) );
            p.fillRect( QRect( x0, y, w, noteH ), c );
            continue;
        }
        // Everything else (CC, bend, pressure): a faint tick low in the body.
        if( e.time < firstFrame || e.time >= lastFrame ) continue;
        // A CC tick is content too, but subordinate to the notes: the same
        // colour, taken down. (The old fixed blue is kept only as the colour a
        // context with no palette set would produce something sensible from.)
        p.fillRect( QRect( xOf( e.time ), bottom - 2, 1, 2 ),
                    ctx.clipColors().wave.darker( 150 ) );
    }
    p.restore();
}

}  // namespace

// ---------------------------------------------------------------------------

SMidiSequenceRendererInline::SMidiSequenceRendererInline( SMidiSequence &s )
    : SObjectRenderer( (SObject &) s )
{
}

SMidiSequence &SMidiSequenceRendererInline::sequence() const
{
    return (SMidiSequence &) getObject();
}

void SMidiSequenceRendererInline::draw( SLink &lk, SRenderContext &ctx )
{
    SMidiSequence &seq = sequence();
    std::shared_ptr<const twEventSeq> table = seq.tickSnapshot();
    if( !table ) return;
    // A bare sequence placed directly (no window) draws its whole table. The
    // table is in TICKS, so it has no frame mapping of its own; the only
    // honest thing is to draw nothing rather than to invent a tempo here.
    // Every real placement goes through SMidiCut.
    (void) lk; (void) ctx;
}

// ---------------------------------------------------------------------------

SMidiCutRendererInline::SMidiCutRendererInline( SMidiCut &c )
    : SObjectRenderer( (SObject &) c )
{
}

SMidiCut &SMidiCutRendererInline::cut() const
{
    return (SMidiCut &) getObject();
}

void SMidiCutRendererInline::draw( SLink &lk, SRenderContext &ctx )
{
    SMidiCut &c = cut();
    const SMidiCutSnapshot snap = c.getSnapshot();
    if( !snap.framesSeq ) return;

    QPainter &p = ctx.getPainter();
    const offset_t clipStart = lk.getStartTime();
    const offset_t first = snap.startOffsetFrames;
    const length_t dur   = snap.durationFrames;
    const length_t loop  = snap.loopFrames;

    // A LOOPING window draws its segment once per repetition, under exactly
    // the law resolveEventClip() gives the audio path: twEventLoopMap maps a
    // clip position p to sequence position (startOffset + p mod loopFrames),
    // so repetition k occupies [clipStart + k*loop, ... ) and reads the SAME
    // segment [startOffset, startOffset + loop) of the sequence.
    //
    // Drawing the table once over the whole window - which is what this did -
    // is only correct when there is no loop: the notes exist solely in the
    // first segment, so every repetition after the first came out BLANK while
    // sounding perfectly. The loop condition is the same one resolveEventClip
    // tests, so the picture cannot disagree with the ear about whether this
    // clip loops at all.
    const bool looping = ( loop > 0 && loop < dur );

    // The pitch range is taken over the SEGMENT THAT IS ACTUALLY REPEATED and
    // shared by every repetition (see keyRangeOf).
    int lowKey = 0, highKey = 0;
    keyRangeOf( *snap.framesSeq, first,
                first + ( looping ? loop : dur ), lowKey, highKey );

    // The sequence's zero is the CONTENT's zero, so the origin that maps a
    // sequence position onto the timeline is (clip start - slip).
    if( !looping ) {
        paintSeq( p, ctx, *snap.framesSeq, clipStart - first,
                  first, first + dur, lowKey, highKey );
    } else {
        // Only the repetitions the visible rect can reach. A long clip over a
        // short loop is thousands of them, and each one is a full pass over
        // the event table -- a scroll must not pay for the ones off screen.
        const QRect visib = ctx.getVisibRect();
        const int64_t tLeft  = (int64_t) ctx.getTimeOf( visib.left() );
        const int64_t tRight = (int64_t) ctx.getTimeOf( visib.right() + 1 );
        auto floorDiv = []( int64_t a, int64_t b ) {
            const int64_t q = a / b;
            return ( a % b != 0 && ( ( a < 0 ) != ( b < 0 ) ) ) ? q - 1 : q;
        };
        int64_t kFrom = floorDiv( tLeft - (int64_t) clipStart, (int64_t) loop );
        int64_t kTo   = floorDiv( tRight - (int64_t) clipStart, (int64_t) loop );
        const int64_t kLast = ( (int64_t) dur - 1 ) / (int64_t) loop;
        if( kFrom < 0 ) kFrom = 0;
        if( kTo > kLast ) kTo = kLast;
        for( int64_t k = kFrom; k <= kTo; ++k ) {
            // The last repetition is normally PARTIAL: the window ends where
            // it ends, mid-segment, and a note crossing that edge is clipped
            // by paintSeq exactly as one crossing a split is.
            const length_t segLen =
                std::min<length_t>( loop, dur - (length_t) ( k * (int64_t) loop ) );
            if( segLen <= 0 ) break;
            paintSeq( p, ctx, *snap.framesSeq,
                      clipStart + (offset_t) ( k * (int64_t) loop ) - first,
                      first, first + segLen, lowKey, highKey );
        }
    }

    if( !c.getSName().isEmpty() ) {
        p.save();
        // The clip name, in the same content colour as the notes under it.
        p.setPen( ctx.clipColors().wave );
        p.drawText( ctx.getVisibRect().adjusted( 3, 1, -3, -1 ),
                    Qt::AlignLeft | Qt::AlignTop, c.getSName() );
        p.restore();
    }
}
