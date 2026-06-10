set(TW303A_HEADERS
    include/tw303aenv.h
    include/twcomponent.h
    include/twformat.h
    include/twconvert.h
    include/twnegotiator.h
    include/twresampler.h
    include/twconstant.h
    include/twmixer.h
    include/twmoog.h
    include/twosc.h
    include/twpipe.h
    include/twrandomsource.h
    include/twsamplesource.h
    include/twsamplereader.h
    include/twloopreader.h
    include/twresampledsource.h
    include/twcapturingsource.h
    include/twgrainparams.h
    include/twgrainsource.h
    include/twrewire.h
    include/twsaw.h
    include/twsimplesaw.h
    include/twspeaker.h
    include/twtestseq.h
    include/twtrackmix.h
    include/twwav.h
    include/twwavinput.h
    include/twwhitenoise.h
    include/audio/audio_backend.h
    include/audio/null_backend.h
    include/audio/audio_file_writer.h
)

set(TW303A_SOURCES
    src/tw303a.cc
    src/tw303aenv.cc
    src/twcomponent.cc
    src/twconstant.cc
    src/twconvert.cc
    src/twnegotiator.cc
    src/twlatch.cc
    src/twmixer.cc
    src/twmoog.cc
    src/twosc.cc
    src/twpipe.cc
    src/twsamplesource.cc
    src/twsamplereader.cc
    src/twloopreader.cc
    src/twresampledsource.cc
    src/twcapturingsource.cc
    src/twgrainsource.cc
    src/twresampler.cc
    src/twrewire.cc
    src/twsaw.cc
    src/twsimplesaw.cc
    src/twspeaker.cc
    src/twstreaminglatch.cc
    src/twtestseq.cc
    src/twtrackmix.cc
    src/twwav.cc
    src/twwavinput.cc
    src/twwhitenoise.cc
    src/audio/audio_backend.cc
    src/audio/null_backend.cc
    src/audio/audio_file_writer.cc
    src/audio/wav_writer.cc
    src/audio/wav_writer.h
    src/audio/ogg_writer.cc
    src/audio/ogg_writer.h
)

add_library(tw303a STATIC ${TW303A_SOURCES} ${TW303A_HEADERS})

target_include_directories(tw303a
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/main/include
)

target_link_libraries(tw303a PUBLIC
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::Xml
)

# Required audio file format libraries
find_package(SndFile REQUIRED)
find_package(Vorbis REQUIRED)

target_link_libraries(tw303a PRIVATE
    SndFile::SndFile
    Vorbis::Vorbis
    Vorbis::VorbisEnc
)

message(STATUS "tw303a: audio file I/O libraries found (libsndfile, libvorbis)")

if(SMARAGD_LINUX AND ENABLE_ALSA)
    find_package(ALSA REQUIRED)
    target_compile_definitions(tw303a PRIVATE QBX_LINUX_ALSA=1)
    target_link_libraries(tw303a PRIVATE ALSA::ALSA)
    target_sources(tw303a
        PRIVATE
            src/audio/alsa_backend.cc
            include/audio/alsa_backend.h
    )
    message(STATUS "tw303a: ALSA backend enabled")
endif()

if(SMARAGD_LINUX AND ENABLE_PIPEWIRE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
    target_compile_definitions(tw303a PRIVATE QBX_LINUX_PIPEWIRE=1)
    target_link_libraries(tw303a PRIVATE PkgConfig::PIPEWIRE)
    message(STATUS "tw303a: PipeWire backend enabled (placeholder; implementation pending)")
endif()

if(SMARAGD_LINUX AND ENABLE_PULSEAUDIO)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PULSEAUDIO REQUIRED IMPORTED_TARGET libpulse libpulse-simple)
    target_compile_definitions(tw303a PRIVATE QBX_LINUX_PULSEAUDIO=1)
    target_link_libraries(tw303a PRIVATE PkgConfig::PULSEAUDIO)
    message(STATUS "tw303a: PulseAudio backend enabled (placeholder; implementation pending)")
endif()

if(ENABLE_JACK)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(JACK REQUIRED IMPORTED_TARGET jack)
    target_compile_definitions(tw303a PRIVATE QBX_JACK=1)
    target_link_libraries(tw303a PRIVATE PkgConfig::JACK)
    message(STATUS "tw303a: JACK backend enabled (placeholder; implementation pending)")
endif()

if(SMARAGD_WINDOWS AND ENABLE_WASAPI)
    target_compile_definitions(tw303a PRIVATE QBX_WIN_WASAPI=1)
    target_link_libraries(tw303a PRIVATE ole32 oleaut32 mmdevapi avrt ksuser uuid winmm)
    target_sources(tw303a
        PRIVATE
            src/audio/wasapi_backend.cc
            include/audio/wasapi_backend.h
    )
    message(STATUS "tw303a: WASAPI backend enabled")
endif()

if(SMARAGD_MACOS AND ENABLE_COREAUDIO)
    find_library(COREAUDIO_LIBRARY  CoreAudio)
    find_library(AUDIOUNIT_LIBRARY  AudioUnit)
    find_library(AUDIOTOOLBOX       AudioToolbox)
    find_library(COREFOUNDATION     CoreFoundation)
    find_library(AVFOUNDATION       AVFoundation)
    target_compile_definitions(tw303a PRIVATE QBX_MAC_COREAUDIO=1)
    target_link_libraries(tw303a PRIVATE
        ${COREAUDIO_LIBRARY}
        ${AUDIOUNIT_LIBRARY}
        ${AUDIOTOOLBOX}
        ${COREFOUNDATION}
        ${AVFOUNDATION}
    )
    target_sources(tw303a
        PRIVATE
            src/audio/coreaudio_backend.cc
            include/audio/coreaudio_backend.h
    )
    message(STATUS "tw303a: CoreAudio backend enabled")
endif()

if(NOT (ENABLE_ALSA OR ENABLE_PIPEWIRE OR ENABLE_PULSEAUDIO OR ENABLE_JACK OR ENABLE_WASAPI OR ENABLE_COREAUDIO))
    message(WARNING "tw303a: no audio backend enabled; the synthesizer will build but cannot output sound.")
endif()
