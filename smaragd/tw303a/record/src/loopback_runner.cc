// loopback_runner — see loopback_runner.h. Proposal 21 L6a.

#include "tw/record/loopback_runner.h"

#include "tw/core/twlog.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

namespace audio {

LoopbackRun runLoopback(AudioBackend *backend, AudioInput *input,
                        const LoopbackRunParams &params)
{
    LoopbackRun run;
    if (!backend || !input) {
        run.error = "no output or input device";
        return run;
    }

    // THE REFUSAL THAT DEFINES L6a's BOUNDARY. Two devices are two clocks: the
    // streams start at unrelated moments, so the difference of their frame
    // counters carries the start skew as well as the latency and is not a
    // latency at all. Refusing is the honest answer; measuring across two
    // clocks is L6c and needs drift handling rather than a bigger buffer.
    if (params.outputDeviceId != params.inputDeviceId) {
        run.error = "the loopback calibration needs ONE device for both directions: "
                    "across two devices the measurement carries the start skew "
                    "between two clocks, not the round-trip latency";
        TW_LOGW("record", "loopback: refused '%s' out / '%s' in — %s",
                params.outputDeviceId.c_str(), params.inputDeviceId.c_str(),
                run.error.c_str());
        return run;
    }

    const AudioConfig      outCfg = backend->getConfig();
    const AudioInputConfig inCfg  = input->getConfig();
    run.sampleRate           = outCfg.sampleRate;
    run.reportedOutputFrames = outCfg.outputLatencyFrames;
    run.reportedInputFrames  = inCfg.inputLatencyFrames;

    if (run.sampleRate == 0) {
        run.error = "the output device is not open";
        return run;
    }
    const std::uint32_t inCh = inCfg.channels;
    if (inCh == 0 || params.inputChannel >= inCh) {
        run.error = "the input has no channel " + std::to_string(params.inputChannel);
        return run;
    }

    const std::vector<float> probe = loopbackProbe(64, params.probeAmplitude);

    // The render callback's whole job: silence, then the probe exactly once at
    // a known output-frame position. Nothing here allocates or locks — it runs
    // on the driver's thread under the usual RT rules.
    std::atomic<std::uint64_t> outFrames{0};
    std::atomic<bool>          emitted{false};
    const std::uint64_t emitAt = params.emitAtFrame;

    backend->setRenderCallback(
        [&probe, &outFrames, &emitted, emitAt](float *out, std::size_t frames,
                                               std::uint32_t ch) -> std::size_t {
            const std::uint64_t base = outFrames.load(std::memory_order_relaxed);
            std::fill_n(out, frames * ch, 0.0f);

            // The probe may straddle a block boundary; write whatever part of
            // it falls inside this one. It is emitted on EVERY output channel,
            // because which socket the cable is in is the user's business.
            for (std::size_t i = 0; i < probe.size(); ++i) {
                const std::uint64_t abs = emitAt + i;
                if (abs < base || abs >= base + frames) continue;
                const std::size_t f = (std::size_t) (abs - base);
                for (std::uint32_t c = 0; c < ch; ++c) out[f * ch + c] = probe[i];
            }
            if (base + frames > emitAt + probe.size())
                emitted.store(true, std::memory_order_relaxed);

            outFrames.store(base + frames, std::memory_order_relaxed);
            return frames;
        });

    // Capture into one contiguous buffer: `runFrames` at the INPUT's width, so
    // the measurement can index it the way it expects.
    std::vector<float> captured((std::size_t) params.runFrames * inCh, 0.0f);
    std::uint64_t got = 0;

    // START ORDER IS INPUT FIRST. The capture must already be running when the
    // probe leaves, or the arrival is simply not in the buffer — and on a
    // shared duplex device starting the input is what starts the driver, so
    // the output's first callback cannot precede it.
    if (input->startCapture() != 0) {
        run.error = "startCapture failed";
        return run;
    }
    if (backend->startOutput() != 0) {
        input->stopCapture();
        run.error = "startOutput failed";
        return run;
    }

    // Drain until the buffer is full or the run has plainly stalled. The
    // deadline is generous: the gate hardware takes ~475 ms just to deliver
    // its first callback (proposal 35), so a tight timeout would abort a
    // healthy device before it ever spoke.
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(
                              (params.runFrames * 1000ull) / run.sampleRate + 5000);
    while (got < params.runFrames && std::chrono::steady_clock::now() < deadline) {
        const std::size_t want = (std::size_t) (params.runFrames - got);
        const std::int32_t n = input->read(captured.data() + got * inCh, want);
        if (n > 0) got += (std::uint64_t) n;
        else       std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    backend->stopOutput();
    input->stopCapture();

    run.framesCaptured = got;
    run.ran            = true;

    if (!emitted.load(std::memory_order_relaxed)) {
        run.error = "the probe was never emitted — the output stream did not run";
        return run;
    }
    if (got < params.emitAtFrame + probe.size()) {
        run.error = "the capture is shorter than the point the probe was emitted at";
        return run;
    }

    run.result = loopbackMeasure(captured.data(), (std::size_t) got, inCh,
                                 params.inputChannel, (std::int64_t) params.emitAtFrame,
                                 params.minPeakToNoise,
                                 params.probeAmplitude * params.minReturnFraction);

    if (run.result.found) {
        TW_LOGI("record",
                "loopback: round trip %lld frames (%.2f ms) at %u Hz; driver reported "
                "%u out + %u in; residual %.2f ms; peak %.3f, peak/noise %.1f",
                (long long) run.result.roundTripFrames,
                loopbackMs(run.result.roundTripFrames, run.sampleRate), run.sampleRate,
                run.reportedOutputFrames, run.reportedInputFrames,
                run.suggestedOffsetMs(), run.result.peakAmplitude,
                run.result.peakToNoise);
    } else {
        run.error = "no probe found in the capture — check the cable, the input "
                    "channel, and that the output is not muted";
        TW_LOGW("record",
                "loopback: %s (peak %.4f against a %.4f floor for a %.2f probe, "
                "peak/noise %.1f)",
                run.error.c_str(), run.result.peakAmplitude,
                params.probeAmplitude * params.minReturnFraction, params.probeAmplitude,
                run.result.peakToNoise);
    }
    return run;
}

}  // namespace audio
