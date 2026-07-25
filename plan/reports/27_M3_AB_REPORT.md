# A/B time-stretch / pitch quality report

- Generated: 2026-07-25 05:13:26Z
- Reference backend: `TW_STRETCH_BACKEND=rubberband`
- Candidate backend: `TW_STRETCH_BACKEND=vocoder`
- Corpus: deterministic 16-bit PCM stereo 48 kHz, 4.0 s each (warp_ab --gen)
- Sidecars disabled (`SMARAGD_SIDECAR_DIR=off`)

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
| stretch 2/1 | A/B | +1.81 | 1.95 | 0.000 | 1.45/1.45 | 0/0 | -0.0 ⚑ | 4.3 |
| stretch 1/2 | A/B | +0.34 | 0.34 | 0.000 | 0.89/0.89 | 0/0 | +0.0 | 6.5 |
| stretch 3/2 + pitch +300c | A/B | +1.14 | 1.28 | 0.000 | 1.06/1.06 | 0/0 | +0.0 ⚑ | 10.1 |
| pitch +1200c | A/B | +0.28 | 0.35 | 0.000 | 1.13/1.13 | 0/0 | -0.0 | 11.3 |
| pitch -700c | A/B | +0.40 | 0.57 | 0.000 | 0.65/0.65 | 0/0 | -0.0 | 1.3 |

## corpus_sine440.wav

| Transform | Status | RMSΔ% | perSec maxdev% | freq maxdev% | rise ratio mean/max | unpaired r/c | warble ΔdB | specbal maxΔdB |
|---|---|---|---|---|---|---|---|---|
| stretch 2/1 | A/B | +0.03 | 0.32 | 0.000 | 1.35/1.35 | 0/0 | +0.0 ⚑ | 4.3 |
| stretch 1/2 | A/B | +0.52 | 0.64 | 0.000 | 0.63/0.63 | 0/0 | +0.0 | 11.6 |
| stretch 3/2 + pitch +300c | A/B | +0.16 | 0.74 | 0.000 | 0.88/0.88 | 0/0 | +0.1 ⚑ | 12.4 |
| pitch +1200c | A/B | +0.02 | 0.15 | 0.000 | 1.19/1.19 | 0/0 | +0.0 | 5.7 |
| pitch -700c | A/B | +0.25 | 0.71 | 0.000 | 0.37/0.37 | 0/0 | +0.0 | 16.5 |

## corpus_voice.wav

| Transform | Status | RMSΔ% | perSec maxdev% | freq maxdev% | rise ratio mean/max | unpaired r/c | warble ΔdB | specbal maxΔdB |
|---|---|---|---|---|---|---|---|---|
| stretch 2/1 | A/B | -0.74 | 0.96 | 0.004 | 1.48/1.48 | 0/0 | -0.2 | 0.5 |
| stretch 1/2 | A/B | -1.89 | 1.89 | 0.005 | 0.90/0.90 | 0/0 | +0.2 | 1.7 |
| stretch 3/2 + pitch +300c | A/B | -0.03 | 0.26 | 0.008 | 1.13/1.13 | 0/0 | +0.4 | 0.2 |
| pitch +1200c | A/B | -0.75 | 0.81 | 0.001 | 1.01/1.01 | 0/0 | +0.2 | 0.5 |
| pitch -700c | A/B | -0.51 | 0.78 | 0.002 | 1.74/1.74 | 0/0 | -0.5 | 1.0 |

## corpus_transients.wav

| Transform | Status | RMSΔ% | perSec maxdev% | freq maxdev% | rise ratio mean/max | unpaired r/c | warble ΔdB | specbal maxΔdB |
|---|---|---|---|---|---|---|---|---|
| stretch 2/1 | A/B | +3.42 | 17.36 | 3636.482 | 2.16/3.73 | 0/0 | +0.1 | 1.5 |
| stretch 1/2 | A/B | +14.77 | 19.77 | 4.463 | 1.52/2.58 | 0/0 | +0.1 | 1.8 |
| stretch 3/2 + pitch +300c | A/B | +6.44 | 19.50 | 15.830 | 1.82/3.23 | 0/0 | +0.1 | 2.4 |
| pitch +1200c | A/B | +3.39 | 9.30 | 1.164 | 2.20/3.35 | 0/0 | +0.0 | 1.5 |
| pitch -700c | A/B | +39.72 | 78.41 | 5.069 | 1.13/1.47 | 0/0 | +0.1 | 2.5 |
