# app/objects/midi — CONTRACT

Purpose: the EVENT clip. `SMidiSequence` (the content: a sorted table of
`SEvent` in musical ticks, persisted inline), `SMidiCut` (the window: a
tick-native `SClipWindow` over it), their inline renderers, and the event
verbs — `insert-midi-clip`, `import-midi-file`, `export-midi-file`,
`add-note`, `remove-note`, `set-notes`, `add-event`, `remove-event`,
`set-events`, `quantize-notes`, `set-midi-cut`, `set-tempo`,
`set-link-timebase`.

Public headers: app/objects/midi/*.h

Depends on (engine): tw/core, tw/graph, **tw/events**. App edges: {actions,
model, persistence} — the RANK of `objects/cut`, deliberately: this is a
second window/content pair, not a layer above one, and it must stay
independent of the audio window. **`objects/track` must NOT depend on this
slice**: the track consults MIDI-ness only through `SObject::contentKind()`
and `SObject::resolveEventClip()` (design 37 §3.5).

Design: `plan/proposed/37_MIDI_INSTRUMENTS_AUTOMATION.md` §3.1–§3.4 (D2, D3).
Normative background: CLIP_MODEL.md, POSITION_DOMAINS.md (rule 7),
`tw303a/events/CONTRACT.md`.

## Invariants

1. **THE WINDOW IS TICK-NATIVE, AND THE CONVERSION HAPPENS EXACTLY ONCE.**
   `SMidiSequence` stores event times in musical ticks at PPQ 960;
   `SMidiCut` stores `srcStartTicks` / `lengthTicks` / `loopTicks` as exact
   `Fraction` ticks plus an exact `rate`. Every side the track, the mixer, a
   gesture or the engine sees speaks FRAMES, and every one of those values is
   derived inside `SMidiCut::rebuild_nolock()` — the single site that
   multiplies by the tempo map's frames-per-tick and floors once, exactly as
   `SCut` derives its warped `startOffset` from `srcStart * stretch`. No
   caller outside this class may convert ticks to frames; two callers
   converting independently is how a rounding difference becomes an
   off-by-one clip edge.

2. **A TEMPO OR SAMPLE-RATE CHANGE REBUILDS, IT DOES NOT MIGRATE.** `SMidiCut`
   listens to `SProject::bpmTempoChanged` and `sampleRateChanged` and re-derives
   everything frame-facing. This is why an event clip needs no `durationSec`
   migration and both classes override the base one away: ticks are rate-free
   (D2). A file's `lengthTicks` / `srcStartTicks` are the authority and are read
   AFTER the base class's attribute pass, so they discard whatever it computed.

3. **SPLIT IS NON-DESTRUCTIVE.** The generic `split-clip` narrows the WINDOW
   through `SClipWindow::setWindowExact`; the shared sequence is never edited
   (D3). Consequences that are gated, not incidental: a note straddling the
   split keeps its ORIGINAL duration in the head, the head's window end
   synthesises the note-off (`twEventClipSet`, events/CONTRACT inv. 8), and the
   tail never re-attacks it because a note-on before the window reaches a
   consumer only through the chase set. A content-editing `split-notes-at` is a
   later verb and would be a different thing.

4. **THERE IS ONE MUTATOR AND THEREFORE ONE INVERSE.**
   `SMidiSequence::setEvents` takes an absolute new table; every content verb
   goes through it and hands back a `set-events` carrying the previous table.
   "The inverse is the previous state" is then true by construction rather
   than by discipline, which is what makes a piano-roll drag of N notes one
   undo step and `quantize-notes` its own inverse.

5. **`set-tempo` IS THE ONLY TEMPO WRITE.** It writes the project's
   `twTempoMap` and then re-derives `startTime` for every `timebase=beats`
   link in the project — walking the PROJECT's own children, which reaches
   nested containers, take-stack lanes and unplaced assets alike and reaches
   each link exactly once. The ticks never change, so repeated tempo edits
   cannot drift; a frames-only rescale would lose a frame per edit. Being an
   action is also what keeps undo exact by LIFO (D2). Gate: the AC6 grep —
   `grep -rn "bpmTempo_ =\|setBPMTempo(" main/` must hit only this verb and
   the loader.

6. **PER-CLIP MODIFIERS ARE APPLIED WHERE THE CONTENT IS RE-EXPRESSED**, never
   written back. `transpose`, `velocityScale` and `channelOverride` are folded
   in by `smidievents::buildSeq` while building the frame-domain sequence, so
   two clips of one sequence can transpose differently and the sequence stays
   the material the user recorded.

7. **THE SEQUENCE'S FRAME-DOMAIN ZERO IS THE CONTENT'S ZERO**, not the
   window's. A slip edit therefore changes only the clip set's MAP
   (`twEventSlipMap` / `twEventLoopMap`), never the table, so slipping costs no
   rebuild — the event twin of POSITION_DOMAINS rule 4 (source-keyed page
   caches survive a slip).

8. **AN EVENT OBJECT SERVES SILENCE, NOT NULL.** `SMidiSequence::
   getRootComponent()` returns a private silence component (the `STakeSilence`
   trick). A null component makes `twView::getComponent()` warn once per freeze
   for the lifetime of the project (fact M5), which is noise, not information —
   and the absence of that line is the only observable difference between "the
   clip was routed into the event clip set" and "it was routed into the track
   mix as a silent clip". (Written as "the bus mixers"; a track has had exactly
   one twTrackMix since proposal 36 B4.) Gated by `midi_clip_render_silent`.

9. **UNKNOWN EVENT KINDS ROUND-TRIP VERBATIM.** An `<e>` whose `k` this build
   does not recognise keeps its spelling AND its whole attribute map, and every
   metadata event keeps its RAW payload whether or not we decode it. A file
   written by a newer build — or imported from another program — survives a
   load/save here untouched. The `<events>` payload is sorted on write (diff
   stability, proposal 32) and is the sanctioned non-`SLink` child: the loader
   ignores it for ordering.

10. **SERIALIZE OUT OF THE LOCK.** `serializeSelfAttributes` snapshots under
    `mutex()` and writes afterwards. The base class's serializer calls
    `getDuration()`, which takes the same mutex, and `std::mutex` is not
    recursive — holding it across that call is a self-deadlock, and a silent
    one: the save simply never finishes. This cost a debugging session; do not
    re-introduce it in a new attribute.

11. **THE APP MODEL AND THE ENGINE SHARE ONE EVENT VOCABULARY.** `SEvent`
    carries `twEventKind` / channel / key / `value` / `value2` / `paramId` /
    `duration` — the engine's own field names, not a parallel set — plus the
    two things an arena-indexed struct cannot hold (the payload as a
    `QByteArray`, and the verbatim attribute map of an unknown kind). A model
    record that renamed the fields would need a translation table that could
    disagree with itself (events/CONTRACT inv. 1).

## Threading

Both classes follow THREADING rule 2: state under `mutex()`, an IMMUTABLE
snapshot swapped whole (`tickSnapshot()`, `getSnapshot()`), so a consumer that
already holds one keeps a coherent view for its whole use. Signals are emitted
OUTSIDE the lock — `SMidiSequence::setEvents` releases before
`eventsChanged`, which reaches `SMidiCut::onContentEventsChanged`, which takes
the cut's lock. The only cross-object lock is cut→sequence (inside
`rebuild_nolock`), and it is one-directional; the emit-outside-the-lock rule is
what keeps it that way.

## How to test

`midi_clip_roundtrip` (import → save → load → export, byte-identical),
`midi_clip_edit_verbs` (every verb + explicit undo, the non-destructive split,
rate/slip/loop, take homogeneity), `midi_clip_tempo_remap` (`set-tempo` moves
MIDI and not audio), `midi_clip_render_silent` (silence, and no `twView`
warning), `midi_folder_feed` (the track feed — that one lives in
`objects/track`'s territory but is only observable with clips from here).
`midi_fixture_authoring` regenerates `tests/midi_multitrack.mid`.

## Known debt

- **Loop tiling is not drawn.** `SMidiCutRendererInline` paints the window
  once; a looping clip shows its first segment. `SCutRendererInline` has the
  loop-segment machinery, and lifting it into a shared helper is the fix.
- **No event editor.** Notes can only be edited through the verbs; the piano
  roll is P4.
- `import-midi-file` keeps every tempo event as metadata and applies only the
  FIRST one, to the tempo map, and only into an empty project. Timing is
  constant until tempo segments (proposal 37); the import warns when a file
  carries more than one tempo.
- `mode="channels"` is not implemented (only `tracks` and `merged`).
- `SMidiSequenceRendererInline` draws nothing: a bare sequence placed without
  a window has no frame mapping of its own, and inventing a tempo in a painter
  would be worse than an empty rectangle. Every real placement is an
  `SMidiCut`.

## Event automation lanes (proposal 37 P5, design D5)

**`cut:VelocityScale` and `cut:Transpose` are applied when the SNAPSHOT is
built, not at freeze time.** They change the EVENTS, and the events are what
every consumer reads — the instrument slot, the MIDI-out pump, the piano roll —
so `smidievents::buildSeq()` takes the two curves and evaluates each at the
event's own CLIP-RELATIVE frame, which is the domain `twEvent::time` is already
in by that point. They compose with the static modifiers the way Trim always
does: the transpose lane ADDS semitones, the velocity lane MULTIPLIES.

`SMidiCut::onAutomationChanged()` therefore REBUILDS (under the cut's own mutex,
so a rebuild and an edit cannot interleave) and then invalidates open-endedly:
the consumer of an event stream is class-1, so an event change is never bounded
on the right (design F9). `cut:Gain` is inherited from `SObject` and means
nothing on an event clip — the mix has no audio page of this cut's to scale.

## The recording verbs (proposal 21 L4 = 37 P8b)

`add-midi-take` and `place-midi-recording` live HERE and not in `objects/cut`,
and they reach a take column through the generic seam on `SObject`
(`windowTakeCount` / `activeWindowTakeIndex` / `insertWindowTake` /
`removeWindowTake` / `setActiveWindowTake`) plus `SClipWindow`'s registered
wrap/collapse factory. `STakeStack` has been window-typed since proposal 37 D8b,
so nothing about a column of takes is audio-specific — but the class lives in
`objects/cut`, and this slice sits at the SAME RANK and must not depend on it.
Same rule, same reason, as `objects/track` consulting MIDI-ness only through
`contentKind()` and `resolveEventClip()`.

**`add-take` could not have been widened.** It addresses a FILE and seeds grain
params; an event take has neither. `SRemoveTakeAction` is the same story from
the other end: it builds its inverse from an `SExternFile` path, so removing an
event take through it would be a NON-UNDOABLE removal — a lost take, not an
undo. Hence `remove-midi-take`, which captures the take's whole event table when
it runs so its own inverse restores the notes.

**`place-midi-recording` is `place-recording`'s planner shape**, and the two
stay parallel deliberately: one call per track per pass, the verb decides by
itself whether the pass is a take, a merge, a replace or a new clip. Three
things about it are worth knowing:

- The `<n>` / `<e>` children are in the PASS WINDOW's ticks (zero = `timePos`),
  not the target sequence's, which is what lets a recorder emit one identical
  table per loop pass. The re-base asks the WINDOW for its map
  (`timelineToSourceExact`, `stretchOrRate`) rather than computing slip and rate
  itself — two callers converting independently is how a rounding difference
  becomes an off-by-one note.
- Overdub and replace both END as one absolute `set-events` on the column's
  active take, which is the universal inverse every event edit already uses
  (37 §3.4). So undo is the previous table, verbatim, whichever mode ran.
  Replace drops only NOTES inside the window; CCs, and everything outside it,
  survive — that is what makes it a replace rather than a clear.
- The input quantise is a `quantize-notes` INSIDE this verb's own composite, so
  one undo entry covers the placement and its grid together. Its clip path is
  computed at PLAN time (the column's index, or `lane->childCount()` for the
  fresh insert, which appends) — the same discipline `place-recording` uses.

Events written by both verbs are `<e .../>` children, never `<n .../>`: a
recorded pass carries CCs, bends and pressure as well as notes, and the note
spelling would read every one of them back as a `NoteOn`. `readEventChildren`
accepts both, so a hand-written case may still use `<n>` for a notes-only pass.

An `add-midi-take` whose target column holds AUDIO takes is REJECTED
(`STakeStack::insertTake`'s homogeneity rule), and so is one whose target
placement is not an event clip at all — checked BEFORE the wrap, because
wrapping an audio cut into a column the insert is then bound to refuse would
have moved the user's clip for nothing.
