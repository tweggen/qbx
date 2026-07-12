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
typedef unsigned long long offset_t;

// The type used for preview datas.
typedef signed char previewPart_t;
typedef struct {
    previewPart_t min, max;
} preview_t;

#define DTOR_DEL(x) {if((x)) {delete (x); (x) = NULL; }}

#endif
