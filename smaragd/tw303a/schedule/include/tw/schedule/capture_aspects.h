#ifndef _CAPTURE_ASPECTS_H_
#define _CAPTURE_ASPECTS_H_

#include <cstdint>

/**
 * Capture aspect bitmask: granular invalidation + lazy recomputation.
 *
 * Different capture data has different lifecycles:
 * - Preview: Waveform for timeline display (batched, low priority)
 * - Playback: Full-quality audio for real-time playback (eager, high priority)
 * - Export: Resampled/normalized for export (on-demand only)
 * - Metadata: Duration, peak levels, RMS (lightweight, computed with playback)
 *
 * Invalidation is aspect-specific: muting a track invalidates Playback+Metadata
 * but NOT Preview (waveform shape unchanged). Revalidation is lazy: aspects
 * recompute on demand via the CaptureRevalidator.
 *
 * Lives in the engine (moved from scut.h) because the revalidator dispatches
 * on these bits; the app's document objects use the same values.
 * NOTE: distinct from twRenderAspect (tw_output_page.h), which has a
 * different bit assignment for frozen component pages — do not mix them.
 */
enum twCaptureAspect : uint32_t {
    Preview   = 1u << 0,  // Waveform peaks for timeline
    Playback  = 1u << 1,  // Reader chain for audio output
    Metadata  = 1u << 2,  // Duration, peak levels (computed with playback)
    Export    = 1u << 3,  // Resampled buffer for export (on-demand)

    All       = Preview | Playback | Metadata | Export,
};

// Historical name, kept so existing app code keeps compiling.
using SCutCaptureAspect = twCaptureAspect;

#endif
