#include "audio/audio_file_writer.h"

#include "wav_writer.h"

namespace audio {

std::unique_ptr<AudioFileWriter> createAudioFileWriter(AudioFormat format) {
    switch (format) {
        case AudioFormat::WAV:
            return std::make_unique<WAVWriter>();
        case AudioFormat::OGG:
        case AudioFormat::MP3:
            // Placeholder for OGG and MP3 writers (implemented in Phase 2b/2c)
            return nullptr;
    }
    return nullptr;
}

}  // namespace audio
