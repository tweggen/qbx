// loopback_calibration — measure the audio round trip by sending a known probe
// out and finding it again in what came back. Proposal 21 L6a.
//
// WHY THIS EXISTS. `audio/recordingOffsetMs/<device>` is the last term of the
// placement conversion (L3b, design D6), and today the app asks the USER to
// produce it by hand: Options → Audio says, in as many words, "record a click,
// look at where it landed, type the difference". That is a measurement a
// machine should make.
//
// The whole of the measurement lives here as a PURE FUNCTION of two buffers,
// deliberately, because that is what makes it gateable at all: the physical
// half needs a cable between an output and an input and is Windows-manual by
// nature, whereas "did we find a probe that we know is at frame N, at frame
// N + d" is arithmetic and can be gated with synthetic captures on any host.
//
// THE PROBE IS AN IMPULSE, NOT A CHIRP OR A TONE. A tone has no unique point
// to align to — every period looks like every other, so a correlation peak is
// ambiguous by exactly one period, and at 440 Hz that is 109 frames of
// plausible-looking error. A single wide-band click has one unambiguous
// arrival, which is the only property this measurement needs. (A chirp buys
// noise immunity that a line-level loopback does not need, at the cost of a
// correlation nobody can check by eye.)

#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace audio {

struct LoopbackResult {
    // Frames between the probe LEAVING (its position in the emitted buffer)
    // and ARRIVING (its position in the captured buffer). This is the round
    // trip: output latency + the cable + input latency.
    std::int64_t roundTripFrames = 0;

    // Peak amplitude of what was found, 0..1. A loopback at line level lands
    // around the probe's own amplitude; a tenth of it means something is
    // attenuating badly and the number is worth doubting.
    float peakAmplitude = 0.0f;

    // How far the peak stands above the surrounding noise floor, as a ratio.
    // This is what makes the answer REFUSABLE: a room mic that happened to
    // hear the click still produces a peak, but not a clean one.
    float peakToNoise = 0.0f;

    // False when nothing convincing was found. The caller must not fall back
    // to a guess — a calibration that returns a plausible wrong number is
    // worse than one that says it failed, because the wrong number is silently
    // applied to every take the user records afterwards.
    bool  found = false;
};

// One click, `frames` long, with an instantaneous attack — the attack is the
// thing being timed, so it must be the first sample. The decay is only there
// to keep it from sounding like a fault; the measurement ignores it.
inline std::vector<float> loopbackProbe(std::size_t frames = 64, float amplitude = 0.5f)
{
    std::vector<float> p(frames, 0.0f);
    if (frames == 0) return p;
    for (std::size_t i = 0; i < frames; ++i) {
        // Alternating sign: broadband, so it survives any sensible filtering
        // in the path, and it cannot be confused with DC or with hum.
        const float env = 1.0f - (float) i / (float) frames;
        p[i] = ((i % 2) ? -amplitude : amplitude) * env * env;
    }
    return p;
}

