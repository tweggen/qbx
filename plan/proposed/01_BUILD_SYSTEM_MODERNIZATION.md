# Proposal: Build System Modernization

## Objective

Migrate from qmake-based builds to a modern, cross-platform build system supporting Linux, macOS, and Windows 11 with minimal friction for developers.

## Current State

- **qmake** (Qt5) used for builds
- Platform detection via preprocessor flags (`-DQBX_LINUX_ALSA=1`)
- Hardcoded platform-specific code paths in headers
- Makefile auto-generated from .pro file
- Tested only on Linux

## Proposed Solution: CMake

**Why CMake?**
- De-facto standard for C++ cross-platform builds
- Better third-party integration (ALSA, WASAPI, CoreAudio)
- Cleaner dependency management
- Works with IDEs: Xcode (macOS), Visual Studio (Windows), VS Code/Qt Creator (all)
- Can still integrate Qt5 tooling seamlessly

## Implementation Plan

### Phase 1: CMake Infrastructure

**1.1 Create CMakeLists.txt**

Top-level structure:
```cmake
cmake_minimum_required(VERSION 3.16)
project(smaragd VERSION 1.0.0 LANGUAGES CXX C)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Qt integration
find_package(Qt5 COMPONENTS Widgets Xml Core REQUIRED)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# Platform detection
if(APPLE)
    message(STATUS "Building for macOS")
    set(SMARAGD_MACOS ON)
elseif(WIN32)
    message(STATUS "Building for Windows")
    set(SMARAGD_WINDOWS ON)
elseif(UNIX AND NOT APPLE)
    message(STATUS "Building for Linux")
    set(SMARAGD_LINUX ON)
endif()

# Feature flags
option(ENABLE_ALSA "Enable ALSA audio (Linux)" ON)
option(ENABLE_PIPEWIRE "Enable PipeWire audio (Linux)" OFF)  # Future
option(ENABLE_PULSEAUDIO "Enable PulseAudio (Linux)" OFF)    # Future
option(ENABLE_JACK "Enable JACK audio" OFF)                  # Future
option(ENABLE_WASAPI "Enable WASAPI (Windows)" ON)
option(ENABLE_COREAUDIO "Enable CoreAudio (macOS)" ON)

# Add subdirectories
add_subdirectory(tw303a)
add_subdirectory(main)

# Global include directories
include_directories(include)
```

**1.2 tw303a/CMakeLists.txt**

```cmake
set(TW303A_SOURCES
    src/tw303aenv.cc
    src/twconstant.cc
    src/twsaw.cc
    src/twlatch.cc
    src/twgrainer.cc
    src/twmoog.cc
    src/twspeaker.cc
    src/twtestseq.cc
    src/twrewire.cc
    src/twwav.cc
    src/twmixer.cc
    src/twtrackmix.cc
    src/twwavinput.cc
    src/twpipe.cc
    src/twcomponent.cc
    src/twgrainspec.cc
    src/twsimplesaw.cc
    src/twstreaminglatch.cc
    src/twwhitenoise.cc
    src/tw303a.cc
    src/twosc.cc
)

set(TW303A_HEADERS
    include/twcomponent.h
    include/twspeaker.h
    # ... (all headers)
)

add_library(tw303a STATIC ${TW303A_SOURCES} ${TW303A_HEADERS})
target_include_directories(tw303a PUBLIC include)

# Platform-specific audio backend selection
if(SMARAGD_LINUX AND ENABLE_ALSA)
    target_compile_definitions(tw303a PRIVATE QBX_LINUX_ALSA=1)
    target_link_libraries(tw303a PRIVATE asound)
    target_sources(tw303a PRIVATE src/audio/alsa_backend.cc)
endif()

# (Windows, macOS, PipeWire, etc. handled similarly)
```

**1.3 main/CMakeLists.txt**

```cmake
set(SMARAGD_SOURCES
    src/main.cpp
    src/sapplication.cpp
    # ... (all source files)
)

add_executable(smaragd ${SMARAGD_SOURCES})
target_link_libraries(smaragd PRIVATE tw303a Qt5::Widgets Qt5::Xml Qt5::Core)
target_include_directories(smaragd PRIVATE include)
```

### Phase 2: Platform-Specific Abstractions

**2.1 Create Audio Abstraction Layer**

Instead of platform ifdefs scattered in twspeaker.cc, create clean interfaces:

```
tw303a/include/audio/
├── audio_backend.h         # Interface
├── alsa_backend.h          # Linux ALSA
├── wasapi_backend.h        # Windows WASAPI
├── coreaudio_backend.h     # macOS CoreAudio
└── pipewire_backend.h      # Future: Linux PipeWire

tw303a/src/audio/
├── alsa_backend.cc
├── wasapi_backend.cc
├── coreaudio_backend.cc
└── pipewire_backend.cc
```

**2.2 Abstract Audio Interface** (twspeaker.h refactor)

