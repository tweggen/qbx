# tw303a — the Smaragd audio engine, split into modules (proposal 14, Phase 2).
#
# Each module is a STATIC library named tw_<module> that
#   - publishes ONLY its own headers (<module>/include, included as
#     "tw/<module>/<header>.h"),
#   - declares its dependencies explicitly via target_link_libraries.
# A module therefore physically cannot include a header of a module it does
# not link — the dependency DAG in proposal 14 §4.1 is enforced by the build.
#
# The umbrella INTERFACE target `tw303a` links every module and additionally
# publishes compat/ (forwarding headers under the pre-modularization include
# strings) so the application keeps compiling unchanged until its own
# modularization phase.

function(tw_module NAME)
    cmake_parse_arguments(TWM "" "" "SOURCES;DEPS" ${ARGN})
    add_library(tw_${NAME} STATIC ${TWM_SOURCES})
    target_include_directories(tw_${NAME}
        PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/${NAME}/include
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/${NAME}/src
    )
    if(TWM_DEPS)
        target_link_libraries(tw_${NAME} PUBLIC ${TWM_DEPS})
    endif()
endfunction()

# --- E1 core: types, format, fraction, conversion, logging/exceptions -------
tw_module(core
    SOURCES
        core/include/tw/core/twtypes.h
        core/include/tw/core/twformat.h
        core/include/tw/core/twconvert.h
        core/include/tw/core/twfraction.h
        core/include/tw/core/twdomains.h
        core/include/tw/core/twframerange.h
        core/include/tw/core/exc.h
        core/include/tw/core/twsyslog.h
        core/include/tw/core/twlog.h
        core/include/tw/core/audio_frame.h
        core/include/tw/core/generation_promise.h
        core/include/tw/core/twcontenthash.h
        core/include/tw/core/twwarpmap.h
        core/include/tw/core/position_code.h
        core/src/twcontenthash.cc
        core/src/twwarpmap.cc
        core/src/generation_promise.cc
        core/src/twlog.cc
        core/src/twconvert.cc
        core/src/twfraction.cpp
    DEPS Qt${QT_VERSION_MAJOR}::Core
)

# --- E2 pages: frozen output pages, IOVector, capture page pool -------------
tw_module(pages
    SOURCES
        pages/include/tw/pages/tw_output_page.h
        pages/include/tw/pages/page_interface.h
        pages/include/tw/pages/io_vector.h
        pages/include/tw/pages/capture_page_pool.h
        pages/src/tw_output_page.cc
        pages/src/io_vector.cc
        pages/src/capture_page_pool.cc
    DEPS tw_core
)

# --- E3 graph: the component contract, latches, views, environment ----------
tw_module(graph
    SOURCES
        graph/include/tw/graph/twcomponent.h
        graph/include/tw/graph/twlatch.h
        graph/include/tw/graph/tw_freeze_context.h
        graph/include/tw/graph/tw_frozen_inputs.h
        graph/include/tw/graph/tw_page_plan.h
        graph/include/tw/graph/twview.h
        graph/include/tw/graph/twnegotiator.h
        graph/include/tw/graph/tw303aenv.h
        graph/src/twcomponent.cc
        graph/src/twlatch.cc
        graph/src/twstreaminglatch.cc
        graph/src/tw_freeze_context.cc
        graph/src/tw_frozen_inputs.cc
        graph/src/twview.cc
        graph/src/twnegotiator.cc
        graph/src/tw303aenv.cc
    DEPS tw_core tw_pages
)

# --- E4 sources: sample data, readers, resampling, grain --------------------
tw_module(sources
    SOURCES
        sources/include/tw/sources/twrandomsource.h
        sources/include/tw/sources/twsamplesource.h
        sources/include/tw/sources/twsamplereader.h
        sources/include/tw/sources/twresampledsource.h
        sources/include/tw/sources/twresampler.h
        sources/include/tw/sources/twcapturingsource.h
        sources/include/tw/sources/twloopreader.h
        sources/include/tw/sources/twgrainsource.h
        sources/include/tw/sources/twgrainparams.h
        sources/include/tw/sources/twpagedvocoder.h
        sources/include/tw/sources/twwavinput.h
        sources/include/tw/sources/twwav.h
        sources/src/twsamplesource.cc
        sources/src/twsamplereader.cc
        sources/src/twresampledsource.cc
        sources/src/twresampler.cc
        sources/src/twcapturingsource.cc
        sources/src/twloopreader.cc
        sources/src/twgrainsource.cc
        sources/src/twpagedvocoder.cc
        sources/src/twwavinput.cc
        sources/src/twwav.cc
    DEPS tw_core tw_pages tw_graph tw_sidecar
)

# tw_sources decodes imported sample files (WAV fast-path in twSampleSource plus
# every other format — MP3/FLAC/AIFF/Ogg/Opus and non-16-bit WAV — via libsndfile).
# Same discovery as tw_sinks below (vcpkg CMake-config on Windows, pkg-config on
# macOS/Linux); this is a third-party link, not a tw_ module edge, so the layering
# checker is unaffected. MP3 read needs a libsndfile built with mpg123 (vcpkg's
# `mpeg` default feature; Homebrew's mpg123 dep).
if(SMARAGD_WINDOWS)
    find_package(SndFile CONFIG REQUIRED)
    target_link_libraries(tw_sources PRIVATE SndFile::sndfile)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SNDFILE_SOURCES REQUIRED IMPORTED_TARGET sndfile)
    target_link_libraries(tw_sources PRIVATE PkgConfig::SNDFILE_SOURCES)
