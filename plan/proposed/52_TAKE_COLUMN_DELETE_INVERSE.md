# Proposal 52 — a real inverse for deleting a take column

**Ticket:** QBX-146, step (a). **Status:** proposed. **Written:** 2026-10-11.

Source locations are given by function or symbol name, never by line number.

Step (b) (#235) stopped the data loss by **refusing** a delete it could not
undo. That is a blocked operation, not a fixed one: a user still cannot delete
a take column. This is what lifts the refusal.

## 1. What has to come back

`STakeStack` holds more than a clip does:

| State | Where |
|---|---|
| N takes, each an `SClipWindow` with its own window | `childCount()` / `takeAt()` |
| the audible take | `activeTake_`, `-1 = none` |
| **the comp map** | `compMap_`, a `twCompMap` |
| the column's own clip window | the `SClipWindow` over the stack |
| the shape: direct (`SLink -> STakeStack`) or wrapped (`SLink -> SCut -> STakeStack`) | both exist and are load-bearing |

An inverse that drops any of these is the bug this project already fixed once:
`SAddSampleAction` used to rebuild "a default cut over the whole wave, dropping
the slip anchor, trim, loop, stretch and pitch", so "a full-length unedited
clip would have come back silently, with no dialog to notice"
(`delete_clip_undo_restores.qxa`). Losing the comp map on undo is the same
failure in a different field.

## 2. Why the content cannot simply be kept

`SLink::~SLink()` ends with `object_.removeRef()`. Content is **refcounted**, so
deleting the clip drops one reference — and a take column built for one clip
has no other, so it dies with the clip. That is exactly why the old code had
nothing to rebuild from and fell through to `{true, nullptr}`.

So the inverse cannot hold a *reference* to the content. It has to hold a
**copy** made before the deletion.

## 3. The mechanism already exists

`stakes::cloneColumn( project, column )` builds a full `STakeStack` copy: it
clones every take through `SClipWindow::cloneWindowOver` and sets the active
take. `duplicate-clip` depends on it, so it is proven code, and the copy is
`new STakeStack( project )` — parented to the **project**, not to a lane, so it
exists without being a visible clip. That is precisely the shape a pending
inverse needs.

**It does not copy `compMap_`.** Verified: `compMap` appears nowhere in
`stakehelpers.cpp` or `sduplicateclipaction.cpp`. `setCompMap()` exists, so
copying it is one line.

### Do NOT fix that inside cloneColumn

Whether **duplicating** a comped column should carry the comp is a product
question, and nothing currently pins it — no `.qxa` asserts comp state after a
`duplicate-clip`. Changing `cloneColumn` would silently change
`duplicate-clip`'s behaviour as a side effect of an undo fix.

So this proposal clones and then calls `setCompMap()` **at the inverse's own
call site**, leaving `duplicate-clip` exactly as it is, and raises the
duplicate question separately (§6).

## 4. The design

A new live-only action, `SRestoreTakeColumnAction`:

- **holds** the cloned `STakeStack` plus the column's clip-window parameters
  (`srcStart`, `cutDuration`, `loopLength`, grain) and `restoreAtIndex`;
- **on apply**, links the clone into the lane, restores the window, and moves
  it to `restoreAtIndex` (QBX-149's rule: an inverse owes child order back);
- **on destruction without having applied**, deletes the clone. A pending
  inverse that is dropped when the undo stack is truncated must not leak a
  parented `STakeStack`.

`SRemoveSampleAction` then, for a take-column clip: clone, copy the comp map,
capture the window, delete the original, return this inverse — and the §(b)
refusal applies only to shapes still without one.

Live-only, like `SRestoreContainerClipAction`: `writeXml` empty, `readXml`
false. It carries a live `SObject*`, which no XML can express.

## 5. Two unknowns, to settle before writing code

Both are measurable, and neither is settled by reading:

1. **Recordings.** `SRecordingContent` is the other shape that falls through to
   the refusal. Whether it can be restored the same way depends on whether its
   content has another reference (an asset registry entry) and so survives the
   clip, or whether it too must be copied — and a recording's PCM is not
   obviously cheap to copy. Measure with a case that records, deletes, undoes.
   If it is not solvable cheaply, the refusal stays for recordings, and that
   must be stated rather than discovered.

2. **The wrapped shape** (`SLink -> SCut -> STakeStack`). The restore must
   rebuild the wrapper too, or undo silently converts a wrapped column into a
   direct one. `stakes::columnOfLink` resolves both shapes and is the only
   sanctioned way to get the column — never a bare `dynamic_cast`.

   **Measured while writing this, and the gate must know it:** a wrapped
   column does NOT survive a project load under normal settings. The migration
   pass converts it to the direct shape, so every committed wrapped fixture
   (`takestack_wrap_*.qxp`) is only observably wrapped with

       SMARAGD_TAKE_MIGRATE=off

   in the case's ENVIRONMENT, exactly as `take_seam_through_window.qxa` and
   `take_column_migrate.qxa` set it. Without that variable
   `assert-take-column shape="wrapped"` fails to apply and the case reads as
   broken rather than mis-configured — I hit precisely that running a probe by
   hand instead of through ctest.

   So the wrapped concern is narrower than it first looks: it applies to a
   column wrapped IN MEMORY (`stakes::wrapCutLinkIntoStack`, whose neighbour
   already warns `"take column left WRAPPED"`), not to one loaded from a file.
   Whether a user can reach that state without disabling migration is the
   thing to establish before deciding how much the inverse owes it.

## 6. Raised, not fixed here

**`duplicate-clip` drops the comp map**, and nothing says whether it should.
Either it is intended and a case should pin it, or it is a second instance of
the same class of bug. Worth its own ticket; this proposal deliberately does
not change it.

## 7. Gate

The case that must pass, and must fail before the change:

- build a take column with ≥3 takes, a non-default active take, **and a comp
  map that selects a different take in some span** — the comp is what a
  takes-only inverse silently loses;
- render, delete the clip, undo, render again, and assert the audio is
  identical **per span**, so an inverse that restored the takes but not the
  comp reads a different take in the comped span and fails;
- assert child order via `assert-clip-window`, per QBX-149;
- assert the shape via `assert-take-column shape=`, in **both** the direct and
  wrapped configurations;
- and the refusal log from (b) must be **absent** for this shape afterwards —
  `assert-log ... minCount="0" maxCount="0"` — or the delete is still being
  refused and the rest of the case is vacuous.

## 8. What this does not do

- It does not lift the refusal for any shape it has not given an inverse.
- It does not change `duplicate-clip` (§6).
- It does not address `SCompositeAction`'s inability to roll back a partially
  applied batch; (b) made that announced, and the design note there stands.
