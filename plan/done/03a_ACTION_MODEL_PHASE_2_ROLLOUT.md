# Strategy: SAction Phase 2 — First Real Actions

## Objective

Implement the first production-ready SAction subclasses to validate the action cycle (apply/inverse/undo/save/load) with real user interactions. Intentionally sequence them to ensure observability of correctness.

## Rationale for sequencing

**Phase 2a precedes Phase 2b because:** Testing volume changes without track content means amplifying silence — the action mechanism works (UI updates, undo functions), but you have zero way to verify the audio path is correct. With `SAddSampleAction` first, phase 2b has testable audio content, so volume changes are audibly observable.

### Phase 2a: `SAddSampleAction` hardened

**Goal:** Polish the existing Phase 1 test action into production-ready code.

**What lands:**

| Item | Purpose |
|------|---------|
| `SAddSampleAction` (refined) | Proper inverse (`SRemoveSampleAction`), error handling (track deleted between submit/apply), save/load of pending sample imports |
| `SRemoveSampleAction` | Inverse of add; removes sample at index with proper undo |
| `SProject` serialization | `<pending-actions>` block in XML preserves unapplied sample imports across save/load |
| Integration test | Add sample to track, play, verify audio cursor advances and sound is audible |

**What gets validated:**

- ✓ `apply()` → `SApplyResult{applied, inverse}` cycle works
- ✓ Inverse construction from pre-mutation state (captured sample index)
- ✓ `writeXml`/`readXml` round-trip for both actions
- ✓ `SActionHistory::undo()` reverses the sample import
- ✓ Save during playback preserves pending sample imports (no blocking on queue drain)
- ✓ Error handling: track deleted between submit and apply
- ✓ **Audible verification:** hear the sample play back

**Non-goals:**
- Merge logic (not applicable to sample import; you don't rapidly add the same sample 50 times)
- Merge key / coalescing
- Rapid-fire stress testing

**Success criteria:**

1. Add sample via action, hear it in playback
2. Ctrl+Z immediately after import undoes it (sample gone)
3. Ctrl+Z during playback (sample still in flight) cancels instantly
4. Save during playback with pending import; reload shows the sample
5. Delete track, try to import sample to it; apply fails cleanly, error logged

---

### Phase 2b: `SSetTrackVolumeAction` with merge

**Goal:** Test merge coalescing and undo latency under rapid-fire interaction.

**What lands:**

| Item | Purpose |
|------|---------|
| `SSetTrackVolumeAction` | Captures `trackIdx`, `newVolume`; inverse captures the pre-mutation volume |
| `mergeKey()` / `mergeWith()` | Same track → absorb newer volume; 50 drag events → 1 engine apply |
| UI integration | Volume slider on track (or use Test menu to submit rapid actions) |
| Stress test | Volume drag during playback; hear level change in real-time; undo cascade works correctly |

**What gets validated:**

- ✓ All of phase 2a (apply/inverse/undo/save/load)
- ✓ **Merge coalescing:** rapid volume drags collapse via `mergeKey` + `mergeWith`
- ✓ **Undo latency:** `SActionHistory::undo()` → `tryCancel()` is instant if in-flight
- ✓ **Undo cascade:** Ctrl+Z twenty times correctly reverses the last 20 volume changes (mix of queued and already-applied)
- ✓ **Audible verification:** drag volume, hear sample get louder/quieter in real-time (only works because phase 2a added audio content)
- ✓ Save during a volume drag; reload and the sample is at the pending volume level
- ✓ Error handling: delete track, drag its volume; apply fails, error logged, UI stays consistent

**Non-goals:**
- Selection as state (phase 3)
- Async engine-thread drain (phase 2b is still synchronous; async is phase 2c)
- Inventory of other mutations (phase 3)

**Success criteria:**

1. Drag volume slider while sample playing; hear level change in real-time with no audio glitch
2. Rapid drag (50 events in 1 second); queue merges to 1 engine apply; no stutter
3. Ctrl+Z immediately after drag undoes it even while audio is playing (near-zero latency)
4. Ctrl+Z during burst of 20 rapid clicks correctly unwinds them (in-flight cancels are instant; already-applied unwind via undo stack)
5. Save during active drag; reload; sample is at the pending volume level
6. Delete track, drag its volume, Ctrl+S; apply fails when drained, error logged, UI reconciles
7. **Audible:** sample remains audible, pitch/speed unchanged, only level changes (confirms volume is wired into the audio path)

---

## Acceptance criteria for phases 2a+2b combined

These come from the original proposal and are now sequenced to be testable:

1. ✓ **Sample import produces audible audio** (2a) + **volume drag changes level** (2b) with no audio glitch under load
2. ✓ **Ctrl+Z immediately after action** (sample import or volume drag) undoes it even when audio is currently playing
3. ✓ **Burst undo:** Ctrl+Z during 20 rapid clicks correctly unwinds, whether each was queued or already applied
4. ✓ **Save during playback** completes without dropouts; reload restores state including pending-but-unapplied actions
5. ✓ **Error handling:** precondition failures (target gone, validation error) are logged and surfaced; UI reconciles

---

## Why this order matters

**2a first:**
- Lowest complexity (no merge, no rapid-fire stress)
- Creates testable audio content
- Validates the basic action cycle (apply/inverse/undo/save/load)
- Gives you a known-working baseline before adding merge logic

**2b after:**
- Merge logic is now testable and you can hear if it works
- Volume changes are audible (because 2a added content)
- Undo latency under load is measurable (rapid drags)
- Full stress test without introducing multiple unknowns at once

If you tried 2b first without 2a, you'd test merge correctly, but you'd have no way to know if volume changes actually affect the audio rendering — that bug would hide until later when you add sample content.

---

## Timeline

- **Phase 2a:** Refine `SAddSampleAction`, implement `SRemoveSampleAction`, wire UI test, audible verification. ~1–2 days.
- **Phase 2b:** `SSetTrackVolumeAction` + merge + stress test + audible verification. ~1–2 days.

---

## Next steps

1. Review and sign off on this sequencing
2. Start phase 2a: polish the sample action
3. Implement removal inverse; wire to undo
4. Verify: add sample, hear it, undo it, save/load it
5. Proceed to phase 2b
