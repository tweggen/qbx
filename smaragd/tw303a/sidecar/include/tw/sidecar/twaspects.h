
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
 * the rate the preview was computed at.
 *
 * Header channels = the SOURCE's channel count, and it is an adoption
 * cross-check: the payload is ONE envelope folded over ALL of them (the
 * smallest min and the largest max across channels), so the same material
 * read at a different width is a different preview and must not be adopted.
 */
constexpr const char *PreviewPeaks        = "preview.peaks";
constexpr uint32_t    PreviewPeaksVersion = 2;
// v2 (proposal 36 B8): the probe envelope folds EVERY channel, not channel 0
// alone, and header channels carries the real source width instead of a
// hard-coded 1. A v1 file was written under the channel-0 rule and its bytes
// are only accidentally right (they agree whenever channel 0 dominates, which
// every fixture in this repo happens to satisfy) — so it orphans on sight
// rather than being adopted. The record layout is unchanged from v1.

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
constexpr uint32_t    OnsetsVersion = 4;
// v4 (proposal 28 W2 follow-up): positions are ATTACK-CENTERED — k*hop +
// (fftSize - hop/2), clamped to the source end. v3 reported the flux frame
// START, which leads the perceptual attack by ~fftSize - hop/2 (-896 frames
// at the 1024/256 defaults, a visible ~19 ms tick-vs-waveform bias). Record
// layout is unchanged from v3: PACKED 12-byte { uint64 pos, float32
// salience } LE, stride 12 — salience is the normalized flux at detection.
// Two consumer tiers: vocoder keyframes take every record; the marker UI
// filters by salience. v2 (normalized flux + energy gate; fixed v1's ~29
// spurious fires/second on steady loud material that M4's keyframes
// amplified into level collapse) kept u64-only records. Older versions
// orphan on sight and regenerate.

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

/**
 * "f0" — per-hop fundamental estimate (proposal 28 W3), the prerequisite
 * metadata for key detection / pitch-correct consumers (none wired yet; the
 * aspect ships ahead of them on the M0 substrate).
 *
 * Computed over the SOURCE-rate decoded PCM, all channels folded to a mono
 * mean before analysis. Estimator: YIN — difference function over a fixed
 * integration window, cumulative-mean normalization, absolute-threshold
 * first-dip pick with local-minimum descent, parabolic refinement, RMS
 * energy gate — see twanalyzers.h for the normative algorithm.
 *
 * Record: float32 LE = f0 in Hz for the window starting at record index ×
 * hopFrames; 0 = UNVOICED. recordStride = 4, recordCount =
 * ceil(sourceFrames / hopFrames), hopFrames = the analysis hop. Header
 * geometry = the analyzed source's native geometry.
 *
 * Params blob v1 (LE, in order): uint32 rate, uint32 hopFrames,
 * uint32 winFrames, float32 fminHz, float32 fmaxHz, float32 threshold.
 */
constexpr const char *F0        = "f0";
constexpr uint32_t    F0Version = 2;
// v2 (W5): all reductions run in FOUR fixed lanes combined (l0+l2)+(l1+l3)
// — the normative accumulation order (platform-identical, vectorizable).
// Values differ from v1 only in last-ulp rounding; v1 orphans on sight.

/**
 * "warp.pcm" — the finished time-stretch/pitch-shift output (proposal 27 M2):
 * a durable copy of twGrainSource's materialized warp, so a project load at
 * SAVED settings is a file read instead of a full Rubber Band analysis+
 * resynthesis (the load-stall fix). Strictly a cache of bytes the engine
 * would compute identically — the M2 gate is byte-identical renders hot vs
 * cold vs store-off. Superseded as the primary path by M5's paged
 * resynthesis, after which it remains a valid zero-DSP top layer.
 *
 * Payload: the warped signal, planar Float32, channel-major
 * (channel c, frame f at payload[c*outFrames + f]). recordStride = 4
 * (one float), recordCount = channels × outFrames, hopFrames = 0.
 * Header sourceRate = the WARP's rate (the project-rate view the grain
 * stage consumed); sourceFrames = OUTPUT frames — exactly the proposal-18
 * render-boundary length floor(inLen × stretch), used as the adoption
 * cross-check.
 *
 * Params blob v2 (LE, in order): uint8 backend (1 = Rubber Band R3 offline,
 * 2 = legacy overlap-add, 3 = in-house vocoder — their bytes differ, so the
 * backend is key material; a Rubber Band library upgrade that changes
 * output bytes must bump WarpPcmVersion), uint32 rate, uint32 channels,
 * int64 stretch numerator, int64 stretch denominator (the exact Fraction,
 * clamped as the ctor clamps it), float64 pitchCents (IEEE bits),
 * uint32 grainSize, uint32 crossfade (vestigial on the RB path but kept in
 * the key — worst case a duplicate cache entry, never a wrong hit),
 * uint64 onsetsHash (M4: fingerprint of the onset keyframe positions the
 * vocoder baked in; 0 = none. Warps built before/after the onsets sidecar
 * exists occupy different keys — availability never aliases bytes),
 * uint64 anchorsHash (v3: W1 warp-marker fingerprint; 0 = none),
 * uint8 preserveFormants (v4: the W4 opt-in flag),
 * float64 formantShiftCents (v6: the independent formant-shift offset;
 * 0 = no-op).
 */
constexpr const char *WarpPcm        = "warp.pcm";
constexpr uint32_t    WarpPcmVersion = 6;   // v6: + float64 formantShiftCents
// (independent formant shift — changes output bytes whenever it is
// non-zero, whether or not the pitch stage runs). v5 was the W5 pitch-stage
// rewrite (integer-offset sinc tables + four-lane tap reduction) — same
// params, different bytes, so v4 caches must orphan. v4: + uint8
// preserveFormants (W4 opt-in — changes output bytes whenever the pitch
// stage runs). v3 added uint64 anchorsHash (W1 warp-marker fingerprint;
// 0 = no anchors); v2 added onsetsHash.

} // namespace twAspect

#endif
