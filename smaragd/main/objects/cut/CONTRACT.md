# app/objects/cut — CONTRACT

Purpose: the clip window object. SCut (startOffset/duration/loopLength/grain
window over any content, reader-chain ownership, container capture,
snapshot-based audio access), its inline renderer, and the window actions:
split-clip, unsplit-clip, resize-clip, duplicate-clip.

Public headers: app/objects/cut/*.h

Depends on (engine): tw/core, tw/graph, tw/pages, tw/schedule, tw/sources.
App edges: per tools/check_layering.py.

Invariants (normative detail: CLIP_MODEL.md, POSITION_DOMAINS.md):
1. Audio threads read via getSnapshot() only; window params change under
   mutex() then invalidate (drag path queues events, applies after).
2. mapTimelineToComponentPos mirrors seekTo exactly: both consume the ONE
   shared map, SCut::clipToReaderMap (proposal 18 Phase 4) — identity for
   looping, +startOffset otherwise (NO stretch scaling, see invariant 4).
   ensureReader() runs first. The preview consumes clipToSourceMap the
   same way; never hand-roll a second mapping.
3. rebuildReader is chain-descriptor-checked: plain trim/slip REUSES the
   reader; grain/loop changes mint a new one (fresh page cache).
4. The slip anchor is stored SOURCE-authoritative (proposal 18 Phase 3):
   srcStart_ is an exact Fraction, invariant under stretch edits; the
   warped-domain startOffset is DERIVED (floor(srcStart * stretch), the
   single render-boundary rounding). stretch is an exact Fraction born as
   a ratio of integer frame counts, denominator-capped at creation.
   cutDuration_/loopLength_ are integer timeline lengths. Positions are
   domain-typed (twdomains.h). Split arithmetic is exact rational and
   relies on the second cut inheriting grain params RAW (setGrainParams
   would preserve-span-rescale the duration).
5. Container-backed cuts capture via freezePage of the content root;
   arrangementChanged drops the capture transparently.

Self-registration (Phase 5): scut.cpp registers "SCut" with
SProjectLoader from a static initializer.

How to test: render_split_slip_offset.qxa (THE regression),
render_sawtooth_clipped_section.qxa, grain_*.qxa.

Known debt: loop tiling of container captures deferred; FIXME bounds check
in seekTo.

## Take stacks (proposal 17, phase 1)

STakeStack (stakestack.h) is the column of parallel takes — see the "Take
stacks" section of CLIP_MODEL.md for the model rules. Verbs: add-take
(wraps a plain cut on first use), remove-take (collapses at 1 take),
select-take; split-clip/resize-clip/unsplit are stack-aware (length ops
write through to every take, slip targets one take via `take`).

Notes:
- The stack serves a private silent component while no take is active
  (STakeSilence in stakestack.cpp) — objects/cut may not include tw/mix,
  so no twRewire here.
- Wrap/collapse preserve the lane child index (moveChildToIndex) so
  recorded action paths and inverses stay valid — do not "simplify" this
  away.
- Undo fidelity limit: removing a take whose content is not file-backed
  (container asset) is applied but NOT undoable (inverse = null); a
  collapse triggered with activeTake == -1 resurfaces the remaining take
  audible (plain cuts cannot be inaudible).

How to test: takes_comping.qxa (audibility, comping per column, undo),
takes_serialize_roundtrip.qxa (loader registration, per-column activeTake
persistence incl. -1).

Phase 2 verbs (recording): place-clip (path-addressed windowed plain-cut
placement; inverse SUnplaceClipAction), place-recording (plans the file
span against the lane's columns — takes for covered columns, place-clips
for gaps, straddling columns untouched — applied atomically via
SCompositeAction from app/actions). Test:
takes_recording_placement.qxa.

Phase 4 (edit groups): split-clip/resize-clip/select-take (and move-clip in
objects/track) carry a `broadcast` attribute — a grouped anchor
(SObject::editGroup, helpers in app/model/seditgroups.h) fans out to every
member's corresponding clip (positional: same startTime+duration) as one
SCompositeAction; fan-out children carry broadcast=0. select-take comps the
same take INDEX; resize syncs the slip to the corresponding take (an
active-take anchor resolves its index first). Test:
takes_group_broadcast.qxa.
