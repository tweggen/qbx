#include "mp3_writer.h"

#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#define dlopen(name, flags) LoadLibraryA(name)
#define dlsym(handle, name) GetProcAddress(static_cast<HMODULE>(handle), name)
#define dlclose(handle) FreeLibrary(static_cast<HMODULE>(handle))
#else
#include <dlfcn.h>
#endif

namespace audio {

MP3Writer::MP3Writer() {}

MP3Writer::~MP3Writer() {
    close();
}

bool MP3Writer::loadLibrary() {
    if (lameHandle) {
        return true;
    }

    const char *libNames[] = {
#ifdef _WIN32
        "libmp3lame.dll",
        "mp3lame.dll",
        "lame.dll",
#elif defined(__APPLE__)
        "./libmp3lame.dylib",
        "libmp3lame.dylib",
        "/opt/homebrew/lib/libmp3lame.dylib",
        "/usr/local/lib/libmp3lame.dylib",
#else  // Linux
        "./libmp3lame.so",
        "libmp3lame.so",
        "libmp3lame.so.0",
        "/usr/lib/libmp3lame.so",
        "/usr/local/lib/libmp3lame.so",
#endif
    };

    for (const char *name : libNames) {
        lameHandle = dlopen(name, RTLD_LAZY);
        if (lameHandle) {
            break;
        }
    }

    if (!lameHandle) {
        lastError = "libmp3lame not found. Copy the library to the application directory.";
        return false;
    }

    // Resolve function pointers
    fn_lame_init = reinterpret_cast<decltype(fn_lame_init)>(dlsym(lameHandle, "lame_init"));
    fn_lame_set_in_samplerate =
        reinterpret_cast<decltype(fn_lame_set_in_samplerate)>(dlsym(lameHandle, "lame_set_in_samplerate"));
    fn_lame_set_num_channels =
        reinterpret_cast<decltype(fn_lame_set_num_channels)>(dlsym(lameHandle, "lame_set_num_channels"));
    fn_lame_set_out_samplerate =
        reinterpret_cast<decltype(fn_lame_set_out_samplerate)>(dlsym(lameHandle, "lame_set_out_samplerate"));
    fn_lame_set_brate =
        reinterpret_cast<decltype(fn_lame_set_brate)>(dlsym(lameHandle, "lame_set_brate"));
    fn_lame_set_quality =
        reinterpret_cast<decltype(fn_lame_set_quality)>(dlsym(lameHandle, "lame_set_quality"));
    fn_lame_init_params =
        reinterpret_cast<decltype(fn_lame_init_params)>(dlsym(lameHandle, "lame_init_params"));
    fn_lame_encode_buffer_ieee_float = reinterpret_cast<decltype(fn_lame_encode_buffer_ieee_float)>(
        dlsym(lameHandle, "lame_encode_buffer_ieee_float"));
    fn_lame_encode_flush =
        reinterpret_cast<decltype(fn_lame_encode_flush)>(dlsym(lameHandle, "lame_encode_flush"));
    fn_lame_close = reinterpret_cast<decltype(fn_lame_close)>(dlsym(lameHandle, "lame_close"));

    if (!fn_lame_init || !fn_lame_set_in_samplerate || !fn_lame_set_num_channels ||
        !fn_lame_set_out_samplerate || !fn_lame_set_brate || !fn_lame_set_quality ||
        !fn_lame_init_params || !fn_lame_encode_buffer_ieee_float || !fn_lame_encode_flush ||
        !fn_lame_close) {
        dlclose(lameHandle);
        lameHandle = nullptr;
        lastError = "Failed to resolve libmp3lame functions";
        return false;
    }

    return true;
}

bool MP3Writer::open(const std::string &path, const AudioFileConfig &config) {
    if (gfp) {
        lastError = "Encoder already open";
        return false;
    }

    if (!loadLibrary()) {
        return false;
    }

    gfp = fn_lame_init();
    if (!gfp) {
        lastError = "Failed to initialize LAME encoder";
        return false;
    }

    channels = config.channels;

    if (!initEncoder(config)) {
        fn_lame_close(gfp);
        gfp = nullptr;
        return false;
    }

    // TODO: File writing implementation would go here
    // For now, just initialize the encoder
    lastError = "MP3 writing not yet fully implemented";
    return false;
}

bool MP3Writer::initEncoder(const AudioFileConfig &config) {
    fn_lame_set_in_samplerate(gfp, static_cast<int>(config.sampleRate));
    fn_lame_set_num_channels(gfp, static_cast<int>(config.channels));
    fn_lame_set_out_samplerate(gfp, static_cast<int>(config.sampleRate));
    fn_lame_set_brate(gfp, bitrate);
    fn_lame_set_quality(gfp, 2);  // 0 = high quality, 9 = low quality

    if (fn_lame_init_params(gfp) < 0) {
        lastError = "Failed to initialize LAME encoder parameters";
        return false;
    }

    return true;
}

bool MP3Writer::write(const float *interleaved, std::size_t frameCount) {
    if (!gfp) {
        lastError = "Encoder not open";
        return false;
    }

    // TODO: Implement MP3 frame writing
    lastError = "MP3 writing not yet fully implemented";
    return false;
}

bool MP3Writer::close() {
    if (gfp) {
        fn_lame_close(gfp);
        gfp = nullptr;
    }

    if (lameHandle) {
        dlclose(lameHandle);
        lameHandle = nullptr;
    }

    return true;
}

const char *MP3Writer::errorMessage() const {
    return lastError.c_str();
}

bool MP3Writer::isAvailable() {
    // Try to load the library without keeping it loaded
    const char *libNames[] = {
#ifdef _WIN32
        "libmp3lame.dll",
        "mp3lame.dll",
        "lame.dll",
#elif defined(__APPLE__)
        "./libmp3lame.dylib",
        "libmp3lame.dylib",
        "/opt/homebrew/lib/libmp3lame.dylib",
#else  // Linux
        "./libmp3lame.so",
        "libmp3lame.so",
        "libmp3lame.so.0",
#endif
    };

    for (const char *name : libNames) {
        void *handle = dlopen(name, RTLD_LAZY);
        if (handle) {
            dlclose(handle);
            return true;
        }
    }

    return false;
}

void MP3Writer::setBitrate(int kbps) {
    bitrate = kbps < 128 ? 128 : (kbps > 320 ? 320 : kbps);
}

}  // namespace audio
