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

# Network and Concurrent arrive with proposal 38 (the media browser). Network
# is the first HTTP client in this tree and is not used until gate 4 -- it is
# named here so that gate does not have to touch the root file. Concurrent is a
# SEPARATE Qt component and was absent from the tree entirely; forgetting it is
# a link error at the END of a gate rather than at its start.
find_package(QT NAMES Qt6 Qt5 COMPONENTS Core Widgets Xml Network Concurrent REQUIRED)
find_package(Qt${QT_VERSION_MAJOR} COMPONENTS Core Widgets Xml Network Concurrent REQUIRED)
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
#  * The per-user config (smaragd.ini, plugincache.v<n>.json). Qt's QSettings takes a
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

# --- the media browser (proposal 38 gate 2) ----------------------------------
#
# These three drive the REAL SMediaBrowserPanel, and the panel PERSISTS its
# state: media/lastSourceId, media/lastPath/<sourceId>, media/categoryMask and
# media/searchRecursive all reach the shared per-user smaragd.ini. Suppressing
# that under --test-case was considered and rejected (design AC 9): it would
# make the gate test something other than the shipping code. So they get the
# `midi_options_page` treatment instead -- RUN_SERIAL, each declaring in its
# header that it OWNS those four keys, and each restoring them to their SOpt
# defaults so the INI comes back byte-identical.
#
# They are NOT wall-clock tests: every verb waits for the panel to go idle
# rather than sleeping, so RUN_SERIAL here is about the shared INI and nothing
# else.
foreach(_media media_browser_browse media_browser_search
                media_browser_drag_local media_browser_drag_belowlast)
    if(TEST qxa.${_media})
        set_tests_properties(qxa.${_media} PROPERTIES RUN_SERIAL TRUE)
    endif()
endforeach()

# --- the deferred `media:` drop (proposal 38 gate 3) --------------------------
#
# Same INI argument as the three above (it writes media/lastSourceId and
# media/lastPath/testdelay and restores both), plus two environment variables
# that CANNOT live in the .qxa because they are read before any script runs:
#
#   SMARAGD_MEDIA_TEST_SOURCE=1 registers SDelayedLocalSource, the test-only
#     provider that is remote-SHAPED (NeedsFetch + a delayed copy). Without it
#     the deferred branch has nothing to be driven by; a user must never see
#     "Delayed local (test)" in the source combo, which is why it is a knob and
#     not a build flag.
#   SMARAGD_MEDIA_CACHE_DIR points the cache into THIS case's own output
#     directory. The cache is otherwise a shared per-user directory, and the
#     case asserts an exact FETCH COUNT on a cold key -- which a hit inherited
#     from a previous run, or from another process under `ctest -j4`, would
#     silently make wrong. The same reason SMARAGD_SIDECAR_DIR exists.
#
# ENVIRONMENT is REPLACED, not appended, so the render timeout is restated.
if(TEST qxa.media_drop_deferred)
    set_tests_properties(qxa.media_drop_deferred PROPERTIES
        RUN_SERIAL TRUE
        ENVIRONMENT "SMARAGD_RENDER_TIMEOUT_MS=180000;SMARAGD_MEDIA_TEST_SOURCE=1;SMARAGD_MEDIA_CACHE_DIR=${CMAKE_BINARY_DIR}/test-output/media_drop_deferred/mediacache")
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

# --- Nextcloud accounts / Options -> Media (proposal 38 GATE 5b) -------------
#
# All three write real per-user keys under media/nextcloud/qxatest/* into the
# SHARED smaragd.ini -- RUN_SERIAL, each owns and restores every key it
# touches, verified by reading the file directly (assert-settings-file), not
# through a QSettings cache.
#
# media_options_page and media_secret_redaction additionally need a backend
# that actually PERSISTS to disk (AC 8/9/17 are specifically about what lands
# on disk), so they override SMARAGD_SECRET_BACKEND to `dpapi` -- `memory` is
# the --test-case default (§B.8 rule 5, T15) and would round-trip fine
# in-process, but never touches the INI at all, so it cannot gate any of
# those three ACs.
#
# media_options_no_store instead FORCES `none` -- SMARAGD_SECRET_BACKEND is
# read ONCE per process by SMediaAccountManager's constructor, so seeing that
# backend's "Remember is disabled" behaviour (AC 10) needs its own CTest entry
# with its own environment, the same way the other two need `dpapi` rather
# than the default.
#
# Note ENVIRONMENT is REPLACED, not appended, so the render timeout is
# restated in both blocks below.
foreach(_media_dpapi media_options_page media_secret_redaction)
    if(TEST qxa.${_media_dpapi})
        set_tests_properties(qxa.${_media_dpapi} PROPERTIES
            RUN_SERIAL TRUE
            ENVIRONMENT "SMARAGD_RENDER_TIMEOUT_MS=180000;SMARAGD_SECRET_BACKEND=dpapi")
    endif()
