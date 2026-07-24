
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

/**
 * "onsets" — transient/onset positions (proposal 27 M1), the prerequisite
 * metadata for any future warp-marker / transient-preservation feature.
 *
 * Computed over the SOURCE-rate decoded PCM, all channels folded to a mono
 * mean before analysis. Detector: spectral flux (Hann STFT, positive
 * per-bin magnitude differences summed), median-adaptive threshold, local-
 * maximum peak picking with a minimum separation — see twanalyzers.h for the
 * normative algorithm parameters.
 *
 * Record: uint64 LE = onset position in SOURCE frames, ascending.
 * recordStride = 8, recordCount = number of onsets (may be 0 — an empty
 * payload is a valid result, distinct from "not analyzed"), hopFrames = the
 * analysis hop (context, not a record period — onset spacing is irregular).
 * Consumer caveat: material that ends at a non-zero level carries a real
 * spectral edge at its end (Hann truncation against the implicit zero pad),
 * so a final onset within ~fftSize of sourceFrames may be that boundary,
 * not a musical transient.
 * Header sourceRate/channels/sourceFrames = the analyzed source's native
 * geometry.
 *
 * Params blob v1 (LE, in order): uint32 fftSize, uint32 hop,
 * float32 thresholdFactor, float32 thresholdFloor, uint32 medianHalfWidth,
 * uint32 minSeparationFrames. Changing any default mints a new key.
 */
constexpr const char *Onsets        = "onsets";
constexpr uint32_t    OnsetsVersion = 1;

/**
 * "loudness" — RMS envelope (proposal 27 M1), for normalization/auto-gain
 * consumers and UI meters.
 *
 * Computed over the SOURCE-rate decoded PCM, all channels folded by power
 * mean. Record: float32 LE = RMS of the window starting at record index ×
 * hopFrames (window length = winFrames, zero-padded at the tail).
 * recordStride = 4, hopFrames = the hop, recordCount = ceil(sourceFrames /
 * hopFrames). Header geometry = the analyzed source's native geometry.
 *
 * Params blob v1 (LE, in order): uint32 hopFrames, uint32 winFrames.
 */
constexpr const char *Loudness        = "loudness";
constexpr uint32_t    LoudnessVersion = 1;

} // namespace twAspect

#endif
