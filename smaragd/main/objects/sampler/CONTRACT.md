# app/objects/sampler — CONTRACT

Purpose: NassauDigital's MODEL in the app (proposal 56 §4.4, §4.5, milestone
Q1; nassau-digital `docs/DESIGN.md` §2, §2.1, §9.2, §9.3, §12).
`SSamplerProgram` (one instance's program, an `SObject` child of its plugin
slot), the resolver hook a verb finds it through (`ssamplerslots.h`), and the
program verbs: `add-zone`, `remove-zone`, `add-layer`, `remove-layer`,
`set-zone`, `set-layer`, `set-sampler-program`, `select-zones`, and the three
inverse forms `reinsert-zone`, `reinsert-layer`, `restore-sampler-values`.

Public headers: app/objects/sampler/*.h

Depends on (engine): tw/core, tw/graph, **tw/sampler** (the editable program
`twSamplerProgramModel` and the snapshot it hands the instrument), **tw/plugins**
(the `twPlugin &` the instance initializer takes, reached as a `twSamplerHost`
by capability). App edges: {actions, model, persistence} — **the RANK of
`objects/midi`** (DESIGN §9.2; proposal 56 Q-6, answered by the author
2026-10-27). It does NOT depend on `objects/track`, although the slot it
configures lives there, and `objects/track` never depends on it (proposal 56
invariant 12). The SHELL joins the two (`main/shell/src/ssamplerwiring.cpp`).

Design: `plan/proposed/56_NASSAU_DIGITAL_SAMPLER.md` §4.4, §4.5, §11 (Q1).

## Invariants

1. **ONE MODEL, AND IT IS THE CORE'S** (A3, proposal 56 invariant 8). The
   truth is a `twSamplerProgramModel` — tw/sampler's pimpl over `nd::Program`.
   qbx never reads or writes a program attribute itself: every parameter, its
   range, the ALL scope, the clamp, the refusals and the XML are the core's.
   `SSamplerProgram` adds undo (through the verbs), persistence, signals and
   the hand-over to the instrument, nothing more.

2. **EVERY CHANGE GOES THROUGH `SSamplerProgram::edit()`**, which edits a COPY
   and commits it only when the edit succeeded: a refusal leaves the program
   bit-identical, however many core calls the edit made before refusing. A
   commit publishes ONE snapshot and emits `programChanged()`.

3. **AN EDIT REACHES EVERY INSTANCE, AND A REBUILT ONE IS BORN CURRENT**
   (proposal 56 invariants 3, 4). Each commit makes a new immutable
   `twSamplerProgram` snapshot and hands an initializer that installs it to
   the bound installer. The shell's installer sets it as the slot's instance
   initializer (future instances: a `setChannelCount()`, a `reloadPlugin()`, a
   rescan) and THEN applies it to the instances that exist through
   `SPluginSlot::applyToInstances()` (under the processor's mutex, then the
   path above the slot is staled). Never through the bare `plugin()` pointer.
   `publishedSerial()` is what every instance's
   `twSamplerHostStats::installedSerial` must equal.

4. **THE PROGRAM OBJECT EXISTS ONLY ONCE AN EDIT SUCCEEDED.** A NassauDigital
   slot has no `SSamplerProgram` until the first verb that changes it (the
   resolver creates it, `create == true`), and a refused `add-zone` creates
   none — the verb validates on a scratch copy first. A slot without one plays
   nothing, exactly as a slot with an empty program does, and saves nothing
   extra: every Q0 file and golden is unchanged. (Proposal 56 §4.4 said
   "created when a NassauDigital slot is inserted"; see §11 Q1.)

5. **UNDO IS ONE STEP PER VERB AND EXACT** (GATES Q1.1, C1.5). Every inverse
   restores ABSOLUTE values and is itself a registered verb, so it
   round-trips through XML:

   | forward | inverse | redo |
   |---|---|---|
   | `add-zone` / `add-layer` | `remove-zone` / `remove-layer` | RE-INSERTS the object the first apply made, same ids — a fresh add is never re-run |
   | `remove-zone` / `remove-layer` | `reinsert-zone` / `reinsert-layer` (the core's XML, at its index) | removes again |
   | `set-zone` / `set-layer` / `set-sampler-program` | `restore-sampler-values` (every replaced value of every target) | re-applies the edit, relative ones from the restored values |
   | `select-zones` | `select-zones mode="replace"` (the previous selection) | re-applies |

   qbx's history re-applies the FORWARD action on redo and re-uses one
   inverse instance for every undo (`SActionUndoCommand`), which is why the
   add verbs remember what they made: a redo that added afresh would mint new
   ids, and the redo of a later `set-zone zone="17"` would then refuse.

6. **IDS ARE NEVER REUSED** (DESIGN §2, Q1.q2). An undone `add-zone` leaves
   `nextId` where the add put it, so a new `add-zone` gets a new id. The undo
   is therefore exact in every zone, layer and parameter, and `nextId` is the
   one field that does not go back — which is what the gates assert
   (`equalsCaptured … ignoreNextId="1"` together with an explicit `nextId=`).

7. **EVERY REFUSAL IS ANNOUNCED AND MUTATES NOTHING**: `qWarning`,
   `<verb>: track T slot S: REFUSED: <why>`, the core's own reason where it is
   the core that refused.

8. **THE ROOT LIVES IN `SAction::pathRoot_`** (actions/CONTRACT.md inv. 6).
   A verb's `trackPath` may be root-qualified ("Drums:0"); a bare one resolves
   against `pathRoot()`. No class here keeps a root of its own; every inverse
   gets it from `applyPropagatingRoot()`.

9. **THE PROGRAM IS STORED VERBATIM, AS TEXT** (A4, GATES Q1.2):
   `<SSamplerProgram …><program><![CDATA[<NassauDigitalProgram …>…
   </NassauDigitalProgram>]]></program></SSamplerProgram>`. Character data,
   not nested elements, because the project file is read with QDom and QDom
   writes attributes SORTED BY NAME (measured): a program handed back through
   QDom would come back with its unknown attributes reordered, which the
   core's structural equality calls a different program. The core escapes
   `>` in everything it writes, so `]]>` cannot occur; the writer splits the
   section anyway if a kept text ever carries one.

10. **A PROGRAM THE CORE REFUSES ON LOAD IS KEPT, NOT LOST.** A value this
    core rejects (a newer build's polyphony, say) leaves the object
    `!isReadable()`: the text is written back on save character for character,
    the instrument plays the empty program, every edit is refused with the
    reason, and the load says so once. Discarding it would destroy the user's
    program on the next save.

11. **THE SELECTION IS MODEL STATE OF THE OBJECT, NOT OF THE PROGRAM**:
    `select-zones` is undoable (GATES Q1.1), changes no sound, publishes
    nothing, and is NOT saved. `selectedZones()` reports only ids that exist,
    in selection order, so a removed zone drops out and comes back with its
    undo. ASSUMPTION awaiting the author (proposal 56 §11 Q1).

## Value syntax (the front end's, DESIGN §2.1)

On a numeric parameter a leading `+`/`-` is a RELATIVE edit; on an enum or a
bool the core refuses it. A negative ABSOLUTE value is written with a leading
`=` (`pan="=-0.5"`) — ASSUMPTION awaiting the author, since DESIGN makes every
signed value relative and names no absolute spelling. A text parameter
(`src`, `name`) is always literal. `add-zone` / `add-layer` values are always
absolute (there is nothing to be relative to).

## Persistence and binding, step by step

- **Save**: the slot writes an `<SLink>` per model child (its existing loop);
  the program, a QObject child of the project like every `SObject`, writes its
  own top-level element.
- **Load**: the loader builds `<SSamplerProgram>` first (it has no links), then
  the slot, whose `instantiateFromDomElement` now attaches its `<SLink>`
  children and hands each to `SPluginSlot`'s child-adopted hook. The shell's
  hook binds an `SSamplerProgram` under a NassauDigital slot: the initializer
  is set before the track declares its width, so the instance is born with
  the program. A child the file lost costs only its link: `SPluginSlot` is a
  CONTAINER element kind since Q1.

## Gates

- `qxa.sampler_q1_verbs` — Q1.1 (each verb, one undo, the core's equality, a
  `differsFromCaptured` before every undo so the equality is not vacuous),
  Q1.q1 (ALL clamps per zone, undo restores each absolute value; relative edit
  on an enum and a bool refused), Q1.q2 (new id after an undone add), the
  refusals, the reinsert validation, and `installed="1"` before and after a
  2 -> 1 channel rebuild.
- `qxa.sampler_q1_persist` — Q1.2 first half: save, reload, structurally equal
  with no exception, a second save/reload a fixed point, bound after the
  reload, selection not saved, edit + undo after the reload.
- `qxa.sampler_q1_newer_program` — Q1.2 second half: a `version="2"` program
  with unknown attributes (in reverse alphabetical order) and elements at all
  three levels keeps them, in order, through a load, an edit, a save and a
  reload; a program this core refuses is kept verbatim; a lost program costs
  only its link.
- `action_roundtrip_test` — every verb and inverse, children included.

## Not gated / known limits

- `remove-plugin` + undo LOSES the program: the inverse `insert-plugin`
  rebuilds the slot from its descriptor and state blob, and the program is a
  model child, not part of the blob. Not a Q1 AC; recorded for a follow-up.
- `sampler-test-install` (testkit, Q0) bypasses the model: it sets its own
  instance initializer, and the next model edit on the same slot replaces it.
  Do not mix the two in one case.
- `SSamplerProgram::edit()`'s copy is defensive: every core edit is already
  all-or-nothing, so no sabotage of the copy can show in a gate.
- An older build (Q0 or earlier) opening a Q1 project does not know
  `<SSamplerProgram>`, so the slot's link to it dangles — and in that build
  `SPluginSlot` is still a PLAIN element kind, so it drops the slot too. A Q1
  or later build in the same position (a child it cannot load) keeps the
  slot.
