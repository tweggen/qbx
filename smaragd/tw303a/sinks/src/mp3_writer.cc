#include "mp3_writer.h"

#include <cstdio>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <windows.h>
#define dlopen(name, flags) LoadLibraryA(name)
#define dlsym(handle, name) GetProcAddress(static_cast<HMODULE>(handle), name)
#define dlclose(handle) FreeLibrary(static_cast<HMODULE>(handle))
#else
#include <dlfcn.h>
#endif

namespace audio {

namespace {

// The libmp3lame runtime, by every name it actually ships under. ONE list, used
// by both loadLibrary() and isAvailable() -- they used to carry a copy each,
// and the copies had already drifted (the loader had two Unix search paths the
// probe lacked), so a library the encoder could open could still be reported
// unavailable by the UI.
//
// libmp3lame-0.dll is the name vcpkg and MSYS2 install, and its absence here
// was a real bug: smaragd's own build copies the whole vcpkg bin/ directory
// next to smaragd.exe (main/CMakeLists.txt), so the DLL was sitting beside the
// executable while every probe failed and the render dialog greyed MP3 out
// with "copy libmp3lame.dll to application directory" -- advice for a file the
// user already had, under a different name.
//
// The .0 / .so.0 forms matter for the same reason on the other platforms: a
// RUNTIME package ships the versioned file, and only the -dev package adds the
// unversioned symlink. A machine that can play MP3s need not be able to find
// `libmp3lame.so`.
const char *const kLameLibNames[] = {
#ifdef _WIN32
    "libmp3lame.dll",       // a hand-placed copy, and what older installs have
    "libmp3lame-0.dll",     // vcpkg, MSYS2 -- what the build actually deploys
    "mp3lame.dll",
    "lame.dll",
#elif defined(__APPLE__)
    "./libmp3lame.dylib",
    "libmp3lame.dylib",
    "libmp3lame.0.dylib",
    "/opt/homebrew/lib/libmp3lame.dylib",
    "/opt/homebrew/lib/libmp3lame.0.dylib",
    "/usr/local/lib/libmp3lame.dylib",
    "/usr/local/lib/libmp3lame.0.dylib",
#else  // Linux and the other ELF platforms
    "./libmp3lame.so",
    "libmp3lame.so",
    "libmp3lame.so.0",
    "/usr/lib/libmp3lame.so",
    "/usr/local/lib/libmp3lame.so",
#endif
};

// Every candidate, for an error message that says what was looked for rather
// than what the reader should go and copy.
std::string candidateList() {
    std::string out;
    for (const char *name : kLameLibNames) {
        if (!out.empty()) {
            out += ", ";
        }
        out += name;
    }
    return out;
}

}  // namespace

MP3Writer::MP3Writer() {}

MP3Writer::~MP3Writer() {
    close();
}

bool MP3Writer::loadLibrary() {
    if (lameHandle) {
        return true;
    }

    for (const char *name : kLameLibNames) {
        lameHandle = dlopen(name, RTLD_LAZY);
        if (lameHandle) {
            break;
        }
    }

    if (!lameHandle) {
        lastError = "libmp3lame not found. Tried: " + candidateList();
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

// The public API declared in tw/sinks/audio_file_writer.h. Thin on purpose: the
// app asks these, never MP3Writer directly.
bool mp3WriterAvailable() {
    return MP3Writer::isAvailable();
}

std::string mp3WriterCandidates() {
    return candidateList();
}

bool MP3Writer::isAvailable() {
    // Try to load the library without keeping it loaded. Same kLameLibNames as
    // loadLibrary() -- deliberately, so the UI can never grey MP3 out over a
    // library the encoder would have opened.
    for (const char *name : kLameLibNames) {
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
