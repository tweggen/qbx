# tw/sampler — CONTRACT

Purpose: NassauDigital, the Nassau family's sampler, as a native instrument of
qbx's own `tw` format (proposal 56; nassau-digital `docs/DESIGN.md` §9). This
module owns the `twPlugin` subclass (`twSamplerInstrument`, format `tw`, uid
`tw.native.nassaudigital`), the extension interface the app reaches it through
(`twSamplerHost`), the two immutable snapshots it is handed
(`twSamplerProgram`, `twSamplerSample`), the EDITABLE program the app's model
wraps (`twSamplerProgramModel`, since Q1), and its registration with the plugin
registry. The voice engine itself is the NassauDigital CORE
(tweggen/nassau-digital `Source/Core`, `nd::Instrument`); this module is the
translation between qbx's plugin ABI and the core's API and nothing else.

Built in milestone Q0 (QBX-250); Q1 (QBX-253) added `twSamplerProgramModel`
and `twSamplerHostStats::installedSerial`. The program MODEL object
(`objects/sampler`'s `SSamplerProgram`, with undo, persistence and signals) and
its verbs are Q1's, in the app; generations, live asset links, stretch, live
play, recording, the editor and the aux outputs are Q2–Q8.

Public headers: twsamplerhost.h, twsamplertypes.h, twsamplerregistry.h,
twsamplerprogrammodel.h. All Qt-free (THREADING.md rule 1), and none names a
core type.

Depends on: tw/core, tw/graph, tw/events, tw/plugins (and tw/pages through
graph, for one constant: `twOutputPage::FRAME_CAPACITY`). `check_layering.py`
also grants tw/sources, for Q4's stretch renderer (`twPagedVocoder`); it is
not linked until Q4 uses it. Plus the NassauDigital core, linked PRIVATE.
Forbidden: app headers, mix, pages beyond the one constant, playback, devices,
sinks. Nothing below the app layer depends on this module.

## Where the core comes from

Not vendored and not a submodule (proposal 56 A2, the author's decision of
2026-10-26): nassau-digital is pinned ONCE for a release, as a top-level
nassau-suite submodule. `smaragd/cmake/NassauDigital.cmake` is a VERBATIM copy
of nassau-digital's `cmake/bootstrap/NassauDigital.cmake` and resolves, in one
order: `-DNASSAU_DIGITAL_DIR` (what the suite passes), a sibling
`../nassau-digital` beside the qbx checkout's ROOT
(`NASSAU_DIGITAL_SIBLING_ROOT`, set in `tw303a/CMakeLists.txt` because that
file sits two levels down), then a FetchContent of `NASSAU_DIGITAL_GIT_REF`, a
commit on nassau-digital's main. Every configure logs
`nassau-digital: <route>, <dir> at <commit>`. Finding nothing is FATAL with
the clone command (B1): the instrument is required, not optional. A qbx
worktree under `.claude/worktrees/` has no sibling at that depth and takes the
fetch route, which the log says.

From core gate C5 on the core also links nassau-dsp, and finds it itself
through its own verbatim `cmake/NassauDSP.cmake`: `-DNASSAU_DSP_DIR`, then
`../nassau-dsp` beside the CORE's checkout, then a fetch of the core's own
`NASSAU_DSP_GIT_REF`; it logs `nassau-dsp: <route>, <dir> at <commit>`. qbx
carries nothing for it. A bare clone fetches both.

## Invariants

1. **THE CORE IS SEEN BY `tw/sampler` ONLY** (proposal 56 invariant 1). It is
   linked PRIVATE (`target_link_libraries(tw_sampler PRIVATE
   nassau::digital)`), the one header that includes it
   (`src/twsamplerimpl.h`) lives in `src/`, and the public snapshots are
   OPAQUE (`Impl` is completed in `src/` only). An app module or another
   engine module that tried to include a core header would not find it; the
   build is the gate, as for the CLAP headers (tw/plugins inv. 4).
2. **REGISTERED BY CALL, NEVER BY STATIC INITIALIZER** (invariant 2).
   `registerNassauDigital(twPluginRegistry &)` hands the registry the
   descriptor and a factory through `registerBuiltin` (tw/plugins inv. 1). The
   composition root calls it (`SApplication::initPluginRegistry`, before the
   first scan) and so does every test that wants the instrument. Idempotent.
3. **EVERY `twSamplerHost` CALL GOES THROUGH `applyToInstances`** (invariant
   3), under the slot processor's mutex (tw/plugins inv. 15a), so an install
   lands between two `process()` calls and `process()` reads the program, the
   samples and the routed mask with NO LOCK of its own — which is also the
   core's own rule ("a setter never runs concurrently with process()"). No
   `twSamplerHost` method is called through the bare `plugin()` pointer. The
   one exception is `stats()`, which reads atomics.
4. **`process()` ALLOCATES NOTHING AND NEVER RELEASES A BUFFER** (R3, invariant
   5). The event buffer is sized in the constructor, the voice pool in
   `prepare()` (by the core, for the largest polyphony). A replaced buffer is
   parked BY THE CORE (`nd::Instrument::setSample`) and freed by
   `collectRetired()` → `nd::Instrument::collectGarbage()` on the host thread
   once no voice and no other owner holds it.
5. **A VOICE KEEPS ITS GENERATION** (DESIGN §6.1 rule 4): a sample installed
   while a note sounds reaches the NEXT note-on, not the sounding one (the
   core latches the buffer at note-on). Gated by `sampler_test` part E: two
   contiguous renders with an install between them equal one uninterrupted
   render, bit for bit.
6. **THE STATE BLOB IS A REFERENCE, NEVER THE PROGRAM** (A3, Q0.4). An 8-byte
   header — `'TWND'`, u16 version (1), u16 reserved, the CLAP frame's shape
   (tw/plugins inv. 3) — and the program-level parameters once proposal 56 Q-2
   names any; a newer version is refused. Until Q-2 is answered the instrument
   declares NO host parameters and the blob is the header alone. The program
   and every sample live in the model, so a 10 MiB sample adds 0 bytes to the
   saved project (`sampler_slot_test`).
7. **ONE STEREO BUS UNTIL Q8** (B7, invariant 10). `io = {0, 2}`,
   `nOutBuses = 1`, `outBusChannels = {2}`: the host hands a generator exactly
   one bus array (`twpluginslotproc.cc`, the generator render), so the
   instrument never writes an aux bus until proposal 55's host passes every
   bus. `setRoutedOutputs()` is stored and read by `process()` already (so the
   seam exists), and changes nothing audible yet: every zone plays on main.
   The generator rows are the processor's: DirectGen on 2 channels, GenFold on
   1, Transparent + Unsupported wider than 2 (proposal 56 Q-7).
8. **THE ROUND-ROBIN CELL IS THE PAGE GRID** (B10, invariant 11), from frame 0:
   `floor(twProcessContext::position / twOutputPage::FRAME_CAPACITY)`. The
   core has no cell size; it has `startPage()`, and this module calls it
   whenever the cell of the frame about to be rendered differs from the last
   one (and after `reset()`/`prepare()`). A chunk that straddles a cell
   boundary is rendered as two core blocks split at the boundary. A call with
   no position (the legacy positionless pull) never starts a page.
9. **DETERMINISTIC** (R8, A17): reset, the same notes, the same bytes. No
   clock, no random, and the core's control grid counts from each note-on, so
   the bytes do not depend on the 4096-frame chunking. The core and this module
   compile with `-ffp-contract=off` (GNU/Clang), so the bytes do not depend on
   whether the CPU fuses a multiply-add; the render golden
   `tests/goldens/sampler_q0_note.wav` assumes it.
10. **NO `-ffast-math`, NO `-march=native`** (nassau-digital R7). The
    configure STOPS if the global flags, the directory's compile options or
    either target's options contain `-ffast-math`, `-Ofast`, `/fp:fast`,
    `-march=native` or `-mcpu=native` (`tw303a/CMakeLists.txt`, logged as
    `tw_sampler: R7 flag check passed`).

## The editable program (Q1)

`twSamplerProgramModel` is a pimpl over one `nd::Program` and a THIN
FORWARDER to the core's edits (`nd_edit.h`) and grammar (`nd_program_xml.h`):
the parameter tables, the ALL scope, the per-target clamp of a relative edit
and its refusal on an enum or a bool, "every edit returns the absolute values
it replaced", ids from `nextId` never reused, and unknown attributes kept in
document order are all the core's, re-stated nowhere here (proposal 56
invariant 8). What it adds, because a host needs it and the core's API does not
spell it out:

