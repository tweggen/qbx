# tw/events — CONTRACT

Purpose: the MIDI/event model leaf (proposal 37 P0b) — **the one** `twEvent`,
the immutable event sequence, the tempo map (the only tick↔frame converter),
the SMF reader/writer, the automation curve, and the event clip set + feed
merge that the instrument slot and the MIDI-out pump read through.

Public headers: twevent.h, tweventseq.h, tweventsource.h, tweventclipset.h,
tweventmerge.h, twtempomap.h, twsmf.h, twautomationcurve.h.

Depends on: **tw/core ONLY**. Forbidden: everything else — pages, graph, mix,
plugins, and app headers. Two independent reasons, both load-bearing:
events are model data and not pages (D1), so there is nothing in the dataflow
DAG for this module to attach to; and `tw/plugins` will consume the clip set
through `twEventSource`, while `tw/plugins` may not include `tw/mix` (proposal
36 F15) — which is why the clip set lives here and returns a core
`twFrameRange` rather than `tw/mix`'s `twEditRange`.

Nothing in the dataflow DAG links `tw_events` yet. P1 (the app model) and P2
(the plugin ABI) are its first consumers.

## Invariants

1. **One `twEvent`.** The engine snapshot, the clip set, the plugin ABI and
   MIDI-out all use the struct in `twevent.h` (proposal 37 §4.1), pinned:
   fields are appended, never renamed or reordered, because both the ABI and
   the file format quote it. A second, format-flavoured event type is what
   review #2 rejected.

2. **`time` has exactly two documented uses.** A POSITION in the owner's domain
   inside a sequence or a clip set, and CHUNK-RELATIVE (0..n−1) inside a
   `process()` call. There is no third.

3. **The DOMAIN of a sequence's `time` belongs to the OWNER, not to the
   sequence.** `twSmf` produces tables in musical TICKS at the file's division;
   `twEventClipSet` reads its resolved sequences in whatever domain the clip's
   map produces (frames, for a track). Nothing in `twEventSeq` converts, and
   nothing may assume one domain.

4. **`twTempoMap` is the only tick↔frame converter and the single tempo
   authority.** Tempo is stored as SMF's own unit — µs per quarter, an integer
   — and BPM is DERIVED (`6e7/us`). Storing seconds-per-beat somewhere else and
   µs/quarter here makes the two disagree in the tenth microsecond, which lands
   on a frame boundary in a long project. The conversion is exact rational
   (`frames = ticks · us · srate / (ppq · 10⁶)`); rounding is the caller's
   explicit decision at the render boundary (`Fraction::floorToInt`), as for the
   warp map. The reduced denominator cannot exceed `ppq · 10⁶`, decades away
   from `Fraction`'s int64 red line — asserted by `events_test`.

5. **A sequence is IMMUTABLE and sorted.** An edit builds a new one and the
   owner swaps the `shared_ptr` under its mutex; a freeze that already holds one
   keeps a coherent table for its whole duration (THREADING rule 2). The sort is
   STABLE by time, which is what lets a file round-trip byte-identically.

6. **Notes are stored WITH their length.** The SMF reader pairs note-on/note-off
   into one `NoteOn` carrying `duration`, so `stateAt` can answer "what is held
   here" without replaying the stream. Overlapping same-key notes pair **FIFO**.
   A note-on with velocity 0 IS a note-off. The open-note representation
   (duration ≤ 0 closed by a later `NoteOff`) is also honoured — a live capture
   produces it — so both survive `stateAt`.

7. **`twEventClipSet` mirrors the `ClipEntry` rules** of `tw/mix`
   (CLIP_MODEL.md, mix/CONTRACT.md inv. 1-3): identity is the caller's opaque
   key (the app passes the `SLink*`), never the resolved sequence; clips are
   handed CLIP-RELATIVE positions and the resolver's map does the domain
   translation; content is clamped to the clip window; every mutator returns the
   timeline extent it affected so the app's invalidation walk can scope itself.

8. **The event twin of the clip-end clamp is a SYNTHESISED note-off.** A note
   still held at a clip end — or at a loop wrap — is released there, flagged
   `twEventSynthesisedOff`. A note-on BEFORE the window is never re-issued; it
   reaches the consumer only through the chase set. Together these are what stop
   a note from either hanging forever or re-attacking on every page boundary.

9. **A boundary that falls exactly on a window START still owes its note-offs.**
   Windows are half-open, so a clip end or loop wrap at `startPos` belongs to
   THIS window at offset 0. Without that rule the previous window (which ended
   before the boundary) and this one (which begins a new segment where the note
   is not open) would both skip it and the note would hang. `collect` therefore
   accepts a clip whose end equals `startPos`.

10. **`collect` resolves each clip ONCE per call**, at the window start — one
    snapshot for the whole call, the same coherence rule `twView::resolve` gives
    the audio path (proposal 19 Inv-1). The clip LIST is likewise copied under
    the mutex before the walk.

