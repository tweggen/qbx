cmake_minimum_required(VERSION 3.16)
project(smaragd VERSION 1.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    # Optimized-with-asserts is the DEFAULT (proposal 27 M4). The project shipped
    # at -O0 for its whole life, which made the DSP paths (vocoder, resampler,
    # Rubber Band driving loops) 10-20x slower than the same code at -O2 — the
    # reported "minutes to warp a minute of audio". RelWithDebInfo here is
    # customized below: -O2 -g WITHOUT -DNDEBUG, because the engine's safety
    # nets (twRtThreadGuard, teardown asserts) must stay armed in every build
    # developers and tests actually run. Explicit -DCMAKE_BUILD_TYPE=Debug
    # still gives the classic -O0 debugging build.
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()
if(NOT MSVC)
    # Keep asserts in optimized builds (strip NDEBUG from the standard flags).
    string(REPLACE "-DNDEBUG" "" CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")
    string(REPLACE "-DNDEBUG" "" CMAKE_C_FLAGS_RELWITHDEBINFO "${CMAKE_C_FLAGS_RELWITHDEBINFO}")
endif()

# Ensure Debug builds include full source line information for LLDB/GDB
if(NOT MSVC)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug" OR NOT CMAKE_BUILD_TYPE)
        # -g = full debug info, -O0 = no optimization (preserves line numbers)
        # Set CMAKE_CXX_FLAGS directly (not _DEBUG variant) for single-config generators
        string(APPEND CMAKE_CXX_FLAGS " -g -O0 -fno-omit-frame-pointer")
        string(APPEND CMAKE_C_FLAGS " -g -O0 -fno-omit-frame-pointer")

        # macOS: Keep debug symbols in executable (don't strip)
        if(APPLE)
            string(APPEND CMAKE_EXE_LINKER_FLAGS " -Wl,-dead_strip_dylibs")
        endif()

        message(STATUS "Debug build: full source line info enabled (-g -O0 -fno-omit-frame-pointer)")
        message(STATUS "To debug with line numbers, use:")
        message(STATUS "  lldb -s .lldbinit ./smaragd/build/bin/smaragd.app/Contents/MacOS/smaragd")
    endif()
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(QT NAMES Qt6 Qt5 COMPONENTS Core Widgets Xml REQUIRED)
find_package(Qt${QT_VERSION_MAJOR} COMPONENTS Core Widgets Xml REQUIRED)
message(STATUS "smaragd: using Qt ${QT_VERSION_MAJOR} (${Qt${QT_VERSION_MAJOR}_VERSION})")

set(SMARAGD_MACOS   OFF)
set(SMARAGD_WINDOWS OFF)
set(SMARAGD_LINUX   OFF)
if(APPLE)
    message(STATUS "smaragd: target platform = macOS")
    set(SMARAGD_MACOS ON)
elseif(WIN32)
    message(STATUS "smaragd: target platform = Windows")
    set(SMARAGD_WINDOWS ON)
elseif(UNIX)
    message(STATUS "smaragd: target platform = Linux")
    set(SMARAGD_LINUX ON)
else()
    message(WARNING "smaragd: unrecognised target platform")
endif()

option(ENABLE_ALSA       "Enable ALSA audio backend (Linux)"          ${SMARAGD_LINUX})
option(ENABLE_PIPEWIRE   "Enable PipeWire audio backend (Linux)"      OFF)
option(ENABLE_PULSEAUDIO "Enable PulseAudio audio backend (Linux)"    OFF)
option(ENABLE_JACK       "Enable JACK audio backend (cross-platform)" OFF)
option(ENABLE_WASAPI     "Enable WASAPI audio backend (Windows)"      ${SMARAGD_WINDOWS})
option(ENABLE_COREAUDIO  "Enable CoreAudio audio backend (macOS)"     ${SMARAGD_MACOS})

if(MSVC)
    add_compile_options(/W3 /permissive-)
    add_compile_definitions(_USE_MATH_DEFINES NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    add_compile_options(-Wall)

    # Address Sanitizer (detects memory errors: use-after-free, buffer overflows, etc.)
    # Enable with: -DENABLE_ASAN=ON
    option(ENABLE_ASAN "Enable Address Sanitizer for memory error detection" OFF)
    if(ENABLE_ASAN)
        message(STATUS "smaragd: Address Sanitizer ENABLED (will catch memory errors)")
        add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address)
    endif()
endif()

enable_testing()

add_subdirectory(tw303a)
add_subdirectory(main)

# Register every headless qxa case with CTest, so `ctest` (from build/) runs
# the module unit tests AND the integration suite in one command. The cases
# run from tests/cases/ so their ../test_sawtooth.wav fixture paths resolve.
# CONFIGURE_DEPENDS: without it this glob is evaluated once and cached, so a
# newly added .qxa is silently NOT registered until someone re-runs cmake by
# hand — and `ctest` then reports all-green while never having run the new case.
file(GLOB _QXA_CASES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/cases/*.qxa)
foreach(_case ${_QXA_CASES})
    get_filename_component(_name ${_case} NAME_WE)
    add_test(NAME qxa.${_name}
        COMMAND $<TARGET_FILE:smaragd>
                --test-case ${_case}
                --test-output-dir ${CMAKE_BINARY_DIR}/test-output/${_name}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/tests/cases
    )
endforeach()

# legacy_project_recovery guards a loader bug whose symptom is a HANG, not a
# failure (an element that can never resolve made createObjects() rescan
# forever). Without a timeout a regression would keep ctest running until
# someone noticed, so bound it: the case itself takes a couple of seconds.
if(TEST qxa.legacy_project_recovery)
    set_tests_properties(qxa.legacy_project_recovery PROPERTIES TIMEOUT 300)
endif()

# The AudioUnit qxa cases (proposal 08 M8) host real macOS system AudioUnits, so
# they are meaningful only on macOS; disable them elsewhere rather than let them
# fail. On macOS the stock AUs they use (AULowpass) ship with the OS, so absence
# is not a concern — only OS-version DSP variance, which their loose RMS bands
# absorb.
if(NOT APPLE)
    foreach(_case ${_QXA_CASES})
        get_filename_component(_name ${_case} NAME_WE)
        if(_name MATCHES "^au_")
            set_tests_properties(qxa.${_name} PROPERTIES DISABLED YES)
        endif()
    endforeach()
endif()

message(STATUS "smaragd: ALSA=${ENABLE_ALSA} PipeWire=${ENABLE_PIPEWIRE} "
               "PulseAudio=${ENABLE_PULSEAUDIO} JACK=${ENABLE_JACK} "
               "WASAPI=${ENABLE_WASAPI} CoreAudio=${ENABLE_COREAUDIO}")