endforeach()
if(TEST qxa.media_options_no_store)
    set_tests_properties(qxa.media_options_no_store PROPERTIES
        RUN_SERIAL TRUE
        ENVIRONMENT "SMARAGD_RENDER_TIMEOUT_MS=180000;SMARAGD_SECRET_BACKEND=none")
endif()

# --- the end-to-end WebDAV cases (proposal 38 GATE 5c) -----------------------
#
# The first time the WebDAV connector is driven from inside the app: an account
# pointed at an in-process SWebDavStub, the REAL SMediaBrowserPanel browsing and
# searching it, and a drop out of it placing a clip whose RENDERED AUDIO is
# asserted (PROPFIND -> GET -> cache -> project copy -> add-sample). The stub
# binds 127.0.0.1:0 -- an OS-assigned port, never a fixed one -- which is what
# lets these run beside three other processes without a port collision; the
# `media-webdav-stub` verb writes the resulting URL straight into the account,
# because a .qxa cannot interpolate a run-time value into a later attribute.
#
# RUN_SERIAL for the SETTINGS reason, not a timing one (nothing here asserts a
# wall clock; every verb waits for the panel to go idle): both write
# media/lastSourceId, media/lastPath/nextcloud:qxastub, media/categoryMask,
# media/searchRecursive and media/nextcloud/qxastub/* into the shared
# smaragd.ini, and both restore all of it.
#
# SMARAGD_SECRET_BACKEND=memory is spelled out rather than inherited from the
# --test-case default (proposal 38 §B.8 rule 5, T15). The account only has to
# work for THIS process, and `memory` writes no INI key and no keychain item at
# all -- which is exactly why, unlike the gate-5b options cases, neither of
# these needs `dpapi`. Leaving it implicit is how a default change would
# silently start writing real ciphertext from a case that never asked for it.
#
# media_webdav_drop additionally needs SMARAGD_MEDIA_CACHE_DIR pointed into its
# OWN output directory: the media cache is otherwise a shared per-user
# directory (the same reason SMARAGD_SIDECAR_DIR exists), and the case's "a
# repeat drop of the same remote file does not re-fetch and does not make a
# second project copy" claim would otherwise be measuring a hit inherited from
# a previous run.
#
# ENVIRONMENT is REPLACED, not appended, so the render timeout is restated.
if(TEST qxa.media_webdav_browse)
    set_tests_properties(qxa.media_webdav_browse PROPERTIES
        RUN_SERIAL TRUE
        ENVIRONMENT "SMARAGD_RENDER_TIMEOUT_MS=180000;SMARAGD_SECRET_BACKEND=memory")
endif()
if(TEST qxa.media_webdav_drop)
    set_tests_properties(qxa.media_webdav_drop PROPERTIES
        RUN_SERIAL TRUE
        ENVIRONMENT "SMARAGD_RENDER_TIMEOUT_MS=180000;SMARAGD_SECRET_BACKEND=memory;SMARAGD_MEDIA_CACHE_DIR=${CMAKE_BINARY_DIR}/test-output/media_webdav_drop/mediacache")
endif()

# --- the live lane (proposal 21 L1b) -----------------------------------------
#
# All five PLAY (or monitor) in real time against a paced FILE INPUT and assert
# what the capture backend was handed, so they are wall-clock tests in exactly
# the sense the section above describes: RUN_SERIAL, and SMARAGD_CAPTURE_SPEED=1
# spelled out rather than left to the default (the pacing is what makes a frame
# count mean a wall-clock duration on both sides of the measurement).
#
# SMARAGD_AUDIO_INPUT_BACKEND has to be set HERE and not in the .qxa: the runner
# reads it at the first createAudioInput() call, long after the script is
# parsed, and there is no verb that could set it earlier. The paths are relative
# to the shared WORKING_DIRECTORY (tests/cases), like every other fixture path
# in a case. Both fixtures are committed and regenerable:
# test_autosaw.wav by tests/tools/gen_auto_fixture.py, test_position.wav by
# `gen_position_fixture --verify`.
#
# Note ENVIRONMENT is REPLACED, not appended, so the render timeout is restated.
foreach(_live monitor_through_chain monitor_folder_closure arm_during_playback
              render_while_armed feel_flow_bounce_while_monitoring)
    if(TEST qxa.${_live})
        set_tests_properties(qxa.${_live} PROPERTIES
            RUN_SERIAL TRUE
            ENVIRONMENT "SMARAGD_RENDER_TIMEOUT_MS=180000;SMARAGD_CAPTURE_SPEED=1;SMARAGD_AUDIO_INPUT_BACKEND=file:../test_autosaw.wav")
    endif()
