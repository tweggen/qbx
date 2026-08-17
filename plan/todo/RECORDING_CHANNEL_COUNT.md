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

## 4. What is actually wrong, and the decision it needs

`CLAUDE.md` § Recording Audio claims a recorded file's **Channels** are "Stereo
(or project channel count)". That is not what happens. `RecordingParams.channels`
is hard-coded to `2` in `SMainWindow`, and the files have 16 — so the width is
coming from the **input device's** `AudioInputConfig` somewhere on the write
path, and neither the project width nor the per-track selection reaches the
writer.

Note the UI for this **already exists and is half-wired**: the ARM button's
right-click menu offers "All Channels", per-channel toggles and stereo pairs,
writing `SObject::recordingChannels_` (a bitmask, 0 = all) — which is **never
serialised** (`serializeSelfAttributes` omits it) and, on this evidence, does not
reach the file either.

**The product decision** — what should a user with a 16-input interface and
"All Channels" selected get?

| Option | Consequence |
|---|---|
| Write only the selected channel(s) | Honours the existing UI; "All Channels" on a 16-input rig still yields 16-channel files |
| Fold/select down to the project width | Always playable, but silently discards inputs the user may have wanted |
| Keep all inputs, fix it at the CLIP (a per-clip source-channel choice) | Most faithful, and the largest piece of new model |

Whatever is chosen, `recordingChannels_` should probably start being serialised,
and `CLAUDE.md`'s claim should be corrected to whatever becomes true.

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