endif()


# --- E5 dsp: unit generators and filters -------------------------------------
tw_module(dsp
    SOURCES
        dsp/include/tw/dsp/twosc.h
        dsp/include/tw/dsp/twsaw.h
        dsp/include/tw/dsp/twsimplesaw.h
        dsp/include/tw/dsp/twmoog.h
        dsp/include/tw/dsp/twwhitenoise.h
        dsp/include/tw/dsp/twconstant.h
        dsp/include/tw/dsp/twpipe.h
        dsp/include/tw/dsp/twtestseq.h
        dsp/src/twosc.cc
        dsp/src/twsaw.cc
        dsp/src/twsimplesaw.cc
        dsp/src/twmoog.cc
        dsp/src/twwhitenoise.cc
        dsp/src/twconstant.cc
        dsp/src/twpipe.cc
        dsp/src/twtestseq.cc
    DEPS tw_core tw_graph
)

# --- E6 mix: arrangement composition (track mix, mixer, rewire) --------------
tw_module(mix
    SOURCES
        mix/include/tw/mix/twtrackmix.h
        mix/include/tw/mix/twmixer.h
        mix/include/tw/mix/twrewire.h
        mix/src/twtrackmix.cc
        mix/src/twmixer.cc
        mix/src/twrewire.cc
    DEPS tw_core tw_pages tw_graph
)

# --- E7 plugins: plugin ABI, registry, hosting chain --------------------------
tw_module(plugins
    SOURCES
        plugins/include/tw/plugins/twplugin.h
        plugins/include/tw/plugins/twplugindescriptor.h
        plugins/include/tw/plugins/twpluginchain.h
        plugins/include/tw/plugins/twplugininsert.h
        plugins/include/tw/plugins/twpluginslotproc.h
        plugins/include/tw/plugins/twpluginsearchpaths.h
        plugins/src/twpluginchain.cc
        plugins/src/twplugininsert.cc
        plugins/src/twpluginslotproc.cc
        plugins/src/twpassthrough.cc
        plugins/src/twnullplugin.cc
        plugins/src/twpluginregistry.cc
        plugins/src/twpluginscancache.h
        plugins/src/twpluginscancache.cc
        plugins/src/twpluginsearchpaths.cc
    DEPS tw_core tw_graph
)