endforeach()
# --- audio recording (proposal 21 L3b) ---------------------------------------
#
# All three RECORD in real time against the paced position-encoded file input
# and assert where the captured material was PLACED, so they are wall-clock
# tests in exactly the sense the section above describes: RUN_SERIAL, and
# SMARAGD_CAPTURE_SPEED=1 spelled out rather than left to the default.
#
# SMARAGD_AUDIO_INPUT_LATENCY_FRAMES=4800 is the driver's CLAIM and nothing
# else -- FileAudioInput does not actually delay by it. That is the point: the
# gate is on the CONVERSION (design D6) applying the reported latency with the
# right sign and the right scaling, which is a closed form in integers, not on
# the physics of a real device, which no headless run can see. Together with
# the capture backend's own 1024-frame output latency it makes the total
# compensation exactly -5824 frames, and +20 ms of recordingOffsetMs exactly
# 960 frames more.
#
# record_offset_zero WRITES audio/recordingOffsetMs/qxa-record-input through
# set-option and puts it back; it OWNS that key, and no other case reads it.
# The device name is deliberately one no real device can have, and the case's
# tracks NAME it through set-track-input, because the recorder resolves the
# offset key from the armed track's input first and the app-wide
# audio/inputDeviceId setting only as a fallback. It used to write the key for
# "default" and rely on that fallback, which made the assertion depend on
# whether the person running the suite had ever picked an input device in
# Edit -> Options -> Audio -- and it failed silently, because a missing key
# reads 0.0, the same answer an uncalibrated device gives.
#
# Note ENVIRONMENT is REPLACED, not appended, so the render timeout is restated.
foreach(_rec record_offset_zero record_loop_takes record_punch
              record_while_monitoring record_stays_armed)
    if(TEST qxa.${_rec})
        set_tests_properties(qxa.${_rec} PROPERTIES
            RUN_SERIAL TRUE
            ENVIRONMENT "SMARAGD_RENDER_TIMEOUT_MS=180000;SMARAGD_CAPTURE_SPEED=1;SMARAGD_AUDIO_INPUT_BACKEND=file:../test_position.wav;SMARAGD_AUDIO_INPUT_LATENCY_FRAMES=4800")
    endif()
endforeach()

# --- transport polish (proposal 21 L5) ---------------------------------------
#
# THE METRONOME CASES have no audio input at all: the click is a live source
# that is a pure function of POSITION (twmetronome.h), so the `null` default a
# headless run already takes is exactly right - the same argument the
# live-instrument six make. They ARE wall-clock cases in the sense the section
# above describes (the click is produced by the pump and consumed by the RT
# callback, and the measurement is the capture backend's recording), so
# RUN_SERIAL with SMARAGD_CAPTURE_SPEED=1 spelled out.
foreach(_met metronome_click metronome_render_identity metronome_toggle_during_playback)
    if(TEST qxa.${_met})
        set_tests_properties(qxa.${_met} PROPERTIES
            RUN_SERIAL TRUE
            ENVIRONMENT "SMARAGD_RENDER_TIMEOUT_MS=180000;SMARAGD_CAPTURE_SPEED=1")
    endif()
endforeach()

# --- AC-g1: the follow-playhead scroll HOLD ----------------------------------
#
# A manual scroll suspends re-paging for FOLLOW_HOLD_MS (5 s), measured
# against a real QElapsedTimer while real-time playback runs — a wall-clock
# case in exactly the sense the section above describes, so RUN_SERIAL with
# SMARAGD_CAPTURE_SPEED=1 spelled out (it is already the default; stating it
# is what makes the ~7 s of wait-ms below mean what the comments in the .qxa
# say they mean rather than an accident of whatever the default happens to be).
if(TEST qxa.follow_scroll_hold)
    set_tests_properties(qxa.follow_scroll_hold PROPERTIES
        RUN_SERIAL TRUE
        ENVIRONMENT "SMARAGD_RENDER_TIMEOUT_MS=180000;SMARAGD_CAPTURE_SPEED=1")