- **add from attribute TEXT**, REFUSING an attribute the table does not know
  and a LAYER's `id` (assigned, never taken) — the core's `addZone()` takes a
  built `Zone`, and a script's typo must be refused rather than kept as an
  "unknown attribute". A ZONE may name an explicit `id` (QBX-265, the
  author's answer of 2026-11-01): accepted when it is free and never handed
  out (`>= nextId`), by setting `nextId` to it before the core's `addZone()`
  assigns it, so `nextId` moves past the zone and its layers; refused when
  in use (`zone id N is in use`) or below `nextId` (`zone id N was handed out
  before (nextId M); ids are never reused`). The ids a jump skips are below
  `nextId` from then on and are never handed out;
- **a removed zone / layer as the core's own XML** plus where it was, and its
  reinsertion VALIDATED: every id free AND already handed out (`< nextId`).
  The core's `reinsertZone()` checks only the zone id and the index;
- **`equalsIgnoringNextId()`**: an undone add-zone leaves `nextId` where the
  add put it, so that is the equality "undone exactly" means for it;
- **`snapshot()`**: a COPY as an immutable `twSamplerProgram`, with a serial
  from the same counter `fromXml()` uses.

`twSamplerHostStats::installedSerial` is the serial of the program the
instance most recently got through `setProgram()`, written there as a relaxed
atomic. `programSerial` says what `process()` READ; `installedSerial` says what
the instance HOLDS, which is what the app asserts its edits on without a
render (`assert-sampler-program installed="1"`).

## The ABI translation

| qbx (`twEvent`, ABI domain) | core (`nd::Event`) |
|---|---|
| `NoteOn` key k, value v in [0,1] | `NoteOn` k, velocity round(127 v), at least 1 |
| `NoteOff` key k | `NoteOff` k |
| `NoteChoke` key k | `NoteOff` k (the core has no choke yet) |
| `NoteOff`/`NoteChoke` key -1 | `NoteOff` for every key this instance holds |
| `ControlChange` paramId 1, value v | `ModWheel` round(127 v) |
| anything else | dropped (no mapping in the core yet) |

`twProcessContext::position` is the core's timeline frame; `tempoBpm` (when
`twCtxTempo` is set) goes to `nd::Instrument::setTempo` per chunk, for the
resample tempo modes.