# --- CLAP plugin backend (plugins only) ---------------------------------------
# Proposal 08 M1. CLAP is MIT, header-only and one C ABI on every platform, so
# discovery is simply "is the submodule checked out?" — there is no library to
# find, no pkg-config file, and nothing to install (_env.sh's ensure_submodules
# fetches it from both build.sh and rebuild.sh). Follows the shape of the
# TW_HAVE_RUBBERBAND block this file used to carry: discover, then PRIVATE
# include dirs + a PRIVATE compile definition, then STATUS or WARNING.
#
# PRIVATE is load-bearing. The clap headers and TW_HAVE_CLAP must stay invisible
# to every other target: a public tw/plugins/*.h whose declarations changed with
# TW_HAVE_CLAP would give tw_plugins and its consumers different views of the
# same types (ODR/ABI skew). That is why the backend's own header lives in
# plugins/src/, not plugins/include/.
set(TW_CLAP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/clap)
set(_tw_have_clap FALSE)
if(EXISTS ${TW_CLAP_DIR}/include/clap/clap.h)
    set(_tw_have_clap TRUE)
    target_sources(tw_plugins PRIVATE
        plugins/src/twclapmodule.h
        plugins/src/twclapmodule.cc
        plugins/src/twclapplugin.cc
    )
    target_include_directories(tw_plugins PRIVATE ${TW_CLAP_DIR}/include)
    target_compile_definitions(tw_plugins PRIVATE TW_HAVE_CLAP=1)
    if(SMARAGD_LINUX)
        target_link_libraries(tw_plugins PRIVATE dl)
    endif()
    message(STATUS "tw_plugins: CLAP backend ENABLED (proposal 08 M1)")

    # De-risking spike, NOT shipped and NOT a test gate: loads one .clap with the
    # production loader and prints the factory contents. Under plugins/tools/ so
    # check_logging.py exempts its table (tools/ is an ALLOW_DIR); it needs the
    # same PRIVATE clap includes because it uses the backend's private header.
    add_executable(clap_probe plugins/tools/clap_probe.cc)
    target_link_libraries(clap_probe PRIVATE tw_plugins)
    target_include_directories(clap_probe PRIVATE
        ${TW_CLAP_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/plugins/src
    )

    # Build-time test fixture: a real 2-in/2-out CLAP module built from this
    # repo, so plugins_test can exercise the actual load path without anyone
    # installing a third-party plugin. Written in C so the .clap has no
    # libstdc++/libgcc import and loads wherever ctest runs it. Later milestones
    # reuse it as the headless stereo plugin for qxa render cases.
    add_library(twtestclap MODULE plugins/tests/twtestclap.c)
    target_include_directories(twtestclap PRIVATE ${TW_CLAP_DIR}/include)
    set_target_properties(twtestclap PROPERTIES
        PREFIX ""
        SUFFIX ".clap"
        C_STANDARD 11
        AUTOMOC OFF
    )
    # ALSO drop it next to the app binary (proposal 08 M3). A qxa action script
    # names the module with a RELATIVE path, and SInsertPluginAction resolves a
    # relative path against the .qxa's directory and then against
    # QCoreApplication::applicationDirPath() — so "twtestclap.clap" reaches the
    # fixture whatever the build directory is called, which a path like
    # ../../build/lib/twtestclap.clap would not.
    add_custom_command(TARGET twtestclap POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:twtestclap>
                ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/twtestclap.clap
        COMMENT "Copying the twtestclap fixture next to the app binary (qxa cases)")
else()
    message(WARNING "tw_plugins: third_party/clap not checked out; CLAP plugin hosting DISABLED. Run 'git submodule update --init --recursive' (or ./build.sh, which does it).")
endif()

# --- VST3 plugin backend (plugins only) ---------------------------------------
# Proposal 08 M6, gated by the vst3_probe spike below (which came first and
# proved the MinGW <-> MSVC ABI works before any of this was written).
#
# Same PRIVATE discipline as the CLAP block above, and load-bearing for the same
# reason: the pluginterfaces include directory and TW_HAVE_VST3 must stay
# invisible to every other target, so the backend's headers live in plugins/src/
# and never in plugins/include/ (CONTRACT invariant 4).
#
# The include path needs explaining. The SDK headers include each other as
# "pluginterfaces/base/funknown.h", so the compiler must be able to resolve a
# directory literally NAMED pluginterfaces. Our submodule is checked out as
# third_party/vst3_pluginterfaces (the upstream repository name), which does not
# match, so a bare -I third_party would resolve nothing. Mirroring the 664 KB of
# headers into the build tree at configure time is the portable fix: a symlink
# needs privileges on Windows, and renaming the submodule would churn .gitmodules
# and every checkout for a cosmetic reason. It reruns only on re-configure —
# which is also what has to happen after a submodule bump, so nothing goes stale
# unnoticed.
set(TW_VST3_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/vst3_pluginterfaces)
set(_tw_have_vst3 FALSE)
if(EXISTS ${TW_VST3_DIR}/base/funknown.h)
    set(_tw_have_vst3 TRUE)
    set(TW_VST3_INC ${CMAKE_CURRENT_BINARY_DIR}/vst3_inc)
    file(COPY ${TW_VST3_DIR}/base ${TW_VST3_DIR}/gui ${TW_VST3_DIR}/vst
         DESTINATION ${TW_VST3_INC}/pluginterfaces)

    # The SDK sources we compile. 08_PLUGIN_HOSTING_EXECUTION.md M6 named exactly
    # these four and that list is right for LINKING — but not sufficient for
    # RUNNING: vst3_pluginterfaces ships no vstinitiids.cpp, so base/coreiids.cpp
    # defines only the BASE IIDs and every VST module IID (IComponent,
    # IAudioProcessor, IEditController, the host interfaces) has to be defined by
    # us. That is plugins/src/twvst3iids.cc, and it must be linked exactly once.
    set(TW_VST3_SDK_SOURCES
        ${TW_VST3_INC}/pluginterfaces/base/funknown.cpp
        ${TW_VST3_INC}/pluginterfaces/base/coreiids.cpp
        ${TW_VST3_INC}/pluginterfaces/base/ustring.cpp
        ${TW_VST3_INC}/pluginterfaces/base/conststringtable.cpp
    )
    # On macOS the SDK base sources (ustring.cpp, funknown.cpp) call into
    # CoreFoundation (CFStringCreateWithBytes, CFRelease). Every target that
    # COMPILES these sources — tw_plugins and the standalone twtestvst3 fixture —
    # must link the framework, or the arm64 link fails with undefined _CFRelease /
    # _CFStringCreateWithBytes. The Windows path uses Win32 instead, which is why
    # this only bites on macOS. Same find_library pattern as the AU/CoreAudio
    # blocks below.
    if(SMARAGD_MACOS)
        find_library(TW_VST3_COREFOUNDATION CoreFoundation)
    endif()
    # The project sets a global -Wall; the vendored SDK is not ours to keep clean,
    # so silence it per-source rather than lowering the bar for our own files.
    if(NOT MSVC)
        set_source_files_properties(${TW_VST3_SDK_SOURCES}
            PROPERTIES COMPILE_OPTIONS "-w")
    endif()

    target_sources(tw_plugins PRIVATE
        plugins/src/twvst3host.h
        plugins/src/twvst3host.cc
        plugins/src/twvst3module.h
        plugins/src/twvst3module.cc
        plugins/src/twvst3plugin.cc
        plugins/src/twvst3iids.cc
        ${TW_VST3_SDK_SOURCES}
    )
    target_include_directories(tw_plugins PRIVATE ${TW_VST3_INC})
    target_compile_definitions(tw_plugins PRIVATE TW_HAVE_VST3=1)
    if(SMARAGD_LINUX)
        target_link_libraries(tw_plugins PRIVATE dl)
    elseif(SMARAGD_MACOS)
        target_link_libraries(tw_plugins PRIVATE ${TW_VST3_COREFOUNDATION})
    endif()
    message(STATUS "tw_plugins: VST3 backend ENABLED (proposal 08 M6)")

    # De-risking spike, NOT shipped and NOT a test gate — the M6 ABI gate. Kept
    # after the backend landed because it is the fastest way to triage "this one
    # plugin will not load" without running the app. It links tw_plugins (which
    # now carries the SDK objects), so it must NOT compile them again.
    add_executable(vst3_probe plugins/tools/vst3_probe.cc)
    target_link_libraries(vst3_probe PRIVATE tw_plugins)
    target_include_directories(vst3_probe PRIVATE
        ${TW_VST3_INC}
        ${CMAKE_CURRENT_SOURCE_DIR}/plugins/src
    )
    set_target_properties(vst3_probe PROPERTIES AUTOMOC OFF)

    # Build-time test fixture: a real 2-in/2-out VST3 module built from this
    # repo, so plugins_test can exercise the actual VST3 load path without anyone
    # installing a third-party plugin. The counterpart of twtestclap, and C++
    # rather than C because VST3's ABI *is* a C++ vtable.
    #
    # It links its OWN copies of the SDK sources: a plugin and its host are
    # separate binaries and must not share IID definitions or FUnknown internals.
    add_library(twtestvst3 MODULE
        plugins/tests/twtestvst3.cpp
        ${TW_VST3_SDK_SOURCES}
    )
    target_include_directories(twtestvst3 PRIVATE ${TW_VST3_INC})
    set_target_properties(twtestvst3 PROPERTIES
        PREFIX ""
        SUFFIX ".vst3"
        AUTOMOC OFF
    )
    # It compiles the SDK base sources but does NOT link tw_plugins, so it needs
    # CoreFoundation directly on macOS (see the note by TW_VST3_SDK_SOURCES).
    if(SMARAGD_MACOS)
        target_link_libraries(twtestvst3 PRIVATE ${TW_VST3_COREFOUNDATION})
    endif()
    # ALSO drop it next to the app binary, for the same reason twtestclap is: a
    # qxa action script names the module with a RELATIVE path, and
    # SInsertPluginAction resolves that against the .qxa's directory and then
    # against QCoreApplication::applicationDirPath().
    add_custom_command(TARGET twtestvst3 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:twtestvst3>
                ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/twtestvst3.vst3
        COMMENT "Copying the twtestvst3 fixture next to the app binary (qxa cases)")
else()
    message(WARNING "tw_plugins: third_party/vst3_pluginterfaces not checked out; VST3 plugin hosting DISABLED. Run 'git submodule update --init --recursive' (or ./build.sh, which does it).")
endif()

# --- AudioUnit plugin backend (macOS only) ------------------------------------
# Proposal 08 M8. Unlike CLAP there is nothing to fetch: the AudioUnit /
# AudioToolbox / CoreFoundation headers ship in the macOS SDK, so discovery is
# simply "are we on macOS?". The backend is pure C CoreAudio (no Objective-C), so
# the sources are .cc. AU is discovered from the OS component registry rather than
# by walking directories (twaumodule.cc), so it needs no search-path plumbing.
#
# PRIVATE, exactly like CLAP: the AudioToolbox includes reach tw_plugins only, and
# the frameworks are PRIVATE link deps. Because tw_plugins is a STATIC library,
# CMake still propagates these PRIVATE link deps (as LINK_ONLY) to whatever links
# it — the app and smaragd_pluginprobe — so neither needs to name the frameworks.
option(ENABLE_AU "Host AudioUnit plugins (macOS)" ON)
set(_tw_have_au FALSE)
if(SMARAGD_MACOS AND ENABLE_AU)
    set(_tw_have_au TRUE)
    find_library(AU_AUDIOTOOLBOX   AudioToolbox)
    find_library(AU_AUDIOUNIT      AudioUnit)
    find_library(AU_COREAUDIO      CoreAudio)
    find_library(AU_COREFOUNDATION CoreFoundation)
    target_sources(tw_plugins PRIVATE
        plugins/src/twaumodule.h
        plugins/src/twaumodule.cc
        plugins/src/twauplugin.cc
    )
    target_compile_definitions(tw_plugins PRIVATE TW_HAVE_AU=1)
    target_link_libraries(tw_plugins PRIVATE
        ${AU_AUDIOTOOLBOX}
        ${AU_AUDIOUNIT}
        ${AU_COREAUDIO}
        ${AU_COREFOUNDATION}
    )
    message(STATUS "tw_plugins: AudioUnit backend ENABLED (proposal 08 M8)")
endif()

# --- Out-of-process scan probe (shipped) --------------------------------------
# The reason plugin scanning cannot take the app down (proposal 08 M2 /
# §Decisions 2): one module per run, JSON to stdout, so a plugin that crashes or
# hangs while being instantiated kills THIS process and becomes a cached
# failed/timeout record. Built whenever ANY probe-able backend is present. It
# links tw_plugins statically and calls the backends' descriptor enumerators
# directly, so its own TU must see the same TW_HAVE_* it dispatches on. Under
# plugins/tools/ so check_logging.py exempts its stdout (tools/ is an ALLOW_DIR).
if(_tw_have_clap OR _tw_have_vst3 OR _tw_have_au)
    add_executable(smaragd_pluginprobe plugins/tools/plugin_probe.cc)
    target_link_libraries(smaragd_pluginprobe PRIVATE tw_plugins)
    target_include_directories(smaragd_pluginprobe PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/plugins/src)
    set_target_properties(smaragd_pluginprobe PROPERTIES AUTOMOC OFF)
    if(_tw_have_clap)
        target_compile_definitions(smaragd_pluginprobe PRIVATE TW_HAVE_CLAP=1)
        target_include_directories(smaragd_pluginprobe PRIVATE ${TW_CLAP_DIR}/include)
    endif()
    if(_tw_have_vst3)
        target_compile_definitions(smaragd_pluginprobe PRIVATE TW_HAVE_VST3=1)
        target_include_directories(smaragd_pluginprobe PRIVATE ${TW_VST3_INC})
    endif()
    if(_tw_have_au)
        target_compile_definitions(smaragd_pluginprobe PRIVATE TW_HAVE_AU=1)
    endif()
endif()

# --- E8 devices: platform audio I/O (backends + inputs) ----------------------
tw_module(devices
    SOURCES
        devices/include/tw/devices/audio_backend.h
        devices/include/tw/devices/null_backend.h
        devices/include/tw/devices/capture_backend.h
        devices/include/tw/devices/audio_input.h
        devices/src/audio_backend.cc
        devices/src/null_backend.cc
        devices/src/capture_backend.cc
        devices/src/audio_input.cc
        devices/src/null_input.cc
        devices/src/null_input.h
    DEPS tw_core
)

# --- E9 sinks: audio destinations (file writers, sinks) -----------------------
tw_module(sinks
    SOURCES
        sinks/include/tw/sinks/audio_sink.h
        sinks/include/tw/sinks/file_sink.h
        sinks/include/tw/sinks/playback_sink.h
        sinks/include/tw/sinks/audio_file_writer.h
        sinks/src/audio_file_writer.cc
        sinks/src/file_sink.cc
        sinks/src/playback_sink.cc
        sinks/src/wav_writer.cc
        sinks/src/wav_writer.h
        sinks/src/ogg_writer.cc
        sinks/src/ogg_writer.h
        sinks/src/mp3_writer.cc
        sinks/src/mp3_writer.h
    DEPS tw_core
)

# --- E12 playback: speaker, engine pump, readahead ----------------------------
tw_module(playback
    SOURCES
        playback/include/tw/playback/twspeaker.h
        playback/include/tw/playback/audio_engine.h
        playback/include/tw/playback/audio_readahead.h
        playback/include/tw/playback/playback_context.h
        playback/src/twspeaker.cc
        playback/src/audio_engine.cc
        playback/src/audio_readahead.cc
    DEPS tw_core tw_pages tw_graph tw_devices tw_sources tw_schedule
)

# --- E10 render: the offline rendering engine ---------------------------------
tw_module(render
    SOURCES
        render/include/tw/render/render_session.h
        render/src/render_session.cc
    DEPS tw_core tw_pages tw_graph tw_sinks tw_playback tw_schedule
)

# --- E11 record: capture from input devices -----------------------------------
tw_module(record
    SOURCES
        record/include/tw/record/recording_session.h
        record/src/recording_session.cc
    DEPS tw_core tw_devices tw_sinks tw_sources
)

# --- E13 schedule: async revalidation ------------------------------------------
tw_module(schedule
    SOURCES
        schedule/include/tw/schedule/capture_revalidator.h
        schedule/include/tw/schedule/revalidatable.h
        schedule/include/tw/schedule/capture_aspects.h
        schedule/src/capture_revalidator.cc
    DEPS tw_core tw_pages tw_graph
)

# --- analysis: acoustic metrics for tests --------------------------------------
tw_module(analysis
    SOURCES
        analysis/include/tw/analysis/audio_analysis.h
        analysis/src/audio_analysis.cc
    DEPS tw_core
)

# --- metering: level meters (proposal 34) ---------------------------------------
# Reads levels out of frozen pages BY POSITION (a page frozen ahead of the
# playhead still describes the audio at the position it covers), plus the
# wall-clock ballistics the UI draws. No taps of its own: the app picks the
# component. Never freezes, never demands — see metering/CONTRACT.md.
tw_module(metering
    SOURCES
        metering/include/tw/metering/tw_level_scan.h
        metering/include/tw/metering/tw_meter_ballistics.h
        metering/include/tw/metering/tw_level_probe.h
        metering/src/tw_meter_ballistics.cc
        metering/src/tw_level_probe.cc
    DEPS tw_core tw_pages tw_graph
)

# --- events: MIDI/event model leaf (proposal 36 P0b) ---------------------------
# The one twEvent, the immutable event sequence, the tempo map (the ONLY
# tick<->frame converter), the SMF reader/writer, the automation curve, and the
# event clip set + merge the instrument slot and the MIDI-out pump read through.
# CORE ONLY, and no place in the dataflow DAG: events are model data, not pages
# (D1), and tw/plugins may not include tw/mix (F15) - which is why the clip set
# lives here and returns a core twFrameRange. See events/CONTRACT.md.
tw_module(events
    SOURCES
        events/include/tw/events/twevent.h
        events/include/tw/events/tweventseq.h
        events/include/tw/events/tweventsource.h
        events/include/tw/events/tweventclipset.h
        events/include/tw/events/tweventmerge.h
        events/include/tw/events/twtempomap.h
        events/include/tw/events/twsmf.h
        events/include/tw/events/twautomationcurve.h
        events/src/tweventseq.cc
        events/src/tweventclipset.cc
        events/src/tweventmerge.cc
        events/src/twtempomap.cc
        events/src/twsmf.cc
        events/src/twautomationcurve.cc
    DEPS tw_core
)

# --- sidecar: derived-data QAF container + LRU store (proposal 27) --------------
tw_module(sidecar
    SOURCES
        sidecar/include/tw/sidecar/twqaf.h
        sidecar/include/tw/sidecar/twsidecarstore.h
        sidecar/include/tw/sidecar/twaspects.h
        sidecar/include/tw/sidecar/twanalyzers.h
        sidecar/src/twqaf.cc
        sidecar/src/twsidecarstore.cc
        sidecar/src/twanalyzers.cc
    DEPS tw_core)

# --- Audio file format libraries (sinks only) -----------------------------------
# On Windows these come from vcpkg (CMake config packages); on macOS/Linux the
# system libraries ship .pc files, so pkg-config is the discovery path.
if(SMARAGD_WINDOWS)
    find_package(SndFile CONFIG REQUIRED)
    find_package(Ogg CONFIG REQUIRED)
    find_package(Vorbis CONFIG REQUIRED)
    target_link_libraries(tw_sinks PRIVATE
        SndFile::sndfile
        Ogg::ogg
        Vorbis::vorbis
        Vorbis::vorbisenc
    )
    target_link_libraries(tw_analysis PRIVATE SndFile::sndfile)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SNDFILE REQUIRED IMPORTED_TARGET sndfile)
    pkg_check_modules(OGG REQUIRED IMPORTED_TARGET ogg)
    pkg_check_modules(VORBIS REQUIRED IMPORTED_TARGET vorbis vorbisenc)
    target_link_libraries(tw_sinks PRIVATE
        PkgConfig::SNDFILE
        PkgConfig::OGG
        PkgConfig::VORBIS
    )
    target_link_libraries(tw_analysis PRIVATE PkgConfig::SNDFILE)