endif()

# THE COUNT-IN / PRE-ROLL CASES record, so they take the same paced,
# position-encoded input and the same reported latency as the L3b record four -
# which is what makes `compensationFrames="-5824"` a closed form in both.
#
# record_count_in WRITES transport/countInBars and record_pre_roll writes
# transport/preRollBars through set-count-in / set-pre-roll, and each puts its
# own key back; between them they OWN those two keys, and no other case reads
# them.
#
# metronome_click_while_recording records too (a plain record-start with no
# count-in/pre-roll, straight from stopped into recording), so it takes the
# same paced input. It OWNS metronome/clickWhileRecording and puts it back.
foreach(_pre record_count_in record_pre_roll metronome_click_while_recording)
    if(TEST qxa.${_pre})
        set_tests_properties(qxa.${_pre} PROPERTIES
            RUN_SERIAL TRUE
            ENVIRONMENT "SMARAGD_RENDER_TIMEOUT_MS=180000;SMARAGD_CAPTURE_SPEED=1;SMARAGD_AUDIO_INPUT_BACKEND=file:../test_position.wav;SMARAGD_AUDIO_INPUT_LATENCY_FRAMES=4800")
    endif()
endforeach()

if(TEST qxa.monitor_latency)
    set_tests_properties(qxa.monitor_latency PROPERTIES
        RUN_SERIAL TRUE
        ENVIRONMENT "SMARAGD_RENDER_TIMEOUT_MS=180000;SMARAGD_CAPTURE_SPEED=1;SMARAGD_AUDIO_INPUT_BACKEND=file:../test_position.wav")
endif()

# --- live instruments (proposal 21 L2) ---------------------------------------
#
# All six PLAY a live instrument in real time and assert what the capture
# backend was handed, so they are wall-clock tests in exactly the sense the
# section above describes: RUN_SERIAL, with SMARAGD_CAPTURE_SPEED=1 spelled out
# rather than left to the default. Three of them bound a LATENCY directly - the
# note-on-to-audio onset (live_instrument_play), the MIDI-thru delta
# (live_instrument_thru) and the hand-back gap (live_instrument_disarm_playback)
# - and those numbers measure the box as well as the code.
#
# No SMARAGD_AUDIO_INPUT_BACKEND here, unlike the L1b five: a MIDI-armed
# instrument track has NO audio input source at all (design section 3 case
# (ii)), so the `null` default a headless run already takes is exactly right.
# The MIDI half arrives through CaptureMidiInput, which
# SMARAGD_MIDI_BACKEND=capture already selects by default.
foreach(_liveinst live_instrument_play live_instrument_merge
                  live_instrument_disarm_playback live_instrument_ownership
                  live_instrument_thru live_instrument_keyboard)
    if(TEST qxa.${_liveinst})
        set_tests_properties(qxa.${_liveinst} PROPERTIES
            RUN_SERIAL TRUE
            ENVIRONMENT "SMARAGD_RENDER_TIMEOUT_MS=180000;SMARAGD_CAPTURE_SPEED=1")
    endif()
endforeach()

# --- MIDI recording (proposal 21 L4 = 37 P8b) --------------------------------
#
# All three RECORD a real-time-paced performance and assert where it was PLACED
# on the timeline, so they are wall-clock tests in exactly the sense the section
# above describes: RUN_SERIAL, with SMARAGD_CAPTURE_SPEED=1 spelled out rather
# than left to the default. The recorder maps a MIDI host time onto a project
# frame through the playhead clock, and any other pacing changes what a project
# frame means in wall-clock terms while the injected messages keep meaning
# wall-clock.
#
# No SMARAGD_AUDIO_INPUT_BACKEND, for the same reason the live-instrument six
# have none: a MIDI-armed track has NO audio input at all (design section 3
# case (ii)), so the `null` default a headless run already takes is right. The
# MIDI half arrives through CaptureMidiInput, which SMARAGD_MIDI_BACKEND=capture
# already selects by default.
#
# midi_record_modes WRITES midi/recordMode and midi/recordQuantize through
# set-option and puts both back; it OWNS those two keys, and no other case
# reads them.
foreach(_mrec midi_record_placement midi_record_modes midi_record_loop_takes
              midi_record_stays_armed)
    if(TEST qxa.${_mrec})
        set_tests_properties(qxa.${_mrec} PROPERTIES
            RUN_SERIAL TRUE
            ENVIRONMENT "SMARAGD_RENDER_TIMEOUT_MS=180000;SMARAGD_CAPTURE_SPEED=1")
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