`tailFrames()` is the longest amp release in the program, extended to the
longest installed sample when the program has a one-shot zone (which plays to
the sample's end whatever the note-off says); it sizes the processor's
pre-roll, capped at 4 s by the processor.

## Gates

- `sampler_test` (tw303a/sampler/tests): registration by call (A), the
  capability (B), the generator rows (C), installs through applyToInstances
  read by `process()` (D: silent before frame 1000, the sample after it within
  the interpolator's passband ripple — measured 7.8e-4 on a 0.5-amplitude
  440 Hz sine — and `twSamplerHostStats` reporting the program serial, the
  routed mask and the sample count `process()` ran on), a voice keeping its
  generation (E), installs racing renders (F), and the blob (G).
- `sampler_slot_test` (main/testkit): Q0.4 and Q0.q2 on a real slot.
- `qxa.sampler_q0_engine_seam`: Q0.3, Q0.q2 and Q0.q4 through the app.
- `qxa.sampler_q1_verbs`, `qxa.sampler_q1_persist`,
  `qxa.sampler_q1_newer_program`: `twSamplerProgramModel` through the Q1 verbs
  and the project file (see `main/objects/sampler/CONTRACT.md`).

## Known limits (recorded, not hidden)

- The core reads every layer through a band-limiting interpolator (a
  Kaiser-windowed sinc, cutoff 0.45 fs) even at the root pitch, so a render is
  the sample only to the filter's passband ripple, and material near Nyquist
  comes out filtered. The gates compare against renders or within a measured
  tolerance for that reason, never "sample for sample".
- `NoteChoke` releases instead of cutting: the core has no choke event.