11. **The map must report a RUN, not just a point** (`twEventMapping
    {seqPos, runFrames}`). `twTrackMix` never needed one because it asks a clip
    to render a page and the clip loops internally; events must be ENUMERATED,
    and an enumeration needs the extent of the affine segment. `runFrames ==
    INT64_MAX` means "never breaks". `twEventSlipMap` / `twEventLoopMap` are the
    two spellings the app needs; a null map is the identity.

12. **Metadata kinds never leave the sequence.** `Tempo`, `TimeSig`, `KeySig`,
    `Marker`, `Lyric`, `Text`, `Unknown`, … are score data; `collect` drops them
    and no plugin ever sees one. Whoever wants the score reads the sequence.

13. **Note ids are composed, not counted.** `twMakeNoteId(clipSlot,
    eventIndex)` (20 bits of index, 7 of clip slot) and
    `twNamespaceNoteId(id, sourceIndex)` (4 bits of source). Derived from the
    note's identity rather than from a per-call counter, because the note-off of
    a note chased in one page must carry the id its note-on carried in the
    previous one. Consequence, and the point of `twEventMerge`: two children
    playing the same key on the same channel are two overlapping notes to the
    instrument, never one truncated one. Beyond 16 merge sources or 128 clips
    per set the namespace wraps — logged once, never fatal; the audible cost is
    one truncated note.

14. **`twEventMerge` merges BY TIME and unions the chase.** Concatenating two
    sorted streams would hand a plugin an unsorted list, which every format
    forbids. A controller two sources both wrote resolves to the LAST source,
    deterministically. Sources are held by `shared_ptr` and the list is copied
    under the mutex for the duration of a collect, so a source removed
    mid-stream is simply absent from the next call and can never be destroyed
    under one in flight.

15. **The SMF writer has ONE canonical spelling**: explicit status bytes (never
    running status), minimal VLQ deltas, note-offs before note-ons at the same
    tick, end-of-track at the recorded end. Hence the gate: a file the writer
    AUTHORED round-trips byte-identically; a FOREIGN file round-trips to an
    equal event TABLE, not to equal bytes — normalising its running status,
    its event order at one tick and its non-minimal deltas is the point.

16. **Every meta event keeps its RAW payload**, understood or not: the ones we
    model also decode into `value`/`value2`, the ones we do not become
    `twEventKind::Unknown` with `paramId` = the meta type. A file from another
    program keeps everything it carried. `paramId` is the meta type on a
    metadata event, the controller number on a `ControlChange`, and the leading
    status byte (0xF0/0xF7) on a `Sysex`.

17. **SMPTE division and type 2 are REFUSED, not guessed at.** A negative
    division word silently misread as PPQ is a file playing at the wrong tempo;
    a type 2 file is independent sequences with no common timebase, which
    nothing in this app models.

18. **`twAutomationCurve` is a snapshot, not a live object.** Immutable, sorted,
    read at freeze time under the consumer's mutex. A segment's shape belongs to
    its LEFT point. `fillRamp` is an optimisation and never a second semantics —
    `events_test` asserts it equals n calls of `valueAt`.

## Threading

Everything here is either an immutable value (`twEventSeq`, `twAutomationCurve`,
`twTempoMap`) or a container with its own mutex (`twEventClipSet`,
`twEventMerge`). Plain C++, no Qt, no logging on a hot path beyond `TW_LOG*`.
Nothing here blocks on a render, allocates a page, or creates a demand: an event
source is model data (D1), so a consumer may call `collect` from a freeze
worker, from the MIDI-out scheduler thread, or from the UI thread.

`twEventClipSet::collect` allocates (the block's vectors). It is therefore for
freeze-time and scheduler-thread use, NOT for the RT audio callback — the same
rule that keeps rendering off the RT thread.

## How to test

`ctest -R events_test`. It gates the six committed SMF fixtures
(`events/tests/fixtures/`: two hand-crafted FOREIGN byte streams — a type 0 with
running status and velocity-0 note-offs, a type 1 with tempo/timesig/keysig/
lyric/marker meta and a sysex and an unmodelled meta — plus four authored by
this writer, including a 30 000-event file), `stateAt` against a brute-force
scan, the tempo map's exactness and denominator bound, the curve's closed forms,
the clip set's chase/clamp/loop/slip/synthesised-off behaviour, and the merge's
id namespacing.

`events_test --author-fixtures <dir>` rewrites the authored half of the corpus.
That is how those fixtures were produced; it is not a gate, and it lives in the
test so the authoring and the assertions cannot drift apart.

## Known debt

- **Constant tempo only.** `twTempoMap` holds one `usPerQuarter`; the API is
  shaped for segments (`usPerQuarterAt(TickPos)`, position-taking conversions)
  so proposal 37 changes this file and nothing else.
- No conductor/tempo-track import: a `Tempo` meta read from a file lands in the
  sequence as an event, and nothing yet feeds it to the tempo map.
- The SMF writer emits release velocity 0 always (SMF has nowhere to keep one;
  a non-zero value would break the byte-identity gate).
- `twEventBlock` allocates per collect. If the instrument path ever needs a
  per-page zero-allocation route, the block grows a reusable capacity — the API
  already takes it by reference for that reason.
- Nothing consumes the module yet; the seams (`twEventSource`,
  `twEventClipResolved`) are asserted by unit tests only, not end to end.