endif()
message(STATUS "tw_sinks: audio file I/O libraries found (libsndfile, libvorbis)")

# --- warp_ab: standalone A/B time-stretch quality harness (proposal 27 M3) -------
# NOT shipped. An offline developer tool that scores a candidate stretch/pitch
# backend (the paged vocoder) against the Rubber Band reference over a synthetic
# corpus. Self-contained DSP (own FFT/onset/envelope), double precision,
# deterministic. Lives under analysis/tools/ so check_logging exempts its metric
# table output (tools/ is an ALLOW_DIR). Links tw_analysis (WAV-metric reuse /
# cross-check) plus libsndfile directly for raw-sample loading and corpus
# writing.
add_executable(warp_ab analysis/tools/warp_ab.cc)
target_link_libraries(warp_ab PRIVATE tw_analysis)
if(SMARAGD_WINDOWS)
    target_link_libraries(warp_ab PRIVATE SndFile::sndfile)
else()
    target_link_libraries(warp_ab PRIVATE PkgConfig::SNDFILE)
endif()

# --- gen_position_fixture: writer + verifier for the position-coded fixture ----
# NOT shipped, and NOT a ctest gate — an offline tool, like warp_ab. It writes
# tests/test_position.wav (the "integer-cycle tone staircase" of
# tw/core/position_code.h) and, in --verify mode, decodes every block of an
# existing file back to its own position. That mode is why the fixture may be
# committed: unlike test_sawtooth.wav, which nobody can regenerate or check,
# this one is provable from source at any time.
add_executable(gen_position_fixture analysis/tools/gen_position_fixture.cc)
target_link_libraries(gen_position_fixture PRIVATE tw_analysis)
if(SMARAGD_WINDOWS)
    target_link_libraries(gen_position_fixture PRIVATE SndFile::sndfile)
