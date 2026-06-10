#include "audio/audio_input.h"

#include "null_input.h"

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

std::unique_ptr<AudioInput> createAudioInput() {
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

}  // namespace audio
