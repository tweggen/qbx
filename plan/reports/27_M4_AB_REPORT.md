# A/B time-stretch / pitch quality report

- Generated: 2026-07-25 07:02:10Z
- Reference backend: `TW_STRETCH_BACKEND=rubberband`
- Candidate backend: `TW_STRETCH_BACKEND=vocoder`
- Corpus: deterministic 16-bit PCM stereo 48 kHz, 4.0 s each (warp_ab --gen)
- Sidecars: per-render hermetic dir (onsets aspect active — vocoder
  onset keyframes engaged; warp.pcm cold per run)

Status legend: **identical** = candidate bytes == reference (vocoder not
yet distinct; plumbing validated, all metrics must be ~0) · **A/B** = the
two backends diverged, metrics are meaningful · **candidate-missing** =
vocoder produced no output · **ref-missing** = reference render failed.

Metric columns (candidate vs reference): RMSΔ overall %, per-second max
RMS dev %, dominant-freq max dev %, transient rise-time ratio mean/max,
unpaired onsets ref/cand, warble ΔdB (cand−ref, +flag), spectral-balance
max band ΔdB.

## corpus_saw220.wav

| Transform | Status | RMSΔ% | perSec maxdev% | freq maxdev% | rise ratio mean/max | unpaired r/c | warble ΔdB | specbal maxΔdB |
|---|---|---|---|---|---|---|---|---|
| stretch 2/1 | A/B | +1.55 | 1.67 | 0.106 | 1.45/1.45 | 0/0 | -0.3 ⚑ | 6.4 |
| stretch 1/2 | A/B | -0.13 | 0.48 | 0.145 | 0.89/0.89 | 0/0 | -0.2 | 3.7 |
| stretch 3/2 + pitch +300c | A/B | +0.62 | 0.95 | 0.268 | 1.06/1.06 | 0/0 | +0.4 ⚑ | 9.7 |
| pitch +1200c | A/B | +0.02 | 0.11 | 0.061 | 1.13/1.13 | 0/0 | -0.1 | 16.2 |
| pitch -700c | A/B | +0.10 | 0.56 | 0.001 | 0.65/0.65 | 0/0 | -0.1 | 1.2 |

## corpus_sine440.wav

| Transform | Status | RMSΔ% | perSec maxdev% | freq maxdev% | rise ratio mean/max | unpaired r/c | warble ΔdB | specbal maxΔdB |
|---|---|---|---|---|---|---|---|---|
| stretch 2/1 | A/B | -0.26 | 0.37 | 0.102 | 1.35/1.35 | 0/0 | -0.2 ⚑ | 12.4 |
| stretch 1/2 | A/B | -0.03 | 0.17 | 0.147 | 0.63/0.63 | 0/0 | -0.1 | 13.1 |
| stretch 3/2 + pitch +300c | A/B | -0.60 | 0.95 | 0.185 | 0.88/0.88 | 0/0 | +0.7 ⚑ | 18.8 |
| pitch +1200c | A/B | -0.27 | 0.28 | 0.065 | 1.19/1.19 | 0/0 | -0.0 | 11.4 |
| pitch -700c | A/B | -0.18 | 0.84 | 0.001 | 0.37/0.37 | 0/0 | -0.1 | 16.2 |

## corpus_voice.wav

| Transform | Status | RMSΔ% | perSec maxdev% | freq maxdev% | rise ratio mean/max | unpaired r/c | warble ΔdB | specbal maxΔdB |
|---|---|---|---|---|---|---|---|---|
| stretch 2/1 | A/B | -1.28 | 1.82 | 2.298 | 1.48/1.48 | 0/0 | -0.6 | 0.7 |
| stretch 1/2 | A/B | -2.34 | 2.44 | 0.060 | 0.90/0.90 | 0/0 | -0.4 | 1.5 |
| stretch 3/2 + pitch +300c | A/B | -0.40 | 0.65 | 2.251 | 1.16/1.16 | 0/0 | -0.4 | 0.3 |
| pitch +1200c | A/B | -1.30 | 1.53 | 2.379 | 1.01/1.01 | 0/0 | -1.2 | 1.5 |
| pitch -700c | A/B | -0.89 | 1.43 | 0.002 | 1.74/1.74 | 0/0 | -0.5 | 0.8 |

## corpus_transients.wav

| Transform | Status | RMSΔ% | perSec maxdev% | freq maxdev% | rise ratio mean/max | unpaired r/c | warble ΔdB | specbal maxΔdB |
|---|---|---|---|---|---|---|---|---|
| stretch 2/1 | A/B | +2.85 | 17.29 | 1023.722 | 2.14/3.52 | 0/0 | +0.0 | 1.6 |
| stretch 1/2 | A/B | +14.72 | 20.90 | 15.165 | 1.30/2.53 | 0/0 | +0.0 | 1.7 |
| stretch 3/2 + pitch +300c | A/B | +6.22 | 19.69 | 15.978 | 1.99/4.34 | 0/0 | -0.3 | 2.6 |
| pitch +1200c | A/B | +2.92 | 9.11 | 1.231 | 2.18/3.36 | 0/0 | +0.7 | 2.1 |
| pitch -700c | A/B | +40.51 | 79.02 | 5.437 | 0.96/1.16 | 0/0 | -0.2 | 2.7 |
