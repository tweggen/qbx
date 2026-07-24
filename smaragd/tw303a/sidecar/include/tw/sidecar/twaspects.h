
#ifndef _TWASPECTS_H_
#define _TWASPECTS_H_

#include <cstdint>

/**
 * The aspect registry (proposal 27) — the closed list of known sidecar
 * payload formats. M0 keeps this deliberately thin: an aspect is an id
 * string (<= 16 ASCII chars, part of the file name and header), a payload
 * version, and a documented record layout. The M1 background-job work adds
 * generator registration on top; nothing at this layer changes for that.
 *
 * Adding an aspect = adding constants + a payload doc comment here. Changing
 * a payload's meaning = bumping its version, which orphans old files
 * (twSidecarStore deletes them on sight; they regenerate).
 */
namespace twAspect {

/**
 * "preview.peaks" — the waveform-preview peak array (the former ad-hoc
 * SObject::previewData_, proposal 27 M0 migration).
 *
 * Record: 2 bytes { int8 min, int8 max }, one per probe — byte-identical to
 * the in-memory preview_t array. recordStride = 2, recordCount = the probe
 * count (incl. the trailing partial probe), hopFrames = previewSkip (frames
 * per probe at the RATE THE PREVIEW WAS COMPUTED AT).
 *
 * Params blob: { uint32 projectRate } (LE). The preview is computed over the
 * project-rate view of the source, so its bytes depend on the project rate;
 * distinct rates are distinct sidecars. Content hash stays the source-rate
 * PCM digest (the same material shares one <hh>/ neighborhood across rates).
 *
 * sourceFrames in the header = the duration the preview was computed for
 * (previewForLength_, project-rate frames), used as the adoption
 * cross-check: a stored preview is only adopted when it matches the
 * object's current duration exactly. Header sourceRate likewise carries the
 * PROJECT rate for this aspect — every geometry field here is expressed at
 * the rate the preview was computed at. Header channels = 1 (the straight
 * preview folds channel 0 only).
 */
constexpr const char *PreviewPeaks        = "preview.peaks";
constexpr uint32_t    PreviewPeaksVersion = 1;

} // namespace twAspect

#endif
