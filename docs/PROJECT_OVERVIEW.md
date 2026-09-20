# Smaragd — Project Overview

**Smaragd** is a Qt 6 / C++17 digital audio workstation. The audio engine is a
static library tree under `smaragd/tw303a/`; the application — the document
model, the arrangement UI, the mixer, plugin hosting and the scripting runtime
— is under `smaragd/main/`. It builds with CMake and runs on Windows (WASAPI,
ASIO), Linux (ALSA) and macOS (CoreAudio).

> **This page is an orientation, not a record.** For the module map read
> [`docs/ARCHITECTURE.md`](ARCHITECTURE.md); for what has been implemented and
> when, [`plan/STATE.md`](../plan/STATE.md); for designs,
> `plan/proposed/`. Where this page and any of those disagree, they win. Facts
> that rot — line counts, dependency versions, what is "next" — are
> deliberately not kept here.

## What is where

| | |
|---|---|
| `smaragd/tw303a/` | the engine: one `tw_<module>` static library per directory, a build-enforced DAG |
| `smaragd/main/` | the app: 17 modules in four layered OBJECT libraries, `app_model < app_core < app_objects < app_ui` |
| `smaragd/tests/` | the `.qxa` case suite, fixtures and goldens |
| `plan/` | proposals (`proposed/`), the implementation record (`STATE.md`) |
| `docs/contracts/` | the cross-module protocols — read before touching an audio path |
| `docs/archive/` | superseded design notes, each with a banner |

Every module has a `CONTRACT.md` beside its sources, and that contract — not
this page — is the authority on the module's invariants.

## Audio architecture

### The device boundary

Platform `#ifdef` sprawl was replaced by a single interface,
`tw/devices/audio_backend.h`, with one concrete implementation per backend:

- **Callback-pull model** — the backend owns the timing and calls a
  `RenderCallback` to pull frames from the engine.
- **`AudioConfig`** reports the rate, channel count, buffer and period sizes,
  and the device's native binary `sampleType` (float32 / int16 / int32).
- **Rate negotiation** — `supportedRates()` advertises what the device can open
  without host resampling, `openDevice(device, preferredRate)` asks for one, and
  `getConfig()` reports what was actually opened.
- **Device enumeration** — `enumerateDevices()` returns `{id, name}` endpoints
  for the picker.
- `createAudioBackend()` selects the backend compiled in for the platform, and
  an always-available `NullBackend` lets the app run silently when no real
  backend is enabled — which is what makes a headless test run possible.

Backends in the tree, with their CMake switches
(`ENABLE_ALSA`, `ENABLE_WASAPI`, `ENABLE_COREAUDIO` and `ENABLE_ASIO` default ON
on their own platform):

| Platform | Backends |
|---|---|
| Windows | WASAPI (shared mode) and **ASIO**, dispatched by `WinMultiBackend`; input on both |
| Linux | ALSA, output and input; the ALSA sequencer also carries MIDI |
| macOS | CoreAudio output (AudioUnit), input via an `AVAudioEngine` tap |
| any | `NullBackend` / `NullInput`, and `FileAudioInput` for scripted capture |
| — | PipeWire, PulseAudio and JACK have CMake switches (`OFF`) and **no implementation** — the wiring is not evidence that they work |

ASIO's Windows gate run is recorded in
[`docs/ASIO_WINDOWS_GATE.md`](ASIO_WINDOWS_GATE.md), including the driver facts
it bought — most usefully that ASIO latency is rate-dependent and must be read
after `setSampleRate` rather than cached from open.

### Sample format and rate

Data format is a property of every wire, not an engine-wide constant.

1. **`twFormat`** (rate, binary sample type, channels, layout) is attached to
   each producing latch and queried by its consumer. **This is the WIRE format
   and says nothing about page width**: since proposal 36 a frozen
   `twOutputPage` carries `SProject::channels()` planar channels, and a plug
   pull takes channel `min(latchIndex, page->channels()-1)`. Nothing negotiates
   channels — `twFormatCaps` carries rate and sample type only, and
   `twComponent::getOutputChannels()` is the sole authority for width.
2. **Per-project sample rate.** `SProject` stores a rate and a candidate-rate
   set, both persisted in the project XML and pushed into the engine via
   `tw303aEnvironment::setSRate`. A fresh project defaults to **48 kHz**;
   a legacy file with no rate attribute loads as **44.1 kHz**, so an old project
   is not silently reinterpreted.
3. **Rate-aware engine.** Oscillators, the delay line, the Moog filter, tempo
   math and the WAV writer all derive their constants from `env.getSRate()`.
4. **Resampling at the device boundary.** `twSpeaker` holds a `twResampler`
   that converts the graph rate to the rate the device actually opened at, and
   is a passthrough when they match. This is what removed the long-standing
   ~8.8 % pitch error on 48 kHz devices. The resampler is **linear** — adequate
   to fix pitch and speed, not mastering-grade.
5. **Format conversion.** A shared `twConvertFrames` handles type and channel
   conversion at the device boundary and in the WAV writer.
6. **Negotiation is advisory.** `twNegotiator` resolves a single rate per wire
   across the graph (an arc-consistency fixpoint over a finite candidate-rate
   domain) and runs before playback, but the speaker's resampler guarantees
   correct output regardless. Do not treat a negotiated rate as binding; live
   insertion of in-graph resampler nodes is deferred.

## Configuration

`SSettings` is a `QSettings`-based per-user INI
(`%APPDATA%/Smaragd/smaragd.ini`, `~/.config/Smaragd/smaragd.ini`) holding what
does not belong in a project file: the selected devices, window layout, plugin
search paths and the last-used directories. Secrets never go in it — they go
through `SSecretStore` (DPAPI, Keychain, libsecret, or none), and a stored
credential is never re-displayed.

## Known limitations

These are the ones that shape decisions rather than the ones that shape a
backlog; `plan/STATE.md` and the proposals carry the rest.

1. **No CI.** There is no GitHub Actions workflow. The local gates listed in
   [`CLAUDE.md`](../CLAUDE.md) are the entire safety net.
2. **Linux is under-tested.** The ALSA backend has not been exercised since the
   module refactor, and every timing figure quoted in `CLAUDE.md` and
   `plan/STATE.md` was measured on the Windows box.
3. **macOS Keychain has never been compiled.** The backend is written; the
   development box is Windows/MinGW. Expect build errors before behaviour.
4. **WASAPI is shared mode only** — no exclusive, bit-perfect path. A device
   whose mix rate differs from the project is bridged by the resampler rather
   than opened natively. ASIO is the answer to that, and to the split-clock
   capture-rate failure class: one driver, one clock, in and out matched.
5. **No plugin delay compensation.** Out of scope as of proposal 37 P9.

## Build

See [`docs/BUILD.md`](BUILD.md). From the repository root:

```bash
./build.sh   [QT_PATH]   # incremental; configures if smaragd/build/ is missing
./rebuild.sh [QT_PATH]   # clean
```

`QT_PATH` is the Qt prefix; omit it to auto-detect.
