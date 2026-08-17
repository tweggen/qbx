#include "tw/devices/audio_input.h"

#include "tw/core/twlog.h"
#include "tw/core/twsyslog.h"

#include "file_input.h"
#include "null_input.h"

#include <cctype>
#include <cstdlib>
#include <string>

#ifdef QBX_WIN_WASAPI
#include "wasapi_input.h"
#endif

#ifdef QBX_LINUX_ALSA
#include "alsa_input.h"
#endif

#ifdef QBX_MAC_COREAUDIO
#include "coreaudio_input.h"
#endif

namespace audio {

namespace {

std::string envString(const char *name)
{
    const char *v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

bool envFlag(const char *name, bool def)
{
    const std::string v = envString(name);
    if (v.empty()) return def;
    return !(v == "0" || v == "false" || v == "off" || v == "no");
}

std::unique_ptr<AudioInput> createPlatformInput()
{
#ifdef QBX_WIN_WASAPI
    return std::make_unique<WASAPIInput>();
#elif defined(QBX_LINUX_ALSA)
    return std::make_unique<ALSAInput>();
#elif defined(QBX_MAC_COREAUDIO)
    return std::make_unique<CoreAudioInput>();
#else
    return std::make_unique<NullInput>();
#endif
}

}  // namespace

std::unique_ptr<AudioInput> createAudioInput(const std::string &backend)
{
    std::string sel = backend;
    if (sel.empty()) sel = envString("SMARAGD_AUDIO_INPUT_BACKEND");

    // Lower-case the SELECTOR only; a file: path keeps its case (paths are
    // case-sensitive on two of the three platforms).
    std::string head = sel;
    const std::size_t colon = head.find(':');
    if (colon != std::string::npos) head = head.substr(0, colon);
    for (char &c : head) c = (char)std::tolower((unsigned char)c);

    if (head.empty() || head == "default") return createPlatformInput();

    if (head == "null") return std::make_unique<NullInput>();

    if (head == "file") {
        const std::string path =
            colon == std::string::npos ? std::string() : sel.substr(colon + 1);
        if (path.empty()) {
            syslog(LOG_WARNING,
                   "devices: SMARAGD_AUDIO_INPUT_BACKEND=file needs a path "
                   "(file:<wav>); falling back to the platform input");
            return createPlatformInput();
        }
        auto in = std::make_unique<FileAudioInput>(path);
        in->setLoop(envFlag("SMARAGD_AUDIO_INPUT_LOOP", true));
        const std::string lat = envString("SMARAGD_AUDIO_INPUT_LATENCY_FRAMES");
        if (!lat.empty())
            in->setReportedLatencyFrames((std::uint32_t)std::strtoul(lat.c_str(),
                                                                    nullptr, 10));
        return in;
    }

    syslog(LOG_WARNING,
           "devices: unknown audio input backend '%s'; using the platform input",
           sel.c_str());
    return createPlatformInput();
}

std::unique_ptr<AudioInput> createAudioInput()
{
    return createAudioInput(std::string());
}

}  // namespace audio
