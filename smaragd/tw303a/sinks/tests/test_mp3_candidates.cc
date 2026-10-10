// test_mp3_candidates.cc — the libmp3lame candidate list names every form the
// library actually ships under.
//
// This test exists because of a bug it would have caught. MP3 export loads
// libmp3lame at RUNTIME, and the candidate list carried only the unversioned
// names: libmp3lame.dll, mp3lame.dll, lame.dll. vcpkg and MSYS2 install it as
// libmp3lame-0.dll, and smaragd's own build copies the whole vcpkg bin/
// directory next to smaragd.exe (main/CMakeLists.txt) -- so on every Windows
// build the library sat beside the executable while every probe failed, and the
// render dialog greyed MP3 out while advising the user to copy a file they
// already had.
//
// It is a STRING test on purpose. It needs no libmp3lame present, so it runs on
// any box and in CI, where the encoder may well be absent. What regressed was
// the LIST, and the list is what this pins.
//
// It is also tw_sinks's first test.

#include "tw/sinks/audio_file_writer.h"

#include <cstdio>
#include <string>

static int failures = 0;

static void expect_contains( const std::string &haystack, const char *needle )
{
    const bool ok = haystack.find( needle ) != std::string::npos;
    std::printf( "  %-26s %s\n", needle, ok ? "listed" : "MISSING" );
    if( !ok ) {
        ++failures;
    }
}

int main()
{
    const std::string cands = audio::mp3WriterCandidates();
    std::printf( "\n-- libmp3lame candidates on this platform --\n  %s\n\n", cands.c_str() );

    if( cands.empty() ) {
        std::printf( "  the candidate list is EMPTY -- nothing could ever load\n" );
        return 1;
    }

#if defined( _WIN32 )
    // The unversioned name AND the one vcpkg / MSYS2 actually install.
    expect_contains( cands, "libmp3lame.dll" );
    expect_contains( cands, "libmp3lame-0.dll" );
#elif defined( __APPLE__ )
    expect_contains( cands, "libmp3lame.dylib" );
    // A Homebrew RUNTIME install ships the versioned file; only the -dev side
    // adds the unversioned symlink.
    expect_contains( cands, "libmp3lame.0.dylib" );
#else
    expect_contains( cands, "libmp3lame.so" );
    // Same asymmetry as macOS: this very box has libmp3lame.so.0 and no
    // libmp3lame.so, so the versioned form is not optional.
    expect_contains( cands, "libmp3lame.so.0" );
#endif

    // The UI prints this string verbatim. One entry is not a list, and a list
    // the reader cannot parse is no better than the advice it replaced.
    const bool reads_as_list = cands.find( ", " ) != std::string::npos;
    std::printf( "  %-26s %s\n", "reads as a list",
                 reads_as_list ? "yes" : "NO -- single entry" );
    if( !reads_as_list ) {
        ++failures;
    }

    std::printf( "\n%s\n\n", failures ? "FAILED" : "PASSED" );
    return failures ? 1 : 0;
}
