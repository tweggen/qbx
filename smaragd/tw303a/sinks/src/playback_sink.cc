#include "tw/sinks/playback_sink.h"

namespace audio {

PlaybackSink::PlaybackSink(AudioBackend* backend)
    : backend_(backend)
{
}

bool PlaybackSink::writeFrame(const AudioFrame& frame) {
    // For real-time playback, frames are consumed by the audio callback
    // as it pulls from AudioEngine. This method is a no-op in Phase 5b
    // since twspeaker.cc directly interleaves frames into the output buffer.
    //
    // Phase 5c (render) will implement buffered file writing.
    // Phase 5b just ensures twspeaker uses AudioEngine internally.
    return true;
}

}  // namespace audio