else()
    target_link_libraries(gen_position_fixture PRIVATE PkgConfig::SNDFILE)
endif()

# --- Platform audio backends (devices only) --------------------------------------
if(SMARAGD_LINUX AND ENABLE_ALSA)
    find_package(ALSA REQUIRED)
    target_compile_definitions(tw_devices PRIVATE QBX_LINUX_ALSA=1 QBX_LINUX_ALSA_INPUT=1)
    target_link_libraries(tw_devices PRIVATE ALSA::ALSA)
    target_sources(tw_devices PRIVATE
        devices/src/alsa_backend.cc
        devices/include/tw/devices/alsa_backend.h
        devices/src/alsa_input.cc
        devices/src/alsa_input.h
    )
    message(STATUS "tw_devices: ALSA backend and input (recording) enabled")
endif()

if(SMARAGD_LINUX AND ENABLE_PIPEWIRE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
    target_compile_definitions(tw_devices PRIVATE QBX_LINUX_PIPEWIRE=1)
    target_link_libraries(tw_devices PRIVATE PkgConfig::PIPEWIRE)
    message(STATUS "tw_devices: PipeWire backend enabled (placeholder; implementation pending)")
endif()

if(SMARAGD_LINUX AND ENABLE_PULSEAUDIO)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PULSEAUDIO REQUIRED IMPORTED_TARGET libpulse libpulse-simple)
    target_compile_definitions(tw_devices PRIVATE QBX_LINUX_PULSEAUDIO=1)
    target_link_libraries(tw_devices PRIVATE PkgConfig::PULSEAUDIO)
    message(STATUS "tw_devices: PulseAudio backend enabled (placeholder; implementation pending)")