```cpp
// audio/audio_backend.h
class AudioBackend {
public:
    virtual ~AudioBackend() = default;
    virtual int openDevice(const std::string& deviceName = "default") = 0;
    virtual int closeDevice() = 0;
    virtual int startOutput() = 0;
    virtual int stopOutput() = 0;
    virtual bool isPlaying() const = 0;
    virtual int writeAudio(const float* samples, size_t frames) = 0;
};

// Platform implementations chosen at compile-time or runtime
std::unique_ptr<AudioBackend> createAudioBackend();
```

**2.3 twspeaker.cc Refactor**

Replace 470 lines of #ifdef chaos with delegation to backend:

```cpp
// Before: ~100 lines of #ifdef for each platform
// After: 
twSpeaker::twSpeaker(tw303aEnvironment &env) 
    : twComponent(env), backend_(createAudioBackend()) {}

void twSpeaker::startOutput() {
    backend_->startOutput();  // Delegates to ALSA, WASAPI, CoreAudio, etc.
}
```

### Phase 3: Build Variants

**3.1 Linux Build**

```bash
# ALSA only (current)
cmake -B build -DENABLE_ALSA=ON
cmake --build build

# PipeWire (future)
cmake -B build -DENABLE_ALSA=OFF -DENABLE_PIPEWIRE=ON
cmake --build build

# Multiple backends at once (requires runtime selection)
cmake -B build -DENABLE_ALSA=ON -DENABLE_PIPEWIRE=ON -DENABLE_JACK=ON
cmake --build build
```

**3.2 macOS Build**

```bash
cmake -B build-macos -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DENABLE_COREAUDIO=ON
cmake --build build-macos
# Output: build-macos/smaragd (universal binary, arm64 + x86_64)
```

**3.3 Windows Build**

```bash
# MSVC (Visual Studio 2022)
cmake -B build-win -G "Visual Studio 17 2022"
cmake --build build-win --config Release

# Or MinGW
cmake -B build-mingw -G "MinGW Makefiles"
cmake --build build-mingw
```

### Phase 4: Dependency Management

**4.1 Find/Provide Dependencies**

CMake's built-in `find_package()` for system libraries:

```cmake
# ALSA (Linux)
find_package(ALSA)
if(ALSA_FOUND)
    target_link_libraries(tw303a PRIVATE ALSA::ALSA)
endif()

# CoreAudio (macOS, built-in)
if(APPLE)
    find_library(COREAUDIO_LIBRARY CoreAudio)
    target_link_libraries(tw303a PRIVATE ${COREAUDIO_LIBRARY})
endif()

# WASAPI (Windows, built-in)
if(WIN32)
    target_link_libraries(tw303a PRIVATE ole32 oleaut32)
endif()
```

**4.2 Optional: Conan/vcpkg for dependency management**

For more complex setups (JACK, PipeWire, custom Qt builds), consider:
- **Conan**: Universal C++ package manager
- **vcpkg**: Microsoft's package manager (good for Windows)

### Phase 5: IDE Integration

**5.1 Qt Creator**
- Automatically detects CMake projects
- Builds work out-of-box

**5.2 Xcode (macOS)**
```bash
cmake -B build -G Xcode
open build/smaragd.xcodeproj
```

**5.3 Visual Studio (Windows)**
```bash
cmake -B build -G "Visual Studio 17 2022"
start build/smaragd.sln
```

**5.4 VS Code**
- CMake Tools extension handles builds
- .vscode/settings.json with CMake configuration

## Deliverables

| Item | Owner | Timeline |
|------|-------|----------|
| Top-level CMakeLists.txt | Dev | Week 1 |
| tw303a/CMakeLists.txt | Dev | Week 1 |
| main/CMakeLists.txt | Dev | Week 1 |
| Audio abstraction layer | Dev | Week 2-3 |
| ALSA backend refactor | Dev | Week 2 |
| macOS CoreAudio backend | Dev | Week 3-4 |
| Windows WASAPI backend | Dev | Week 4-5 |
| Documentation | Dev | Week 5 |
| GitHub Actions CI/CD | DevOps | Week 5-6 |

## Testing Strategy

1. **Linux**: Build with ALSA, JACK (if available), PipeWire (future)
2. **macOS**: Build universal binaries (arm64 + x86_64), test on M1+ and Intel
3. **Windows**: Build with MSVC and MinGW, test on Windows 11
4. **Cross-compilation**: ARM Linux (Raspberry Pi), etc.

## Migration Path

1. **Phase 1 & 2**: Keep qmake alongside CMake during transition
2. **Phase 3-4**: CMake becomes primary build system
3. **Final**: Remove qmake once all platforms verified

## Risks & Mitigation

| Risk | Mitigation |
|------|-----------|
| CMake complexity for contributors | Provide pre-built Docker images, detailed docs |
| macOS backend changes | Start with backward-compat CoreAudio, upgrade progressively |
| Windows dependency issues | Use vcpkg or Conan for automated dependency resolution |
| CI/CD setup | GitHub Actions templates included in repo |

## References

- CMake Best Practices: https://cliutils.gitlab.io/modern-cmake/
- Qt5 CMake Integration: https://doc.qt.io/qt-5/cmake-manual.html
- ALSA/PipeWire/WASAPI docs: See audio strategy document
