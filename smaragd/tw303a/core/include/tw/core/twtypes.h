#ifndef _TWTYPES_H_
#define _TWTYPES_H_

// Core engine types (proposal 14, Phase 1 — extracted from twcomponent.h).
// This header is the bottom of the dependency graph: it may not include
// anything else from the project, and must stay Qt-free.

typedef signed long long length_t;
typedef signed short idx_t;
typedef float sample_t;
#define SAMPLE_NORM_MIN (-1.0)
#define SAMPLE_NORM_MAX (1.0)
// SIGNED (proposal 23): a position may legitimately be negative — a clip that
// begins before its data resolves to a negative source position, and the old
// unsigned type wrapped that to ~1.8e19 at the first cast, so the page came back
// silent. Positions still saturate at INT64_MAX (see SObject::satShift); the
// "unbounded extent" sentinel is INT64_MAX, never UINT64_MAX.
typedef signed long long offset_t;

// Align a position DOWN to a page/grain boundary. Must floor, not truncate:
// C++ integer division rounds toward zero, so the obvious (pos/grain)*grain puts
// a negative position in the page ABOVE the one that contains it — e.g. with a
// 65536 grain, -30464 aligns to 0 instead of -65536, and the page holding the
// silence-to-data seam is then never the page that gets rendered (proposal 23).
inline offset_t twFloorAlign( offset_t pos, offset_t grain )
{
    if( grain <= 0 ) return pos;
    offset_t q = pos / grain;
    if( pos % grain != 0 && pos < 0 ) --q;
    return q * grain;
}

// The type used for preview datas.
typedef signed char previewPart_t;
typedef struct {
    previewPart_t min, max;
} preview_t;

#define DTOR_DEL(x) {if((x)) {delete (x); (x) = NULL; }}

#endif