endif()

if(ENABLE_JACK)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(JACK REQUIRED IMPORTED_TARGET jack)
    target_compile_definitions(tw_devices PRIVATE QBX_JACK=1)
    target_link_libraries(tw_devices PRIVATE PkgConfig::JACK)
    message(STATUS "tw_devices: JACK backend enabled (placeholder; implementation pending)")
endif()

if(SMARAGD_WINDOWS AND ENABLE_WASAPI)
    target_compile_definitions(tw_devices PRIVATE QBX_WIN_WASAPI=1 QBX_WIN_WASAPI_INPUT=1)
    target_link_libraries(tw_devices PRIVATE ole32 oleaut32 mmdevapi avrt ksuser uuid winmm)
    target_sources(tw_devices PRIVATE
        devices/src/wasapi_backend.cc
        devices/include/tw/devices/wasapi_backend.h
        devices/src/wasapi_input.cc
        devices/src/wasapi_input.h
    )
    message(STATUS "tw_devices: WASAPI backend and input (recording) enabled")
endif()

if(SMARAGD_MACOS AND ENABLE_COREAUDIO)
    find_library(COREAUDIO_LIBRARY  CoreAudio)
    find_library(AUDIOUNIT_LIBRARY  AudioUnit)
    find_library(AUDIOTOOLBOX       AudioToolbox)
    find_library(COREFOUNDATION     CoreFoundation)
    find_library(AVFOUNDATION       AVFoundation)
    target_compile_definitions(tw_devices PRIVATE QBX_MAC_COREAUDIO=1 QBX_MAC_COREAUDIO_INPUT=1)
    target_link_libraries(tw_devices PRIVATE
        ${COREAUDIO_LIBRARY}
        ${AUDIOUNIT_LIBRARY}
        ${AUDIOTOOLBOX}
        ${COREFOUNDATION}
        ${AVFOUNDATION}
    )
    target_sources(tw_devices PRIVATE
        devices/src/coreaudio_backend.cc
        devices/include/tw/devices/coreaudio_backend.h
        devices/src/coreaudio_input.mm
        devices/src/coreaudio_input.h
    )
    message(STATUS "tw_devices: CoreAudio backend and input (recording) enabled")
