#include "tw/devices/audio_backend.h"
#include "tw/devices/null_backend.h"

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
