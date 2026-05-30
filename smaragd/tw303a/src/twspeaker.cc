#include "twspeaker.h"

#include "audio/audio_backend.h"
#include "sapplication.h"
#include "twsyslog.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>

twSpeaker::twSpeaker(tw303aEnvironment &env0)
    : twComponent(env0),
      backend_(audio::createAudioBackend()),
      isPlaying_(false)
{
    syslog(LOG_INFO, "twSpeaker: using audio backend '%s'", backend_->name());
}

twSpeaker::~twSpeaker()
{
    if (isPlaying_) backend_->stopOutput();
    backend_->closeDevice();
}

void twSpeaker::startOutput()
{
    if (isPlaying_) return;

    if (backend_->openDevice("default") != 0) {
        syslog(LOG_ERR, "twSpeaker::startOutput: openDevice failed");
        return;
    }

    backend_->setRenderCallback(
        [this](float *out, std::size_t frames, std::uint32_t channels) -> std::size_t {
            // Throwaway diagnostics: count callback invocations and the
            // peak amplitude of what we pulled from the synth, logged
            // roughly every second.
            static std::atomic<std::uint64_t> s_calls{0};
            static std::atomic<std::uint64_t> s_framesReqTotal{0};
            static std::atomic<std::uint64_t> s_framesGotTotal{0};
            static std::atomic<int>           s_peakSinceLogX1000{0};
            static std::atomic<std::uint64_t> s_logEveryCalls{50};

            std::uint64_t callIdx = s_calls.fetch_add(1) + 1;
            s_framesReqTotal.fetch_add(frames);

            if (pInputPlugs == nullptr || pInputPlugs[0] == nullptr) {
                std::fill_n(out, frames * channels, 0.0f);
                if (callIdx == 1) {
                    syslog(LOG_WARNING,
                           "twSpeaker render: pInputPlugs[0] is null — nothing is wired into the speaker");
                }
                return frames;
            }

            offset_t formerPos = SApplication::app().getGlobalLocatorPos();
            length_t framesRead =
                static_cast<twLatchStreamingOutput *>(pInputPlugs[0])
                    ->readStreamingData(out, static_cast<length_t>(frames));

            if (framesRead <= 0) {
                std::fill_n(out, frames * channels, 0.0f);
                if (callIdx == 1 || callIdx % s_logEveryCalls.load() == 0) {
                    syslog(LOG_WARNING,
                           "twSpeaker render: pInputPlugs[0]->readStreamingData returned %ld for %zu frames",
                           (long)framesRead, frames);
                }
                return frames;
            }

            // Peak of the mono buffer we just got.
            float peak = 0.0f;
            for (length_t i = 0; i < framesRead; ++i) {
                float a = std::fabs(out[i]);
                if (a > peak) peak = a;
            }
            int peakX1000 = static_cast<int>(peak * 1000.0f);
            int prevPeak = s_peakSinceLogX1000.load();
            while (peakX1000 > prevPeak
                   && !s_peakSinceLogX1000.compare_exchange_weak(prevPeak, peakX1000)) {}

            s_framesGotTotal.fetch_add(static_cast<std::uint64_t>(framesRead));

            // Mono synthesizer → fan out to N channels. We read mono into the
            // start of the buffer and expand in place from the tail.
            if (channels > 1) {
                for (length_t i = framesRead - 1; i >= 0; --i) {
                    float s = out[i];
                    for (std::uint32_t c = 0; c < channels; ++c) {
                        out[i * channels + c] = s;
                    }
                }
            }

            SApplication::app().setGlobalLocatorPos(formerPos + framesRead);

            if (callIdx == 1 || callIdx % s_logEveryCalls.load() == 0) {
                int p = s_peakSinceLogX1000.exchange(0);
                syslog(LOG_INFO,
                       "twSpeaker render: call #%llu, req=%llu got=%llu (last batch %ld/%zu), peak=%.3f",
                       (unsigned long long)callIdx,
                       (unsigned long long)s_framesReqTotal.load(),
                       (unsigned long long)s_framesGotTotal.load(),
                       (long)framesRead, frames,
                       p / 1000.0f);
            }

            return static_cast<std::size_t>(framesRead);
        });

    if (backend_->startOutput() != 0) {
        syslog(LOG_ERR, "twSpeaker::startOutput: backend startOutput failed");
        backend_->closeDevice();
        return;
    }
    isPlaying_ = true;
}

void twSpeaker::stopOutput()
{
    if (!isPlaying_) return;
    backend_->stopOutput();
    backend_->closeDevice();
    isPlaying_ = false;
}

bool twSpeaker::isPlaying()
{
    return isPlaying_;
}

length_t twSpeaker::calcOutputTo(sample_t *, length_t, idx_t)
{
    return 0;
}

void twSpeaker::createOutputLatches()
{
#ifdef DEBUG_COMPONENT
    syslog(LOG_DEBUG, "twSpeaker::createOutputLatches(): entered.");
#endif
}