endif()

if(NOT (ENABLE_ALSA OR ENABLE_PIPEWIRE OR ENABLE_PULSEAUDIO OR ENABLE_JACK OR ENABLE_WASAPI OR ENABLE_COREAUDIO))
    message(WARNING "tw303a: no audio backend enabled; the synthesizer will build but cannot output sound.")
endif()

# --- Umbrella target: the app links this until its own modularization phase ------
add_library(tw303a INTERFACE)
target_link_libraries(tw303a INTERFACE
    tw_core tw_pages tw_graph tw_sources tw_dsp tw_mix tw_plugins
    tw_devices tw_sinks tw_playback tw_render tw_record tw_schedule tw_analysis
    tw_sidecar tw_metering
)

# --- DSP optimization flags (proposal 28 W5) ----------------------------------
# The vocoder and the analyzers are the measured hot paths (warp_bench).
# -O3 enables auto-vectorization of the element-wise loops; without
# -ffast-math GCC will not reassociate FP reductions, so results stay
# bit-identical to -O2. -ffp-contract=off forbids FMA fusion so x86-64
# (baseline: no FMA) and aarch64 (FMA everywhere) produce the SAME bytes —
# the cross-platform determinism the render byte-cmp gates assume.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(tw_sources PRIVATE -O3 -ffp-contract=off)
    target_compile_options(tw_sidecar PRIVATE -O3 -ffp-contract=off)
endif()
# Legacy include strings ("twcomponent.h", "audio/audio_backend.h", …) resolve
# through forwarding headers; new code uses "tw/<module>/<header>.h" directly.
target_include_directories(tw303a INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/compat)

# --- Module tests (proposal 14, Phase 4) -------------------------------------
# Thin per-module executables: each links ONLY its module's subtree, so a
# test that stops linking is a layering regression in itself. Registered
# with CTest (see also the qxa suite registered in ../CMakeLists.txt).

function(tw_module_test NAME SRC)
    add_executable(${NAME} ${SRC})
    target_link_libraries(${NAME} PRIVATE ${ARGN})
    add_test(NAME ${NAME} COMMAND ${NAME})
endfunction()

