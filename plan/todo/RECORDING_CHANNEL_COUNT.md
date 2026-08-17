# The recorder writes the INPUT DEVICE's channel count (found 2026-08-17)

> **Status: OPEN.** Diagnosed from a real user project, not from reading code.
> The fix is deliberately not attempted here — it needs a product decision (see
> §4). Written as a handoff so whoever picks it up starts from evidence.

## 1. The symptom, as reported

*"I hear output on the left channel only with an arrangement containing
mono-material only. VU meters show one channel only."*

On a machine where the device is 48 kHz / 2 ch / float32 and the project is
48 kHz — so no resampling, no format conversion, no channel fan-out arithmetic.
The monitor rule resolved correctly (`monitoring a 2-channel project (L=ch0,
R=ch1)`), and the meters were reporting **two lanes with the right one dead**.

## 2. The cause — it is the FILES, and nothing in the engine

Every recording in the project is a **16-channel** file with the signal in
channel 0 and a noise floor in channels 1–15:

```
20260712_164250_756_(untitled).wav: 16ch 16bit 48000Hz  per-channel RMS = 7,   1, 1, 1, …
20260723_105643_825_(untitled).wav: 16ch 16bit 48000Hz  per-channel RMS = 247, 1, 1, 1, …
20260723_183352_848_.wav:           16ch 16bit 48000Hz  per-channel RMS = 257, 1, 1, 1, …
```
(RMS in 16-bit units out of 32767; `1` is dither, not signal.)

The engine agrees — `twSampleSource: "…20260712_164250_756_(untitled).wav":
16 channels, 48000 Hz, 16 bits per sample` — and rendering that project
headlessly gives **ch0 RMS 0.0853535 against ch1 RMS 0.000154446**, a factor of
553.

**Every component in between is behaving exactly as designed.** Proposal 36 B3
made a reader keep its *file's* width, the track is width 2, so it takes the
file's channels 0 and 1 — and channel 1 of the file is silent. The §4.4 clamp
does not apply, because it only widens a *narrow* page; a 16-channel page
feeding a 2-channel track is a legitimate narrowing.

## 3. Why it only appeared now

Before proposal 36 the engine rendered `idx = 0` only and the sink duplicated it
into both outputs, so channel 0 reached both speakers and a file's extra
channels were unreachable. The recordings were **always** 16-channel; it was
invisible. Carrying channels honestly made a pre-existing defect audible.

This is the same shape as proposal 36 B4's golden, where the "correct" old bytes
turned out to be a *saturating* render nobody had noticed.

## 4. What is actually wrong — traced, not guessed

`CLAUDE.md` § Recording Audio claims a recorded file's **Channels** are "Stereo
(or project channel count)". **That has been false since this code was written.**
It is the **input device's capture channel count**, minus the per-track mask:

| Step | Location | What happens |
|---|---|---|
| 1 | `main/shell/src/smainwindow.cpp:818` | `params.channels = 2;` — hard-coded |
| 2 | `tw303a/record/include/tw/record/recording_session.h:23` | `std::uint32_t channels = 2; // device input channel count` |
| 3 | — | **`params_.channels` is read NOWHERE.** Step 1 is dead code |
| 4 | `record/src/recording_session.cc:232` | `openDevice(inputDeviceId, sampleRate)` — device id and **rate only**; no channel count is requested |
| 5 | `record/src/recording_session.cc:244-247` | `const AudioInputConfig &in = input->getConfig(); channels = in.channels;` — **the authority** |
| 6 | `devices/src/wasapi_input.cc:151` | `config_.channels = deviceFormat->nChannels;` — the endpoint's shared-mode mix format. **16 on a 16-input interface** |
| 7 | `record/src/recording_session.cc:288-300` | per armed track: `outChannels = channels`, recounted from the mask's set bits **only if `trackChannelMask != 0`** |
| 8 | `record/src/recording_session.cc:315-317` | `fileConfig.channels = outChannels` — the WAV header width |
| 9 | `record/src/recording_session.cc:404-412` | `filterChannels(...)`, which (`:29-64`) **returns every channel verbatim when the mask is 0** |

### The per-track selection is NOT broken — its DEFAULT is

An earlier draft of this note said the ARM channel selection never reaches the
writer. **That was wrong.** It does: `ssmvmixercontrol.cpp:745-822` →
`SObject::recordingChannels_` → `smainwindow.cpp:825` → `params.trackChannels`
→ step 9. Pick "Channel 1" on a 16-input interface and you genuinely get a
1-channel file.

What bites is that **`recordingChannels_` defaults to `0`, and `0` means "all
channels"** (`sobject.h:784`) — so an armed track records everything the
interface offers unless the user has explicitly been into a right-click menu
they have no reason to suspect exists.

And it is **never serialized**: `sobject.cpp:131-132` writes
`armedForRecording` and nothing writes or reads a `recordingChannels`
attribute anywhere (`grep -rn recordingChannels main/persistence/` is empty).
**Every save/reload silently reverts an armed track to "All Channels"** — so a
default-side fix is the only thing a returning user would ever see, and
serializing it is not optional.

### The decision this needs

| Option | Consequence |
|---|---|
| Default a newly armed track to the first channel (or first stereo pair) rather than mask 0 | Smallest change; the existing UI keeps meaning what it says |
| Keep mask 0, but clamp the written width to the project's at open time | Also small; silently discards inputs on a genuine multitrack capture |
| Fold N inputs down to the project width | **Argue against**: it sums unrelated physical inputs |
| Keep all inputs, add a per-clip source-channel choice | Most faithful, and much the largest piece of new model |

"All Channels" exists precisely for the user doing a live multitrack capture, so
whatever is chosen should keep that reachable.

## 5. Existing recordings

Files already on disk are 16-channel with content in channel 0. Any fix to the
recorder leaves them as they are, so a migration path (convert to mono, or a
per-clip channel choice) is part of the problem, not separate from it.

## 6. Test coverage — this was untested end to end

- **Nothing in the suite records anything.**
- **No fixture is wider than 4 channels** (`test_channels4.wav`); the only
  1-channel fixture, `test_position.wav`, is used exclusively by
  position-decoding cases that never inspect channel content.
- So "a file wider than the track feeding a narrower track" has no coverage at
  all, which is why nine milestones of channel work never tripped over it.

A case that inserts a wide-but-mostly-silent fixture into a 2-channel project
and asserts what reaches the track root would have caught this the moment B3
landed.
