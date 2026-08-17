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
# ASIO additionally needs the Steinberg ASIO SDK dropped into
# smaragd/third_party/asiosdk (license forbids redistributing it as a
# submodule); without the SDK the option is inert and configure warns once.
option(ENABLE_ASIO       "Enable ASIO audio backend (Windows)"        ${SMARAGD_WINDOWS})

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
    # Two properties that together make the suite safe to run with `ctest -j`:
    #
    # SMARAGD_RENDER_TIMEOUT_MS -- SRenderAction's watchdog is a WALL-CLOCK budget
    #   for one render, defaulting to 30 s. It is meant to catch a hung render,
    #   but on a busy machine it fires on a perfectly healthy one, and `-j` makes
    #   the machine busy by construction. Measured under `-j8`: a render advancing
    #   steadily (a page every ~1.6 s rather than ~0.03 s) was killed at 96 % done,
    #   about a second short. <render> appears 147 times across these cases, so
    #   that one constant is what caps the usable -j. Raise it here and let the
    #   TIMEOUT below be the hang guard instead.
    # TIMEOUT -- and it has to BE a guard, or raising the budget would trade a
    #   fast false failure for a slow real one. CTest's default is 1500 s; 600 s
    #   bounds every case (the slowest observed is ~130 s, and that was under an
    #   8-way run plus three other suites on the box). legacy_project_recovery
    #   overrides it below with something tighter still.
    set_tests_properties(qxa.${_name} PROPERTIES
        ENVIRONMENT "SMARAGD_RENDER_TIMEOUT_MS=180000"
        TIMEOUT 600
    )
endforeach()

# --- the cross-process determinism gate (proposal 37 P3c, AC1 + AC2c) --------
#
# The render-vs-render byte gate is a FRESH-PROCESS property (design F4), and a
# .qxa case is one process with a private output directory, so no case can make
# that compare on its own. This test drives two of them through one shared
# output directory; the second case names the first's render as its reference
# with the ordinary assert-file-identical verb, and the driver cmp's the files
# again itself. Pass 2 lives in tests/cases/xproc/ so the CONFIGURE_DEPENDS glob
# above cannot register it as a standalone case (it would fail looking for a
# reference no one had rendered).
add_test(NAME qxa.instrument_render_determinism_xproc
    COMMAND ${CMAKE_COMMAND}
            -DRUNNER=$<TARGET_FILE:smaragd>
            -DCASE1=${CMAKE_CURRENT_SOURCE_DIR}/tests/cases/instrument_render_determinism.qxa
            -DCASE2=${CMAKE_CURRENT_SOURCE_DIR}/tests/cases/xproc/instrument_render_determinism_xproc.qxa
            -DOUTDIR=${CMAKE_BINARY_DIR}/test-output/instrument_render_determinism_xproc
            -DWORKDIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/cases
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_xproc_determinism.cmake
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/tests/cases
)
set_tests_properties(qxa.instrument_render_determinism_xproc PROPERTIES
    ENVIRONMENT "SMARAGD_RENDER_TIMEOUT_MS=180000"
    TIMEOUT 900
)

# mc_legacy_pull_wide is proposal 36 AC B4.6: the LEGACY PULL path, exercised
# wide. SMARAGD_REVAL_WORKERS=0 disables the revalidator entirely, so there is no
# scheduler, no plan and no bound input pages -- every page is materialised by
# the recursive freeze through twStreamingLatch::copyData. That is the path
# assert-meter drives and the one no other AC in the proposal covers, and it is
# the configuration in which B4's extraction of copyData's page acquisition into
# a shared acquirePage() could be wrong with nothing else noticing. It has to be
# set HERE rather than in the .qxa, because the runner reads it before any script
# is parsed. The render timeout is re-stated because set_tests_properties REPLACES
# the ENVIRONMENT property rather than appending to it.
if(TEST qxa.mc_legacy_pull_wide)
    set_tests_properties(qxa.mc_legacy_pull_wide PROPERTIES
        ENVIRONMENT "SMARAGD_RENDER_TIMEOUT_MS=180000;SMARAGD_REVAL_WORKERS=0"
    )
endif()

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

