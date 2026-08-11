#include "tw/devices/audio_backend.h"
#include "tw/devices/capture_backend.h"
#include "tw/devices/null_backend.h"

#include "tw/core/twsyslog.h"

#include <cctype>
#include <cstdlib>
#include <string>

#if defined(QBX_LINUX_ALSA)
#  include "tw/devices/alsa_backend.h"
#endif
#if defined(QBX_WIN_WASAPI)
#  include "tw/devices/wasapi_backend.h"
#endif
#if defined(QBX_MAC_COREAUDIO)
#  include "tw/devices/coreaudio_backend.h"
#endif

namespace audio {

std::unique_ptr<AudioBackend> createAudioBackend()
{
    // SMARAGD_AUDIO_BACKEND outranks the platform. This is the whole of the
    // runtime selection: `capture` records to memory (headless playback tests),
    // `null` is the silent stub, `default` (or an unset variable) is the
    // platform backend below.
    //
    // The environment is the mechanism rather than a constructor argument
    // because the backend is minted inside twSpeaker's constructor, which is
    // reached from SApplication's constructor — there is no call site between
    // the two for a test harness to pass anything through. main.cpp sets the
    // variable for a --test-case run BEFORE SApplication exists, and only if it
    // is not already set, so an explicit setting always wins in both directions.
    if (const char *env = std::getenv("SMARAGD_AUDIO_BACKEND")) {
        std::string want;
        for (const char *p = env; *p; ++p)
            want.push_back((char) std::tolower((unsigned char) *p));

        if (want == "capture") {
            return std::unique_ptr<AudioBackend>(new CaptureBackend());
        }
        if (want == "null") {
            return std::unique_ptr<AudioBackend>(new NullBackend());
        }
        if (!want.empty() && want != "default") {
            syslog(LOG_WARNING,
                   "audio: unknown SMARAGD_AUDIO_BACKEND='%s' (expected capture|null|default);"
                   " using the platform backend",
                   env);
        }
    }

#if defined(QBX_WIN_WASAPI)
    return std::unique_ptr<AudioBackend>(new WASAPIBackend());
#elif defined(QBX_LINUX_ALSA)
    return std::unique_ptr<AudioBackend>(new ALSABackend());
#elif defined(QBX_MAC_COREAUDIO)
    return std::unique_ptr<AudioBackend>(new CoreAudioBackend());
#else
    return std::unique_ptr<AudioBackend>(new NullBackend());
#endif
}

}  // namespace audio
