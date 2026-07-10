cmake_minimum_required(VERSION 3.16)
project(smaragd VERSION 1.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# Ensure Debug builds include full source line information for LLDB/GDB
if(NOT MSVC)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug" OR NOT CMAKE_BUILD_TYPE)
        # GCC/Clang: -g3 = full debug info with macros, -O0 = no optimization
        # Set CMAKE_CXX_FLAGS directly (not _DEBUG variant) for single-config generators
        string(APPEND CMAKE_CXX_FLAGS " -g3 -O0 -fno-omit-frame-pointer")
        string(APPEND CMAKE_C_FLAGS " -g3 -O0 -fno-omit-frame-pointer")
        message(STATUS "Debug build: full source line info enabled (-g3 -O0 -fno-omit-frame-pointer)")
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

add_subdirectory(tw303a)
add_subdirectory(main)

message(STATUS "smaragd: ALSA=${ENABLE_ALSA} PipeWire=${ENABLE_PIPEWIRE} "
               "PulseAudio=${ENABLE_PULSEAUDIO} JACK=${ENABLE_JACK} "
               "WASAPI=${ENABLE_WASAPI} CoreAudio=${ENABLE_COREAUDIO}")
