#ifndef APP_TIMELINE_SFOLLOWPLAYHEAD_H
#define APP_TIMELINE_SFOLLOWPLAYHEAD_H

// FOLLOW THE PLAYHEAD (QBX-237): the one decision, with nothing to show.
//
// The arranger keeps the playhead on screen while the transport advances. The
// rule has four conditions that each override the next, and every one of them
// used to live inline in SMVActualView::followLocator, readable only through a
// mapped window and a wall clock. Here it is a pure function over a State, so
// each condition can be unit-tested inside the others' allowed region
// (timeline/tests/followplayhead_test.cpp), and the view only gathers the
// state and applies the answer.
//
//   1. off       SOpt::FollowPlayhead is unchecked            -> never scroll
//   2. resting   this view's root is not being heard          -> never scroll
//                (proposal 09 §15: the cursor RESTS, and a view that scrolled
//                while nothing moved would be chasing a still line)
//   3. gesture   a pointer gesture or a drag-and-drop is under way on the
//                canvas (a clip drag, a resize, a range drag, an automation
//                marquee...)                                  -> never scroll:
//                the content would move under the pointer mid-edit
//   4. held      the user scrolled by hand less than HOLD_MS ago (AC-g1,
//                timeline CONTRACT inv. 26: 5 s)              -> never scroll
//
// Past those, PAGE-FLIP: the view jumps so the cursor lands LEAD_IN of the
// span from the left edge when the cursor
//   - is anywhere outside the visible span (either side: a loop wrap, a
//     playback started off screen, a hold that expired while the user looked
//     elsewhere), or
//   - is advancing and has crossed FORWARD_EDGE of the span.
// Otherwise the view stays. Page-flip rather than a continuous scroll is the
// behaviour the arranger already had (bef8b104); QBX-237 kept it.
//
// All positions are frames in THIS VIEW's own timeline - in an arrangement
// tab, the DERIVED playhead, never the master frame the transport reports.

namespace sfollow {

inline constexpr long long HOLD_MS      = 5000;   // AC-g1
inline constexpr double    FORWARD_EDGE = 0.80;   // trigger, fraction of the span
inline constexpr double    LEAD_IN      = 0.10;   // where the cursor lands

struct State {
    bool      enabled     = true;    // SOpt::FollowPlayhead
    bool      sounding    = true;    // the view's root is heard right now
    bool      advancing   = true;    // the transport moved FORWARD this tick
    bool      interacting = false;   // a gesture / drag-and-drop is under way
    bool      holdArmed   = false;   // a manual scroll has armed the hold...
    long long msSinceManualScroll = 0;   // ...this long ago
    long long playhead    = 0;       // the DRAWN cursor, this view's frames
    long long leftOffset  = 0;       // the view's left edge, frames
    long long visibleSpan = 0;       // frames from the left edge to the right
};

enum class Why {
    Off, Resting, Interacting, Held, NoSpan,   // the early-outs, in order
    Visible,                                   // in view, not at the edge
    PagedForward,                              // crossed FORWARD_EDGE
    PagedIntoView,                             // was outside the span
};

struct Decision {
    bool      scroll      = false;
    long long newLeft     = 0;       // valid when scroll
    bool      holdExpired = false;   // the view may clear its hold flag
    Why       why         = Why::Off;
};

inline Decision decide( const State &s )
{
    Decision d;
    d.newLeft = s.leftOffset;
    // Reported whatever else decides, so a hold that ran out during a gesture
    // is not left armed for ever.
    d.holdExpired = s.holdArmed && s.msSinceManualScroll >= HOLD_MS;

    if( !s.enabled )     { d.why = Why::Off;         return d; }
    if( !s.sounding )    { d.why = Why::Resting;     return d; }
    if( s.interacting )  { d.why = Why::Interacting; return d; }
    if( s.holdArmed && !d.holdExpired ) { d.why = Why::Held; return d; }
    if( s.visibleSpan <= 0 ) { d.why = Why::NoSpan;  return d; }

    const long long left  = s.leftOffset;
    const long long right = s.leftOffset + s.visibleSpan;   // exclusive
    Why why;
    if( s.playhead < left || s.playhead >= right )
        why = Why::PagedIntoView;
    else if( s.advancing
             && (double)( s.playhead - left ) >= FORWARD_EDGE * (double) s.visibleSpan )
        why = Why::PagedForward;
    else { d.why = Why::Visible; return d; }

    long long target = s.playhead - (long long)( LEAD_IN * (double) s.visibleSpan );
    if( target < 0 ) target = 0;
    d.why = why;
    d.newLeft = target;
    d.scroll = ( target != s.leftOffset );
    return d;
}

inline const char *whyName( Why w )
{
    switch( w ) {
    case Why::Off:           return "off";
    case Why::Resting:       return "resting";
    case Why::Interacting:   return "interacting";
    case Why::Held:          return "held";
    case Why::NoSpan:        return "no-span";
    case Why::Visible:       return "visible";
    case Why::PagedForward:  return "paged-forward";
    case Why::PagedIntoView: return "paged-into-view";
    }
    return "?";
}

}  // namespace sfollow

#endif  // APP_TIMELINE_SFOLLOWPLAYHEAD_H
