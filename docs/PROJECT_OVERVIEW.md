# Smaragd Audio Synthesizer - Project Overview

## Executive Summary

**Smaragd** is a Qt-based audio synthesis application featuring a software TB-303 synthesizer clone with granular synthesis capabilities. The codebase (~7,200 lines of C++) currently supports Linux with ALSA, legacy macOS CoreAudio, and legacy Linux OSS, but lacks modern audio driver support for current macOS, Windows, and modern Linux alternatives (PipeWire, PulseAudio, JACK).

## Project Structure

```
smaragd/
├── tw303a/                    # TB-303 synthesizer synthesizer components
│   ├── include/               # Header files for synthesizer modules
│   │   ├── twosc.h           # Oscillators
│   │   ├── twmoog.h          # Moog filter
│   │   ├── twgrainer.h       # Granular synthesis engine
│   │   ├── twspeaker.h       # Audio output handler (KEY FILE)
│   │   └── ...
│   └── src/                   # Synthesizer implementation
├── main/                      # Main application (Qt UI, project management)
│   ├── include/               # Application headers
│   └── src/                   # Application implementation
├── include/                   # Shared utilities
├── doc/                       # Documentation (images, notes)
├── CMakeLists.txt            # Top-level CMake project
└── .gitignore

main/
├── include/
│   ├── sproject.h            # Project/session management
│   ├── sstdmixer.h          # Standard mixer
│   ├── strack.h             # Audio tracks
│   ├── smainwindow.h        # Main UI window
│   └── ...
└── src/
```

## Current Audio Driver Architecture

### Supported Platforms

| Platform | Driver | Status | Notes |
|----------|--------|--------|-------|
| Linux | ALSA | ✅ Active | Primary implementation, asynchronous PCM handler |
| Linux | OSS | ⚠️ Legacy | Deprecated code path, socket notifier-based |
| macOS | CoreAudio | ❌ Broken | OS X 10.2 API (deprecated 2005+), uses removed APIs |
| Windows | — | ❌ None | Not implemented |

### Current Implementation Details

**Audio Output Flow:**
1. Application generates floating-point audio samples (tw303a synthesizer)
2. `twSpeaker::calcOutputTo()` or streaming callbacks provide samples
3. Samples converted from float to 16-bit signed PCM (S16_LE)
4. Output to ALSA default device or OSS `/dev/dsp`
5. Hard-coded: 44.1 kHz, mono synthesizer → stereo output

**ALSA Implementation (twspeaker.cc:244-303):**
- Opens "default" device in playback mode
- Hardcoded: 44100 Hz, 2 channels, S16_LE format
- Buffer size: 1024 frames, period size: 64 frames
- Async callback handler (`alsaPcmHandlerStatic_`) fills buffer on demand
- No device enumeration, no format negotiation

**macOS CoreAudio (deprecated):**
- Uses pre-10.5 APIs: `OpenAComponent()`, `FindNextComponent()`
- AudioUnit callback model (still valid pattern, but APIs removed)
- 44.1 kHz, float format, single callback for rendering

**OSS Legacy Code (unused):**
- `/dev/dsp` socket notifier approach
- Pre-ALSA era Linux support

### Known Issues & Limitations

1. **Platform-specific compilation**: Requires compiler flags like `-DQBX_LINUX_ALSA=1`
2. **No sample rate flexibility**: Hardcoded 44.1 kHz only
3. **No audio device selection**: Always uses "default" device
4. **macOS support broken**: CoreAudio API deprecated, app won't compile/run on modern macOS
5. **Windows completely missing**: Critical gap for cross-platform audio application
6. **No PulseAudio/PipeWire**: Linux developers using modern sound stacks are blocked
7. **No JACK support**: Professional audio users cannot use this synthesizer
8. **Buffer management primitive**: Fixed sizes, no dynamic adaptation
9. **No latency optimization**: ~1024-sample buffer introduces ~23ms latency at 44.1 kHz

## Codebase Statistics

- **Total Lines**: ~7,200 (C++, C)
- **Synthesizer (tw303a/)**: ~3,500 LOC (oscillators, filters, effects, granular synthesis)
- **Main App (main/)**: ~3,000 LOC (UI, project management, rendering)
- **Audio Output (twspeaker)**: ~470 LOC (ALSA/OSS/CoreAudio logic)

## Key Dependencies

- **Qt5**: UI framework (widgets, XML, core)
- **ALSA**: Linux sound driver (libasound)
- **pthreads**: Threading
- **X11/OpenGL**: Linux display (desktop)
- **CoreAudio/AudioUnit**: macOS (broken, needs update)
- **Windows APIs**: Not integrated

## Modernization Goals

### Build System
- Support CMake for cross-platform builds
- Conditional compilation for macOS (Xcode), Windows (MSVC/MinGW), Linux (GCC/Clang)
- Modern Qt5 integration

### Audio Drivers
- **macOS**: AVAudioEngine or modern CoreAudio APIs
- **Windows**: WASAPI (modern standard)
- **Linux**: ALSA (keep), add PipeWire and JACK support
- **Cross-platform**: Dynamic format/sample-rate negotiation

### Quality-of-Life Improvements
- Device enumeration and selection UI
- Configurable buffer sizes/latency
- Error recovery and fallback mechanisms
- Cross-compilation support

## Next Steps

See `plan/proposed/` for detailed modernization strategies.
