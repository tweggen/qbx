# docs/archive — superseded design notes

Nothing in this directory describes the system as it is today. Every file here
carries a banner naming what replaced it and when. They are kept, rather than
deleted, because several of them hold the only written account of a failure the
current rules exist to prevent — and a rule is easier to keep when you know
which bug wrote it.

**If you are looking for current documentation:**

| You want | Read |
|---|---|
| the module map | [`docs/ARCHITECTURE.md`](../ARCHITECTURE.md) |
| how a page is frozen | [`docs/contracts/FREEZE_PROTOCOL.md`](../contracts/FREEZE_PROTOCOL.md) |
| which thread may do what | [`docs/contracts/THREADING.md`](../contracts/THREADING.md) |
| who speaks which time domain | [`docs/contracts/POSITION_DOMAINS.md`](../contracts/POSITION_DOMAINS.md) |
| how a clip on a track works | [`docs/contracts/CLIP_MODEL.md`](../contracts/CLIP_MODEL.md) |
| what was implemented, and when | [`plan/STATE.md`](../../plan/STATE.md) |
| a module's own invariants | that module's `CONTRACT.md` |

## What is in here

**Milestone plans and completion reports** (2026-06-28) — `PHASE1_IMPLEMENTATION_PLAN.md`,
`PHASE5_UNIFIED_PLAYBACK_RENDER.md`, `PHASE5A_FOUNDATION_COMPLETE.md`,
`PHASE5B_PLAYBACK_UNIFICATION_COMPLETE.md`, `PHASE5C_RENDER_UNIFICATION_COMPLETE.md`.
Written before the module split; `plan/STATE.md` is the authoritative record.

**The unified-rendering chain** — `UNIFIED_RENDERING_ARCHITECTURE.md` (2026-06-22)
was explicitly replaced by `UNIFIED_RENDERING_ARCHITECTURE_V2.md` (2026-06-27),
and `UNIFIED_RENDERING_ARCHITECTURE_V2_GAPS.md` compares V2 against the tree of
that same week. Only V3, which stayed in `docs/`, still describes live code (the
teardown protocol: `ComponentState::ZOMBIE` is in `twrewire.cc` today).

**Thread-safety audits** (2026-06-07 … 06-29) — `THREAD_SAFETY_ANALYSIS.md` and
its `.txt` summary, `TWPLUGININSERT_THREAD_SAFETY_ANALYSIS.md`,
`TWSPEAKER_TRACKMIX_RACE_CONDITIONS.md`, `APPLYING_NOLOCK_PATTERN.md`,
`MULTITHREADING_POLICY.md`, `PAGE_CACHE_SAFETY_PROOF.md`. These are where the
rules in `docs/contracts/THREADING.md` came from. Two are worth reading for the
narrative alone: the `QFile` shared between the waveform preview and the audio
callback, and the `twGrainSource` deleted out from under an audio-thread
snapshot. Note that `PAGE_CACHE_SAFETY_PROOF.md`'s conclusion has since been
qualified — `THREADING.md` rule 1 documents a deliberately *accepted* race.

**Test documents describing a suite that was never built** — `TEST_PLAN.md` and
`TESTING_PROTOCOL.md` plan a GoogleTest tree at `tw303a/test/`, which does not
exist. `EXACT_ARITHMETIC_TESTING.md` is a pass-count snapshot against pre-split
paths.

**Superseded implementation notes** — `IMPLEMENTATION_SUMMARY.md` (the colour
modifier; the concept and the rejected HSV alternative stayed in `docs/`) and
`ZOOM_SCROLLBAR_USAGE.md` (the widget's header is the reference now).

## Adding to this directory

Move a document here when it has been replaced rather than merely aged, and give
it a banner in the same shape as the others: what superseded it, when it stopped
being current, and what is worth reading it for anyway. A file with nothing worth
reading it for should be deleted instead — git history keeps it either way.
