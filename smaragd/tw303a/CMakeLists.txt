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
        core/include/tw/core/exc.h
        core/include/tw/core/twsyslog.h
        core/include/tw/core/twlog.h
        core/include/tw/core/audio_frame.h
        core/include/tw/core/generation_promise.h
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
        sources/include/tw/sources/twwavinput.h
        sources/include/tw/sources/twwav.h
        sources/src/twsamplesource.cc
        sources/src/twsamplereader.cc
        sources/src/twresampledsource.cc
        sources/src/twresampler.cc
        sources/src/twcapturingsource.cc
        sources/src/twloopreader.cc
        sources/src/twgrainsource.cc
        sources/src/twwavinput.cc
        sources/src/twwav.cc
    DEPS tw_core tw_pages tw_graph
)

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
        plugins/src/twpluginchain.cc
        plugins/src/twplugininsert.cc
        plugins/src/twpassthrough.cc
        plugins/src/twpluginregistry.cc
    DEPS tw_core tw_graph
)

# --- E8 devices: platform audio I/O (backends + inputs) ----------------------
tw_module(devices
    SOURCES
        devices/include/tw/devices/audio_backend.h
        devices/include/tw/devices/null_backend.h
        devices/include/tw/devices/audio_input.h
        devices/src/audio_backend.cc
        devices/src/null_backend.cc
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
)
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
tw_module_test(timemap_test                 core/tests/test_timemap.cpp                 tw_core)
tw_module_test(twlog_test                   core/tests/test_twlog.cpp                   tw_core)
tw_module_test(io_vector_test               pages/tests/io_vector_test.cc               tw_pages)
tw_module_test(plugins_test                 plugins/tests/test_plugin_insert.cc         tw_plugins)
target_compile_definitions(plugins_test PRIVATE TEST_PLUGIN_INSERT_MAIN=1)
tw_module_test(sources_test                 sources/tests/sources_test.cc               tw_sources)
tw_module_test(mix_test                     mix/tests/mix_test.cc                       tw_mix)
tw_module_test(schedule_test                schedule/tests/schedule_test.cc             tw_schedule tw_pages)
tw_module_test(playback_test                playback/tests/playback_test.cc             tw_playback)
tw_module_test(render_test                  render/tests/render_test.cc                 tw_render)
