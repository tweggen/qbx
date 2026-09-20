# test_sawtooth.wav and render/playback alignment

`smaragd/tests/test_sawtooth.wav` is the workhorse audio fixture: about 175
`.qxa` cases load it. This page says what it actually contains and what it can
and cannot prove.

## What the file is

Measured from the committed file on 2026-09-20 (`smaragd/tests/test_sawtooth.wav`,
768044 bytes):

| Property | Value |
|---|---|
| Format | 16-bit PCM WAV, **2 channels**, 48 kHz |
| Length | **192000 frames — exactly 4.000 s** |
| Content | a **440 Hz sawtooth**: 1760 periods in 4 s, 109.09 frames each |
| Shape | strictly rising within a period, one discontinuity per period — **not band-limited** |
| Amplitude | peaks at −26155 / +26021, about 0.8 of full scale |
| Channels | **bit-identical to each other** |

Re-measure rather than trust this table if anything depends on it:

```bash
python3 - <<'PY'
import wave, struct
w = wave.open('smaragd/tests/test_sawtooth.wav')
n, ch, sr = w.getnframes(), w.getnchannels(), w.getframerate()
d = struct.unpack('<%dh' % (n*ch), w.readframes(n))
L, R = d[0::ch], d[1::ch]
print(f"{ch}ch {sr}Hz {n} frames = {n/sr:.4f}s  identical={L==R}")
print("periods:", sum(1 for i in range(1, n) if L[i] < L[i-1]))
PY
```

> **The two channels being identical is a trap.** On this fixture the source
> channel always equals the output channel, so a per-channel bug is invisible.
> Never make a per-channel claim over `test_sawtooth.wav` alone — pair it with
> `test_stereo.wav` or `test_channels4.wav`, and include a **mono** source such
> as `tests/test_position.wav` when the claim is about channel indexing.

> **There is no generator, and that is deliberate.** `generate_test_wav.cpp`
> used to sit at the repository root, wired into no build, still producing the
> *old* fixture — mono, four quantized full-scale ramps, 1,048,576 samples,
> 21.85 s. Running it would have silently replaced the file 175 cases depend on,
> and the failures would have read as engine regressions rather than as a
> changed input. It was deleted 2026-09-20; `git log --diff-filter=D -- generate_test_wav.cpp`
> finds it. **Do not write a replacement** without reading the next section
> first.

## The formula behind it, and why it is not regenerable

The signal is a 440 Hz sawtooth under a linear amplitude envelope, truncated
toward zero:

```python
S, f, N, sr = 0.8 * 32767, 440.0, 192000, 48000
ph = f * i / sr
v  = int((i / N) * (2.0 * (ph - math.floor(ph)) - 1.0) * S)   # both channels
```

In double precision this reproduces **191994 of the 192000 frames exactly**.

The remaining **six do not, and no formula will fix them**, because the file
itself is inconsistent there. All six sit at `i % 1200 == 0`, where
`440·i/48000` is an exact integer and the sawtooth is at its discontinuity —
but so do 154 other frames, and those agree with the formula. At `i = 1200` the
file holds the negative value the formula gives; at `i = 27600` it holds the
positive one:

| i | formula | file |
|---|---|---|
| 1200 | −163 | −163 |
| 27600 | −3768 | **+3768** |
| 49200 | −6717 | **+6717** |
| 55200, 98400, 104400, 110400 | negative | **positive** |

A phase accumulator instead of the multiply is worse, not better (153
mismatches). float32 throughout is far worse (86189). The likeliest explanation
is the original's float→int16 write path resolving the discontinuity
differently depending on where the accumulated error landed — which is not
something a rewrite can reproduce on purpose.

So: the fixture is a **committed binary with no generator**, like
`test_stereo.wav`, `test_channels4.wav` and `test_position.wav`. Treat the
bytes as the definition. The formula above is for *reasoning* about the
signal — predicting where a discontinuity falls, what the amplitude is at a
given frame — not for producing it.

## What it is good for

A 440 Hz ramp with a hard discontinuity every 109 frames is easy to reason
about at sample granularity: a timing error moves the discontinuities, and a
dropped or duplicated block shows up as a period that is the wrong length.
That makes the file useful for render/playback alignment and for clip-window
arithmetic (split, slip, loop), which is what most of the cases using it do.

It is **not** a full-scale signal and **not** a quantized staircase, so it does
not exercise clipping, and it cannot show a value-level quantization error the
way a held-value ramp would.

## How alignment is actually gated

Render exactness is gated by **byte-level `cmp` of rendered WAVs** — they are
16-bit PCM, so never parse them as float32. A render and the playback of the
same material go through the same frozen pages, so a divergence between them is
a bug in one of the two consumers rather than in the material.

The mechanism that makes that true is the freeze protocol, not a shared
resampler call: both offline render and playback read frozen `twOutputPage`s by
position, and a page is a pure function of its position and its inputs. See
[`docs/contracts/FREEZE_PROTOCOL.md`](contracts/FREEZE_PROTOCOL.md) for the
normative sequence and
[`docs/contracts/POSITION_DOMAINS.md`](contracts/POSITION_DOMAINS.md) for which
positions each side speaks.

---

*An earlier version of this page walked through `twSpeaker`'s render callback
and `RenderSession`'s loop pulling mono frames through a resampler and
expanding them to stereo in place. That engine is gone: since proposal 36 B4 a
page is planar and `SProject::channels()` wide, there is one track mix rather
than one per bus, and playback reads frozen pages rather than pulling the synth
plug directly. The walkthrough was removed rather than corrected, because
`docs/contracts/FREEZE_PROTOCOL.md` already describes the current path. The old
text is in git history: `git show 1e93d080:docs/FIXING_RENDERING.md`.*