# --- Running the suite in parallel (`ctest -jN`) ------------------------------
#
# The suite is safe to run concurrently. That is a property of the tests, not an
# accident, and it rests on four things that were audited rather than assumed:
#
#  * Per-case artifacts. Every case gets its own --test-output-dir
#    (${CMAKE_BINARY_DIR}/test-output/<case>), and renders, screenshots, capture
#    dumps and sidecar-root all resolve THROUGH it (SApplication::testOutputDir).
#    Nothing a case writes lands in the shared WORKING_DIRECTORY.
#  * Project save targets. The handful of cases that save a .qxp write to
#    ../../build/<name>.qxp, i.e. the build root. Those names are unique per case
#    (au_missing_resave, au_slot_resave, clip_properties_actions,
#    exact_stretch_roundtrip, plugin_missing_resave, plugin_remove_restores_param,
#    plugin_slot_resave, takes_roundtrip, warp_anchors_roundtrip) and each is
#    written and read back by ITS OWN case only. Everything under tests/ that a
#    case loads is a committed, read-only fixture.
#  * The derived-data sidecar store. One shared per-user directory, and cases
#    using the same fixture derive the same key, so concurrent stores of one key
#    are routine. They are safe because each writer stages its own temp file --
#    see tw303a/sidecar/CONTRACT.md inv. 2, which is exactly the invariant that
#    had to be repaired to make `-j` sound.
#  * The per-user config (smaragd.ini, plugincache.json). Qt's QSettings takes a
#    QLockFile, and the plugin cache is written through QSaveFile, so a
#    concurrent write is a lost update at worst, never a torn file. Headless
#    --test-case runs take no file log sink at all (see main.cpp), so the
#    rotating smaragd.log is not shared either.
#
# RUN_SERIAL is for the opposite kind of test: ones whose assertion is about
# WALL-CLOCK LATENCY, so a loaded machine changes the answer. They are not
# isolation failures -- they measure the box, and under `-j` the rest of the
# suite IS the load. RUN_SERIAL gives them the machine to themselves; it does not
# make them immune to load from outside this ctest invocation.
#
# qxa.log_dock_scale -- log-stress asserts NO SINGLE event-loop pump exceeds
#     maxStallMs=50 while the log dock drains 300k records. A stall cap is a
#     latency bound on the GUI thread, so background load moves it directly.
#
# The other one, twlog_test, is marked in tw303a/CMakeLists.txt instead:
# set_tests_properties() only reaches tests added in the SAME directory scope, so
# marking it from here silently does nothing (`if(TEST twlog_test)` is false
# here) -- which is exactly how it was missed the first time.
if(TEST qxa.log_dock_scale)
    set_tests_properties(qxa.log_dock_scale PROPERTIES RUN_SERIAL TRUE)
endif()

# --- MIDI out (proposal 37 P7b) ----------------------------------------------
#
# These four PLAY, in real time, and assert WHEN a MIDI message reached the wire
# against the audio capture backend's own block log. That makes them
# wall-clock-latency tests in exactly the sense the section above describes, so
# they get the machine to themselves within the run. Two of them additionally
# write a per-user setting (midi/chaseNoteOns, midi/outOffsetMs) that the pump
# reads at every transport start, so a concurrent playback case would inherit
# it -- another reason the set is serialized rather than merely paced.
#
# midi_out_render_silent and midi_out_backend_reject neither play nor mutate a
# setting, so they stay parallel.
foreach(_serial midi_out_capture midi_out_chase_and_stop midi_out_loop_wrap
                midi_out_offset_and_folder midi_options_page)
    if(TEST qxa.${_serial})
        set_tests_properties(qxa.${_serial} PROPERTIES RUN_SERIAL TRUE)
    endif()
endforeach()

# SMARAGD_CAPTURE_SPEED=1 is spelled out rather than left to the default: the
# testkit maps a MIDI host time onto a project frame through the audio capture
# backend's block log, and any other pacing changes what a project frame means
# in wall-clock terms while the MIDI due times keep meaning wall-clock (D6).
foreach(_paced midi_out_capture midi_out_chase_and_stop midi_out_loop_wrap
               midi_out_offset_and_folder)
    if(TEST qxa.${_paced})
        set_tests_properties(qxa.${_paced} PROPERTIES
            ENVIRONMENT "SMARAGD_RENDER_TIMEOUT_MS=180000;SMARAGD_CAPTURE_SPEED=1")
    endif()
endforeach()

# AC5: `assert-midi-out` must REJECT when there is no capture MIDI recorder,
# instead of reporting "0 messages, all good". The only way to put the app in
# that state is to choose another backend before it starts.
if(TEST qxa.midi_out_backend_reject)
    set_tests_properties(qxa.midi_out_backend_reject PROPERTIES
        ENVIRONMENT "SMARAGD_RENDER_TIMEOUT_MS=180000;SMARAGD_MIDI_BACKEND=null")
endif()

message(STATUS "smaragd: ALSA=${ENABLE_ALSA} PipeWire=${ENABLE_PIPEWIRE} "
               "PulseAudio=${ENABLE_PULSEAUDIO} JACK=${ENABLE_JACK} "
               "WASAPI=${ENABLE_WASAPI} CoreAudio=${ENABLE_COREAUDIO} "
               "ASIO=${ENABLE_ASIO}")
