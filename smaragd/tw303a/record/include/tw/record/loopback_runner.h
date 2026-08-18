// loopback_runner — drive a real loopback pass: emit the probe on an output,
// capture it on an input, and measure the round trip. Proposal 21 L6a.
//
// The MEASUREMENT is `loopback_calibration.h` and is pure. THIS is the part
// that needs two real streams and a cable, and it exists so the app has one
// object to drive rather than a recipe to re-implement.
//
// WHERE THE TWO STREAMS GET A SHARED TIME REFERENCE — the whole problem
//
// "Round trip" is (arrival in the capture) − (departure from the output), and
// those two positions are counted by different streams. What makes them
// comparable is the FRAME COUNTERS on each side, both advanced by whole
// blocks, and the assumption that the two sides are driven by ONE clock.
//
// **On a full-duplex ASIO device that assumption is exactly true**, and it is
// the reason this is worth building now: one `bufferSwitch` hands the driver's
// input and output buffers for the SAME instant (proposal 35 Phase 3), so
// output frame N and input frame N are simultaneous by construction and the
// difference of the counters IS the round trip.
//
// **On two different devices it is NOT true**, and the error is not small: the
// two streams start at unrelated moments, so the difference carries the
// START SKEW as well as the latency, and the skew is whatever the OS happened
// to do that day. The runner therefore REFUSES a cross-device pass rather than
// returning a number that looks like a latency (`sameDeviceRequired`). That
// refusal is also the honest boundary of L6a: measuring across two clocks is
// L6c, and it needs drift handling, not a bigger buffer.
//
// WHAT IT DOES NOT DO: apply anything. It returns a measurement. Writing
// `audio/recordingOffsetMs` is the caller's decision and, per the L6a brief,
// the USER's — a calibration that silently rewrites a timing constant is one
// bad measurement away from moving every take recorded afterwards.

#pragma once

#include "tw/devices/audio_backend.h"
#include "tw/devices/audio_input.h"
#include "tw/record/loopback_calibration.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace audio {

struct LoopbackRunParams {
    // Both must name the SAME device. See the header comment: across two
    // devices the answer carries the start skew and is not a latency.
    std::string outputDeviceId = "default";
    std::string inputDeviceId  = "default";

    // Which input carries the returning signal, as a channel INDEX (not a
    // mask): the cable goes into exactly one socket.
    std::uint32_t inputChannel = 0;
    // Which input channels the device must open, as a mask (proposal 35
    // Phase 3 semantics: bit n == input n). Defaults to the one being read.
    std::uint64_t inputChannelMask = 0;

    // How long to run, and how far in to emit. The lead-in exists so the
    // capture has a noise floor to be measured against, and so the probe is
    // not sitting inside the driver's start-up ramp (~475 ms on the gate
    // hardware — see proposal 35).
    std::uint32_t runFrames      = 96000;   // 2 s at 48 k
    std::uint32_t emitAtFrame    = 24000;   // 0.5 s in

    float probeAmplitude = 0.5f;
    // Below this peak-to-noise the answer is REFUSED rather than reported.
    float minPeakToNoise = 8.0f;
    // ...and below this FRACTION OF WHAT WAS EMITTED, likewise. A relative
    // test alone is not enough: measured on real hardware with NO CABLE, the
    // input's own noise peak (0.45 % of full scale) stood 17.9x above its RMS
    // floor and produced a confident, entirely fictional 345.62 ms. 40 dB of
    // loss is already generous for a line-level loopback.
    float minReturnFraction = 0.01f;
};

struct LoopbackRun {
    LoopbackResult result;

    // Context the caller needs to explain the number to a human.
    std::uint32_t sampleRate           = 0;
    std::uint32_t reportedOutputFrames = 0;   // what the driver claimed
    std::uint32_t reportedInputFrames  = 0;
    std::uint64_t framesCaptured       = 0;
    bool          ran                  = false;
    std::string   error;

    // The residual the user should be OFFERED, in ms, sign per the app-wide
    // convention (POSITIVE = EARLIER). Zero when the driver reported honestly.
    double suggestedOffsetMs() const
    {
        return result.found
                   ? loopbackSuggestedOffsetMs(result.roundTripFrames,
                                               reportedOutputFrames,
                                               reportedInputFrames, sampleRate)
                   : 0.0;
    }
};

// Runs one pass. BLOCKS for `runFrames` worth of wall clock plus the device
// start-up, so it belongs on a worker or behind a progress dialog — never on
// an audio thread, and never on the UI thread without one.
//
// `backend` and `input` must already be OPEN. The runner starts and stops
// them; it does not own them, because the app's are already open and
// re-opening a device to measure it would measure a different device state
// than the one being calibrated.
LoopbackRun runLoopback(AudioBackend *backend, AudioInput *input,
                        const LoopbackRunParams &params);

}  // namespace audio
