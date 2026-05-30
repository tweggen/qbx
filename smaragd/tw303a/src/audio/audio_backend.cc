#include "audio/audio_backend.h"
#include "audio/null_backend.h"

#if defined(QBX_LINUX_ALSA)
#  include "audio/alsa_backend.h"
#endif

namespace audio {

std::unique_ptr<AudioBackend> createAudioBackend()
{
#if defined(QBX_LINUX_ALSA)
    return std::unique_ptr<AudioBackend>(new ALSABackend());
#else
    return std::unique_ptr<AudioBackend>(new NullBackend());
#endif
}

}  // namespace audio
