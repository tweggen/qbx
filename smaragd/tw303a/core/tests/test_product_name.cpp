// test_product_name.cpp — the product name reaches the ENGINE, with the value
// the build configured.
//
// QBX-137 / plan 51 stage 1. This test exists in tw303a/ rather than beside
// the app on purpose, and it links tw_core and NOTHING ELSE. That is the point
// of it.
//
// The plan's first draft put a product-name accessor in app_model, "the lowest
// layer that can carry it". Review found that 21 of the 41 surveyed name
// literals are in tw303a/, and engine code may not include an app header
// (tools/check_layering.py rule 1; the tw_* libraries do not link app
// targets) — so that accessor could not have served half the sweep. The fix
// was to carry the name in the generated header, which
// smaragd/CMakeLists.txt puts on EVERY target's include path.
//
// A test that merely included the header from app code would not have caught
// that mistake. This one does: if the carrier ever moves somewhere the engine
// cannot reach, this target stops compiling.
//
// It also catches a STALE generated header. CMake passes the configured name a
// second time, as TEST_EXPECTED_PRODUCT_NAME, so the test compares two paths
// out of the same CMake variable. Equal means configure_file ran and the
// header on disk matches the cache. Asserting only "non-empty" would pass
// against a header generated before the value changed.

#include <cstdio>
#include <cstring>

#include "smaragd_version.h"

#ifndef TEST_EXPECTED_PRODUCT_NAME
#error "TEST_EXPECTED_PRODUCT_NAME must be passed by CMake; see tw_module_test in tw303a/CMakeLists.txt"
#endif

static int failures = 0;

static void check( bool ok, const char *what )
{
    std::printf( "  %-58s %s\n", what, ok ? "ok" : "FAILED" );
    if( !ok ) {
        ++failures;
    }
}

int main()
{
    std::printf( "\n-- the product name reaches the engine (QBX-137, plan 51) --\n" );

    check( std::strlen( SMARAGD_PRODUCT_NAME ) > 0,
           "SMARAGD_PRODUCT_NAME is not empty" );

    const bool matches =
        std::strcmp( SMARAGD_PRODUCT_NAME, TEST_EXPECTED_PRODUCT_NAME ) == 0;
    if( !matches ) {
        std::printf( "    header says \"%s\", CMake configured \"%s\"\n",
                     SMARAGD_PRODUCT_NAME, TEST_EXPECTED_PRODUCT_NAME );
        std::printf( "    A generated header that disagrees with the cache is a\n" );
        std::printf( "    STALE header: re-run cmake. If it persists, configure_file\n" );
        std::printf( "    in smaragd/cmake/SmaragdVersion.cmake is not running.\n" );
    }
    check( matches, "it equals the name CMake configured" );

    // The identity keys must NOT be derived from the display name. Nothing
    // here can enforce that — tools/check_product_name.py job 2 does — but a
    // product name that had been set to one of them would mean the two
    // populations had been confused, which is plan 51's central error mode.
    check( std::strcmp( SMARAGD_PRODUCT_NAME, "com.smaragd.media" ) != 0
               && std::strcmp( SMARAGD_PRODUCT_NAME, "dev.tweggen.smaragd" ) != 0
               && std::strcmp( SMARAGD_PRODUCT_NAME, "smaragd" ) != 0,
           "it is not one of the frozen identity keys" );

    std::printf( "\n%s\n\n", failures ? "FAILED" : "PASSED" );
    return failures ? 1 : 0;
}
