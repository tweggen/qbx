#ifndef _TWCONVERT_H_
#define _TWCONVERT_H_

#include "twcomponent.h"   // length_t
#include "twformat.h"

// Pure format conversion between two interleaved buffers: sample binary type
// (Float32/Float64/Int16/Int32), and channel count (mono <-> N: mono fans out,
// N down-mixes to mono by averaging, equal counts map 1:1). It performs NO
// sample-rate change — that is the resampler's job; src.sampleRate and
// dst.sampleRate are ignored here.
//
// Returns the number of frames converted (== frames), or 0 if the request is
// unsupported (planar layout, which no wire uses yet). When the two formats
// share a memory shape it is a single memcpy, so Float32->Float32 stays cheap.
length_t twConvertFrames( const twFormat &src, const void *srcBuf,
                          const twFormat &dst, void       *dstBuf,
                          length_t frames );

#endif