// Find the probe's ARRIVAL in `captured` (interleaved, `channels` wide; only
// `channel` is examined), given that it was EMITTED at `emittedAtFrame`.
//
// TWO refusal thresholds, and BOTH are needed. `minPeakToNoise` is relative —
// does the candidate stand clear of the floor — and `minPeakAmplitude` is
// ABSOLUTE: is it plausibly the probe at all.
//
// THE ABSOLUTE ONE WAS ADDED AFTER HARDWARE PROVED THE RELATIVE ONE
// INSUFFICIENT, and the failure is worth stating because it is the exact thing
// this function exists to prevent. Run against a real interface with NO CABLE
// CONNECTED, the relative test alone reported a confident 345.62 ms: the
// unconnected input's own noise had a peak of 0.0045 — 0.45 % of full scale,
// 40 dB below the emitted probe — standing 17.9x above an even quieter RMS
// floor. Synthetic Gaussian noise never produced that, because Gaussian noise
// has no spikes; real inputs do (interference, a relay, a cable being
// touched), and a spike over a quiet floor passes any purely relative test.
//
// So the caller passes what it EMITTED, and a candidate must be within a
// sensible loss of it. A line-level loopback returns near unity; a pad or a
// mismatched level might cost 20-30 dB. Two orders of magnitude (40 dB) is
// already generous, and it is three orders clear of the 0.0045 that fooled the
// relative test.
inline LoopbackResult loopbackMeasure(const float *captured, std::size_t frames,
                                      std::uint32_t channels, std::uint32_t channel,
                                      std::int64_t emittedAtFrame,
                                      float minPeakToNoise = 8.0f,
                                      float minPeakAmplitude = 0.0f)
{
    LoopbackResult r;
    if (!captured || frames == 0 || channels == 0 || channel >= channels) return r;

    // Pass 1: the largest |sample|, and where.
    std::size_t peakAt = 0;
    float       peak   = 0.0f;
    for (std::size_t i = 0; i < frames; ++i) {
        const float a = std::fabs(captured[i * channels + channel]);
        if (a > peak) { peak = a; peakAt = i; }
    }
    if (peak <= 0.0f) return r;

    // Pass 2: the noise floor, measured AWAY from the peak. "Away" is a
    // generous margin either side, because the probe's own decay is not noise
    // and counting it as such would flatter every measurement.
    const std::size_t guard = 2048;
    double sum = 0.0;
    std::size_t n = 0;
    for (std::size_t i = 0; i < frames; ++i) {
        if (i + guard >= peakAt && i <= peakAt + guard) continue;
        const float a = std::fabs(captured[i * channels + channel]);
        sum += (double) a * a;
        ++n;
    }
    const float noise = n ? (float) std::sqrt(sum / (double) n) : 0.0f;

    r.peakAmplitude = peak;
    // A perfectly silent floor is not an error — it is what a digital loopback
    // gives. Report it as a large ratio rather than dividing by zero.
    r.peakToNoise   = (noise > 1e-9f) ? (peak / noise) : 1e9f;

    // THE ARRIVAL IS THE ONSET, NOT THE PEAK — and this distinction was paid
    // for by a failing test rather than foreseen. The probe's first two
    // samples differ by only ~3 % in magnitude (0.0800 and 0.0775 for a
    // 64-frame probe), so a noise floor a tenth of that size is enough to make
    // sample 1 the larger of the two, and timing the MAXIMUM then reports the
    // arrival ONE FRAME LATE. Measured: delay 4801 against a true 4800, with a
    // noise floor 8x below the probe — a perfectly ordinary line-level
    // loopback.
    //
    // So the peak is used for AMPLITUDE and SNR, and the onset is found by
    // walking BACK from it to the first sample that clears a fraction of it.
    // That is stable under noise, and it is also what survives any filtering
    // in the path: a smeared attack still crosses the threshold at the same
    // place, whereas its maximum can move by several samples.
    const float onsetFloor = peak * 0.25f;
    std::size_t onset = peakAt;
    // The walk is bounded: it may not run past the probe's own length, or a
    // noisy stretch before the arrival could drag the onset arbitrarily early.
    const std::size_t maxWalk = 256;
    for (std::size_t back = 0; back < maxWalk && onset > 0; ++back) {
        const float prev = std::fabs(captured[(onset - 1) * channels + channel]);
        if (prev < onsetFloor) break;
        --onset;
    }
    peakAt = onset;
    r.roundTripFrames = (std::int64_t) peakAt - emittedAtFrame;

    // Refuse rather than guess. Three ways to fail, and each one has been seen:
    //   - too quiet in ABSOLUTE terms: not the probe, whatever its SNR (the
    //     no-cable case above);
    //   - not clear of the floor: something is there but it is not clean;
    //   - an arrival BEFORE the emit: impossible, and what a stale capture
    //     buffer or a mis-stamped emit position looks like. It must never
    //     become a negative "latency" that is then subtracted from every
    //     placement.
    r.found = r.peakAmplitude >= minPeakAmplitude &&
              r.peakToNoise   >= minPeakToNoise &&
              r.roundTripFrames >= 0;
    return r;
}

// The round trip in milliseconds, for a human. Kept beside the measurement so
// the two cannot disagree about the rate.
inline double loopbackMs(std::int64_t frames, std::uint32_t sampleRate)
{
    return sampleRate ? 1000.0 * (double) frames / (double) sampleRate : 0.0;
}

// What the user should be offered for `audio/recordingOffsetMs`, given a
// measured round trip and what the DRIVER already claimed.
//
// The app already compensates the driver's REPORTED latencies automatically
// (L3b), so the offset must carry only what the driver got WRONG — the
// residual. Handing over the whole round trip would double-count everything
// the driver reported correctly, which on a well-behaved interface is all of
// it.
//
// Sign follows the app-wide convention: POSITIVE = EARLIER (37 P7's
// `midi/offsetMs`, L3b's `recordingOffsetMs`). A round trip LONGER than
// reported means audio arrives later than the app thinks, so it must be placed
// earlier, so the offset is positive.
inline double loopbackSuggestedOffsetMs(std::int64_t measuredRoundTripFrames,
                                        std::uint32_t reportedOutputFrames,
                                        std::uint32_t reportedInputFrames,
                                        std::uint32_t sampleRate)
{
    const std::int64_t reported =
        (std::int64_t) reportedOutputFrames + (std::int64_t) reportedInputFrames;
    return loopbackMs(measuredRoundTripFrames - reported, sampleRate);
}

}  // namespace audio
