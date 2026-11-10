// Gate for QBX-237: follow-the-playhead's ONE decision, sfollow::decide
// (app/timeline/sfollowplayhead.h). The view (SMVActualView::followLocator)
// only gathers the State and applies the answer; the .qxa
// follow_playhead_conditions drives that wiring through the real view.
//
// THE SHAPE OF EVERY SECTION: start from a state in which EVERY condition
// allows a scroll and the playhead is far outside the view (so the answer is
// "page"), then flip exactly ONE input. A section can therefore fail only
// for the condition it names -- the rule is a chain of early-outs, and a
// state that two conditions refuse at once would pass with either deleted.

#include "app/timeline/sfollowplayhead.h"

#include <cstdio>
#include <string>

namespace {

int g_failures = 0;

void check( bool ok, const std::string &what )
{
    std::printf( "%s %s\n", ok ? "ok  " : "FAIL", what.c_str() );
    if( !ok ) ++g_failures;
}

constexpr long long SPAN = 100000;   // frames from the left edge to the right
constexpr long long LEFT = 200000;

// Every condition allows; the playhead is a whole span past the right edge.
sfollow::State allowing()
{
    sfollow::State s;
    s.enabled = true;
    s.sounding = true;
    s.advancing = true;
    s.interacting = false;
    s.holdArmed = false;
    s.msSinceManualScroll = 0;
    s.leftOffset = LEFT;
    s.visibleSpan = SPAN;
    s.playhead = LEFT + 2 * SPAN;
    return s;
}

const long long PAGED_TO = LEFT + 2 * SPAN - SPAN / 10;   // LEAD_IN of the span

std::string why( const sfollow::Decision &d ) { return sfollow::whyName( d.why ); }

}  // namespace

int main()
{
    // --- the control: everything allows, so it pages ------------------------
    {
        const auto d = sfollow::decide( allowing() );
        check( d.scroll && d.newLeft == PAGED_TO && d.why == sfollow::Why::PagedIntoView,
               "control: off-screen playhead, every condition allowing -> pages to "
               "playhead - span/10 (got " + why( d ) + ", newLeft "
               + std::to_string( d.newLeft ) + ")" );
    }

    // --- condition 1: the option ---------------------------------------------
    {
        auto s = allowing(); s.enabled = false;
        const auto d = sfollow::decide( s );
        check( !d.scroll && d.why == sfollow::Why::Off,
               "off: SOpt::FollowPlayhead unchecked never scrolls (got " + why( d ) + ")" );
    }

    // --- condition 2: the root is not being heard -----------------------------
    {
        auto s = allowing(); s.sounding = false;
        const auto d = sfollow::decide( s );
        check( !d.scroll && d.why == sfollow::Why::Resting,
               "resting: a resting cursor is not chased (got " + why( d ) + ")" );
    }

    // --- condition 3: a gesture is under way ----------------------------------
    {
        auto s = allowing(); s.interacting = true;
        const auto d = sfollow::decide( s );
        check( !d.scroll && d.why == sfollow::Why::Interacting,
               "interacting: no scroll while a drag / range drag / drop is under way (got "
               + why( d ) + ")" );
    }

    // --- condition 4: the hold after a manual scroll --------------------------
    {
        auto s = allowing(); s.holdArmed = true; s.msSinceManualScroll = 0;
        auto d = sfollow::decide( s );
        check( !d.scroll && d.why == sfollow::Why::Held && !d.holdExpired,
               "held: just after a manual scroll, no scroll (got " + why( d ) + ")" );

        s.msSinceManualScroll = sfollow::HOLD_MS - 1;
        d = sfollow::decide( s );
        check( !d.scroll && d.why == sfollow::Why::Held && !d.holdExpired,
               "held: 1 ms before the hold ends, still no scroll (got " + why( d ) + ")" );

        s.msSinceManualScroll = sfollow::HOLD_MS;
        d = sfollow::decide( s );
        check( d.scroll && d.newLeft == PAGED_TO && d.holdExpired,
               "held: AT the hold's end it pages and reports the hold expired (got "
               + why( d ) + ")" );
    }
    check( sfollow::HOLD_MS == 5000, "the hold is 5 s (AC-g1, timeline CONTRACT inv. 26)" );

    // An expired hold is reported even when something else refuses, so the
    // view clears its flag and the hold cannot outlive a gesture for ever.
    {
        auto s = allowing(); s.interacting = true;
        s.holdArmed = true; s.msSinceManualScroll = sfollow::HOLD_MS + 1;
        const auto d = sfollow::decide( s );
        check( !d.scroll && d.holdExpired,
               "an expired hold is reported while a gesture still refuses" );
    }

    // --- where it pages: inside the view, the forward edge, either side ------
    {
        auto s = allowing(); s.playhead = LEFT + SPAN / 2;
        auto d = sfollow::decide( s );
        check( !d.scroll && d.why == sfollow::Why::Visible,
               "visible mid-span: stays (got " + why( d ) + ")" );

        s.playhead = LEFT + ( SPAN * 79 ) / 100;
        d = sfollow::decide( s );
        check( !d.scroll && d.why == sfollow::Why::Visible,
               "79 % across: stays (got " + why( d ) + ")" );

        s.playhead = LEFT + ( SPAN * 80 ) / 100;
        d = sfollow::decide( s );
        check( d.scroll && d.why == sfollow::Why::PagedForward
                   && d.newLeft == s.playhead - SPAN / 10,
               "80 % across while advancing: pages forward (got " + why( d ) + ")" );

        s.advancing = false;
        d = sfollow::decide( s );
        check( !d.scroll && d.why == sfollow::Why::Visible,
               "80 % across NOT advancing: stays, the edge is a forward trigger (got "
               + why( d ) + ")" );

        s = allowing(); s.playhead = LEFT + SPAN;   // exactly the exclusive right edge
        d = sfollow::decide( s );
        check( d.scroll && d.why == sfollow::Why::PagedIntoView,
               "at the right edge (exclusive): outside, pages into view (got " + why( d ) + ")" );

        s = allowing(); s.playhead = LEFT - 1;       // just LEFT of the view
        d = sfollow::decide( s );
        check( d.scroll && d.why == sfollow::Why::PagedIntoView
                   && d.newLeft == LEFT - 1 - SPAN / 10,
               "left of the view while advancing: pages back to it (got " + why( d ) + ")" );

        s.advancing = false;                          // a loop wrap
        d = sfollow::decide( s );
        check( d.scroll && d.why == sfollow::Why::PagedIntoView,
               "left of the view on a backward jump (loop wrap): pages to it (got "
               + why( d ) + ")" );

        s = allowing(); s.leftOffset = 500000; s.playhead = 3000;
        d = sfollow::decide( s );
        check( d.scroll && d.newLeft == 0,
               "a target before the timeline's start clamps to 0" );

        s = allowing(); s.leftOffset = 0; s.playhead = SPAN / 20;   // left-clamped and in view
        d = sfollow::decide( s );
        check( !d.scroll, "in view at the timeline's start: stays" );

        s = allowing(); s.visibleSpan = 0;
        d = sfollow::decide( s );
        check( !d.scroll && d.why == sfollow::Why::NoSpan,
               "an unsized view (span 0) never scrolls (got " + why( d ) + ")" );
    }

    std::printf( "%d failure(s)\n", g_failures );
    return g_failures == 0 ? 0 : 1;
}
