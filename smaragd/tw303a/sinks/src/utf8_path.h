#ifndef _TW_SINKS_UTF8_PATH_H_
#define _TW_SINKS_UTF8_PATH_H_

// Output paths reach the writers as UTF-8: the app hands over
// QString::toStdString(), and tw_sinks has no Qt to convert with. On Windows a
// narrow path given to std::fopen -- or to this build's libsndfile sf_open,
// measured (QBX-145) -- is read in the ANSI code page, so a name with any
// character outside it is created as mojibake or not at all. The writers
// therefore widen the path themselves on Windows and open it with the wide
// API. Elsewhere the narrow path is already UTF-8 and is used as it is.

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX   // keep min/max macros out of the includers
#endif
#include <windows.h>

#include <string>

namespace audio {

// UTF-8 to UTF-16. An invalid sequence yields an empty string, which no open
// accepts -- the caller reports the failure rather than writing somewhere else.
inline std::wstring utf8ToWide(const std::string &utf8) {
    if (utf8.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                      (int)utf8.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring wide((std::size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), (int)utf8.size(),
                        &wide[0], n);
    return wide;
}

}  // namespace audio

#endif  // _WIN32

#endif
