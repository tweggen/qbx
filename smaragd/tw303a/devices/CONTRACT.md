# tw/devices — CONTRACT

Purpose: platform audio I/O. AudioBackend (callback-pull output) and
AudioInput (capture) interfaces plus the WASAPI/ALSA/CoreAudio/Null
implementations.

Public headers: audio_backend.h, audio_input.h, null_backend.h, and the
per-platform backend headers. Platform *_input.h headers are PRIVATE (src/).

Depends on: tw/core. Platform SDKs (ole32/avrt/ALSA/CoreAudio) are PRIVATE
link deps; QBX_* backend defines are PRIVATE compile definitions. Forbidden:
tw/graph and above — a backend moves buffers, it does not know components.

Invariants:
1. The render callback runs on the BACKEND'S thread: THREADING.md rule 1
   applies to everything reachable from it.
2. openDevice/startOutput/stopOutput/closeDevice are blocking control-plane
   calls (UI/worker thread), never called from the callback.
3. stopOutput() blocks until the callback thread has exited — callers rely
   on this for teardown ordering.
4. Device ids are backend-native strings; "default"/empty = system default.
5. createAudioBackend()/createAudioInput() choose the platform impl via the
   QBX_* defines; adding a backend touches this module + CMake only.

How to test: WASAPI is the only regularly exercised backend (manual GUI
playback); Null backend keeps headless/CI paths honest.

Known debt: WASAPI shared-mode only; ALSA untested since the refactor;
PipeWire/Pulse/JACK placeholders; input enumeration shows only defaults.