tw_module_test(exact_arithmetic_test        core/tests/test_exact_arithmetic.cpp        tw_core)
tw_module_test(serialization_roundtrip_test core/tests/test_serialization_roundtrip.cpp tw_core)
tw_module_test(twfraction_test              core/tests/test_twfraction.cpp              tw_core)
tw_module_test(twwarpmap_test               core/tests/test_twwarpmap.cpp               tw_core)
tw_module_test(timemap_test                 core/tests/test_timemap.cpp                 tw_core)
tw_module_test(twlog_test                   core/tests/test_twlog.cpp                   tw_core)
tw_module_test(io_vector_test               pages/tests/io_vector_test.cc               tw_pages)
tw_module_test(metering_test                metering/tests/metering_test.cc             tw_metering)
tw_module_test(plugins_test                 plugins/tests/test_plugin_insert.cc         tw_plugins)
target_compile_definitions(plugins_test PRIVATE TEST_PLUGIN_INSERT_MAIN=1)
if(TARGET twtestclap)
    # The fixture's absolute path, resolved by the generator so it is correct for
    # whatever output layout the platform picks. Its presence is also what makes
    # the CLAP half of plugins_test compile at all — the test #ifdefs on it, so a
    # build without the submodule skips those checks instead of failing to build.
    add_dependencies(plugins_test twtestclap)
    target_compile_definitions(plugins_test PRIVATE
        TW_TESTCLAP_PATH="$<TARGET_FILE:twtestclap>")
endif()
# The M6 counterpart. Same shape: its presence is what makes the VST3 half of
# plugins_test compile at all, so a build without the submodule skips those
# checks instead of failing to build.
if(TARGET twtestvst3)
    add_dependencies(plugins_test twtestvst3)
    target_compile_definitions(plugins_test PRIVATE
        TW_TESTVST3_PATH="$<TARGET_FILE:twtestvst3>")
endif()
# The scanner/cache gate (proposal 08 M2): cache hit / miss / invalidate-on-mtime,
# the stickiness of a failed record, and the out-of-process probe. It needs
# plugins/src on its include path for the cache's private header, and both the
# fixture module and the probe executable built first.
tw_module_test(plugins_scan_test             plugins/tests/scan_test.cc                  tw_plugins)
target_include_directories(plugins_scan_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/plugins/src)
if(TARGET twtestclap)
    add_dependencies(plugins_scan_test twtestclap)
    target_compile_definitions(plugins_scan_test PRIVATE
        TW_TESTCLAP_PATH="$<TARGET_FILE:twtestclap>")
endif()
if(TARGET twtestvst3)
    add_dependencies(plugins_scan_test twtestvst3)
    target_compile_definitions(plugins_scan_test PRIVATE
        TW_TESTVST3_PATH="$<TARGET_FILE:twtestvst3>")
endif()
if(TARGET smaragd_pluginprobe)
    add_dependencies(plugins_scan_test smaragd_pluginprobe)
    target_compile_definitions(plugins_scan_test PRIVATE
        TW_PLUGINPROBE_PATH="$<TARGET_FILE:smaragd_pluginprobe>")
endif()
# The scan gate asserts exact module counts against a controlled fixture dir, so
# it must NOT also enumerate the machine's system AudioUnits (proposal 08 M8).
set_tests_properties(plugins_scan_test PROPERTIES ENVIRONMENT "SMARAGD_SCAN_AU=0")
# The AudioUnit backend gate (proposal 08 M8). Built only where the AU backend
# is; it skips at runtime when the machine registers no hostable AU, so it is
# safe on a headless mac. Needs plugins/src for the backend's private header.
if(_tw_have_au)
    tw_module_test(au_test plugins/tests/au_test.cc tw_plugins)
    target_include_directories(au_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/plugins/src)
    target_compile_definitions(au_test PRIVATE TW_HAVE_AU=1)
endif()
tw_module_test(sources_test                 sources/tests/sources_test.cc               tw_sources)
tw_module_test(vocoder_test                 sources/tests/vocoder_test.cc               tw_sources)
# NOT a gate: timing benchmark for the W5 SIMD work (run manually).
add_executable(warp_bench sources/tests/warp_bench.cc)
target_link_libraries(warp_bench PRIVATE tw_sources tw_sidecar)
tw_module_test(mix_test                     mix/tests/mix_test.cc                       tw_mix)
tw_module_test(schedule_test                schedule/tests/schedule_test.cc             tw_schedule tw_pages)
tw_module_test(events_test                  events/tests/events_test.cc                 tw_events)
# The committed SMF corpus (type 0 with running status, a multi-track type 1 with
# meta + sysex, a 30k-event file, and the writer-authored ones the byte-identity
# check needs) is read from the source tree, not copied - the test only reads.
target_compile_definitions(events_test PRIVATE
    TW_EVENTS_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/events/tests/fixtures")
tw_module_test(sidecar_test                 sidecar/tests/sidecar_test.cc               tw_sidecar)
tw_module_test(analyzers_test               sidecar/tests/analyzers_test.cc             tw_sidecar)
# tw_schedule + tw_pages are named EXPLICITLY (playback already depends on both,
# so this is a link-line fact, not a new module edge — check_layering.py's DAG is
# unchanged): the position-coded case constructs a CapturePagePool and a
# CaptureRevalidator and hands the latter to AudioEngine::setScheduler, which is
# the first time the demand scheduler is exercised from a unit test at all.
tw_module_test(playback_test                playback/tests/playback_test.cc             tw_playback tw_schedule tw_pages)
tw_module_test(render_test                  render/tests/render_test.cc                 tw_render)
