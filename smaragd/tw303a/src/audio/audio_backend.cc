#include "audio/audio_backend.h"
#include "audio/null_backend.h"

#if defined(QBX_LINUX_ALSA)
#  include "audio/alsa_backend.h"
#endif
#if defined(QBX_WIN_WASAPI)
#  include "audio/wasapi_backend.h"
#endif
#if defined(QBX_MAC_COREAUDIO)
#  include "audio/coreaudio_backend.h"
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
