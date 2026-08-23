#include "app/model/sclipcolors.h"
#include "app/model/sobject.h"
#include "app/model/slink.h"

#include <algorithm>

namespace {

// One anchor: a name and its HSL triple. HSL rather than a hex literal because
// every derived variant below is an HSL move (lighter / desaturated), and doing
// those moves on a stored hex means converting back and forth at every call.
struct Anchor { const char *name; double h; double s; double l; };

// The 16, ORDERED SO NEIGHBOURS DIFFER. The auto assignment is by lane order,
// so index 0 and index 1 are two lanes stacked on top of each other: a hue ramp
// (0, 22.5, 45, ...) would make every adjacent pair nearly identical. These hop
// the wheel instead -- blue, orange, green, purple, ... -- so any short run of
// consecutive tracks is legible, and the whole set is still evenly covered.
const Anchor kAnchors[] = {
    { "Denim", 212, 0.38, 0.44 },
    { "Clay",   22, 0.36, 0.45 },
    { "Sage",  152, 0.28, 0.42 },
    { "Plum",  291, 0.28, 0.45 },
    { "Ochre",  44, 0.40, 0.42 },
    { "Teal",  186, 0.36, 0.38 },
    { "Rose",  340, 0.32, 0.48 },
    { "Moss",  100, 0.30, 0.36 },
    { "Slate", 206, 0.20, 0.44 },
    { "Rust",   12, 0.42, 0.42 },
    { "Fern",  128, 0.26, 0.40 },
    { "Mauve", 315, 0.24, 0.46 },
    { "Amber",  34, 0.44, 0.44 },
    { "Ocean", 196, 0.40, 0.36 },
    { "Brick", 358, 0.36, 0.42 },
    { "Olive",  68, 0.30, 0.38 },
};
const int kCount = (int) ( sizeof( kAnchors ) / sizeof( kAnchors[0] ) );

inline double clamp01( double v ) { return std::max( 0.0, std::min( 1.0, v ) ); }

QColor fromHsl( double hDeg, double s, double l )
{
    // QColor's hue is a fraction of the circle and must stay inside [0,1) --
    // 360 degrees wraps to 1.0, which fromHslF rejects.
    double h = hDeg / 360.0;
    h -= std::floor( h );
    return QColor::fromHslF( (float) h, (float) clamp01( s ), (float) clamp01( l ) );
}

const Anchor &anchorAt( int index )
{
    // A negative index is "auto that was never resolved" -- take anchor 0
    // rather than reading off the front of the array.
    if( index < 0 ) return kAnchors[0];
    return kAnchors[ index % kCount ];
}

}  // namespace

namespace sclipcolors {

int count() { return kCount; }

QString name( int index ) { return QString::fromLatin1( anchorAt( index ).name ); }

QColor base( int index )
{
    const Anchor &a = anchorAt( index );
    return fromHsl( a.h, a.s, a.l );
}

QColor body( int index, bool selected, bool muted )
{
    const Anchor &a = anchorAt( index );
    // Selected FIRST: a selected muted clip is drawn selected. See the header.
    if( selected ) return fromHsl( a.h, a.s + 0.06, a.l + 0.13 );
    // Muted keeps a TRACE of the hue rather than going fully grey, so a muted
    // track is still identifiable as itself at a glance.
    if( muted )    return fromHsl( a.h, a.s * 0.22, a.l - 0.07 );
    return fromHsl( a.h, a.s, a.l );
}

QColor wave( int index, bool selected, bool muted )
{
    const Anchor &a = anchorAt( index );
    if( selected ) return fromHsl( a.h, 0.06, 0.95 );
    if( muted )    return fromHsl( a.h, 0.04, 0.68 );
    return fromHsl( a.h, 0.10, 0.80 );
}

namespace {

// Depth-first over LANES ONLY. Recursing into everything would walk every clip
// in the project (and, through an asset, into another track's contents) for a
// number that only counts lanes.
bool walkLanes( SObject &node, const SObject &target, int &counter, int &found )
{
    for( SLink *lk : node.childLinks() ) {
        if( !lk ) continue;
        SObject &child = lk->getSObject();
        if( !child.isLane() ) continue;
        if( &child == &target ) { found = counter; return true; }
        ++counter;
        if( walkLanes( child, target, counter, found ) ) return true;
    }
    return false;
}

}  // namespace

int autoIndexForLane( SObject &root, const SObject &lane )
{
    int counter = 0, found = -1;
    walkLanes( root, lane, counter, found );
    return found < 0 ? 0 : found;
}

int indexForLane( SObject &root, const SObject &lane )
{
    const int chosen = lane.colorIndex();
    if( chosen >= 0 ) return chosen;
    return autoIndexForLane( root, lane );
}

}  // namespace sclipcolors
