# app/objects/mixer — CONTRACT

Purpose: the root container object (SStdMixer: ordered tracks, asset
management) and SPluginChain (per-track plugin hosting model), with asset
actions (create/place/remove-asset, remove-asset-placement) and plugin
actions (insert/remove-plugin).

Public headers: app/objects/mixer/*.h

Depends on (engine): tw/core, tw/graph, tw/mix, tw/plugins. App edges: per
tools/check_layering.py.

Invariants:
1. getNTracks() counts TOP-LEVEL children only (reparenting under a group
   reduces it — assertions in tests rely on this).
2. Assets are objects owned by the project and placed via links; removing
   the last placement does not delete the asset (remove-asset does).
3. SPluginChain mirrors its model into tw/plugins chain components; wiring
   rebuilds go through rebuildWiring() after input changes.
4. Track selection is a SET with one distinguished PRIMARY, and both are
   held as QPointers — a removed track survives on the undo stack but dies
   when that command is discarded, so raw pointers here would dangle until
   the next click. Every mutator funnels through setSelectedTracks(), which
   is the one place that normalizes (no nulls, no duplicates), decides the
   primary (a member, else the last entry), and emits: selectedTracksChanged()
   for the set, then selectedTrackChanged() for the primary. The primary is
   what "the selected track" means everywhere else (activeLane(), the Track
   Detail dock); the SET only matters to the arranger, which decides what a
   gesture acts on (app/timeline/CONTRACT.md inv. 12). Selection is NOT
   serialized and is not an action — it is view state.

Self-registration (Phase 5): sstdmixer.cpp and spluginchain.cpp register
"SStdMixer" / "SPluginChain" with SProjectLoader from static initializers.

How to test: render_sawtooth_with_effects.qxa; asset actions have roundtrip
coverage via action_roundtrip_test.

Known debt: mixer→timeline/pluginui edges (getDetailEditWidget creates
views) — the renderer/editor factory extraction is the Phase 6 fix.

5. **The master's two components are reachable (proposal 21 L1b).**
   `masterMixComponent()` / `masterRewireComponent()` exist so the live plan
   builder can hand both to `twlive::checkMasterShape` — the "root(unarmed) +
   ring" split is exact only while the master is a UNITY SUM followed by an
   IDENTITY MAP, and that precondition is CHECKED on every plan build rather
   than assumed. `getRootComponent()` already handed out the rewire; the
   summing mixer had no accessor at all.

6. **`reconnectTracksToMixer` carries a SECOND, SEPARATE audibility term.**
   A live-owned lane gets a NULL input plug exactly the way an inaudible one
   does, but the predicate is `STrack::isLiveOwnedLane()` and not
   `ssolo::isLaneAudible` — see objects/track inv. 17 for why the two must not
   be folded. A top-level closure member is always the TOPMOST one by
   construction, which is why the rule here is simply "in the closure ⇒ null
   the plug".

7. **`pack-selection` PARTITIONS; `pack-clips` PACKS — and the split is not
   cosmetic.** `pack-clips` is single-lane BY CONTRACT (proposal 41 D8 /
   AC2.5: it refuses a two-lane selection naming both lanes, and
   `fragment_pack_multilane_refused.qxa` gates exactly that refusal). The
   arranger's "Pack clips into fragment" item, however, acts on a selection
   that routinely crosses lanes. Widening `pack-clips` with a mode flag would
   have blunted the one gate keeping a fragment single-lane, so the partition
   is a verb ABOVE it: `SPackSelectionAction` groups the paths by lane, packs
   every lane holding two or more of them, leaves a lane holding exactly one
   alone, and delegates each actual pack to an UNMODIFIED `SPackClipsAction`
   inside one `SCompositeAction` — the "a planner verb is a composite of
   primitives" shape `place-recording` and `duplicate-asset-here` already use,
   and what makes a multi-lane gesture ONE undo step.

   Two consequences worth knowing rather than rediscovering:

   - **The members are independent by construction**, which is why their
     order is free. Packing lane A removes clips from A and adds a placement
     to A, which cannot shift an index in lane B; no track is added or
     removed, so no LANE path moves either.
   - **The naming works only because each pack registers its asset before the
     next member runs.** `pack-selection` has no `name` attribute at all (one
     name cannot serve N lanes), so every lane takes `pack-clips`' generated
     first-unused "\<lane name\> N" — and two lanes with the SAME name come
     out as "Riff 1" and "Riff 2" purely because `generatePackName`'s
     `hasAsset()` sees the first registration. `fragment_pack_selection.qxa`
     names both packed lanes "Riff" on purpose to pin that.

   Refusing when NO lane holds two or more clips is deliberate: an
   applied-but-empty action would put a no-op on the undo stack.

   **The partition rule is ONE function**, `spackselection::groupByLane` /
   `packableLaneCount`, shared by the action and by the arranger menu item's
   enabled state. Those are the same question — "what would this pack?" — and
   proposal 41 M7 already paid for what happens when two call sites each
   derive the same rule separately (paint and hit-test disagreeing on z-order,
   green for two milestones).

8. **Every arrangement root owns exactly one MASTER LANE, and it is NOT a
   child link** (proposal 45 D1/D2). It is an ordinary `STrack` answering
   `systemRole() == Master`, minted by the constructor — so `masterLane()` is
   never null, no caller needs a "what if there is no master" branch, and every
   root created at runtime (`create-arrangement`, `extract-arrangement`) gets
   one for free. It is constructed HIDDEN.

   Keeping it out of `childLinks()` is FORCED, not chosen, and a change that
   moves it in breaks four things at once: every index path in every `.qxa`,
   every fixture and both goldens shifts by one; inv. 1 above becomes false;
   `reconnectTracksToMixer()` sums it alongside the tracks it is meant to
   PROCESS; and `ssolo::anySoloInTree` joins it to the solo set.

   So it is an OWNED REFERENCE LINK published through `ownedRefLinks()`, the
   shape `STrack` already uses for its plugin chain — *including* the reason
   that shape must be published at all (objects/track's header records the
   `~SProject` survivor-ordering crash that came of not publishing one).
   Dropping the ref in `~SStdMixer` is also what stops a removed arrangement
   leaving an orphan lane behind that would serialize forever: the lane is a Qt
   child of `SProject`, so nothing else would collect it.

9. **`masterLaneId` is refused when it does not name a master-roled track.**
   Serialization is regenerated from live objects and there is no
   unknown-attribute passthrough, so an OLDER build re-saving a project drops
   `systemRole=` and `masterLaneId=` alike; a file can legitimately come back
   naming a track that is now an ordinary one. Adopting it would make a user
   track the master — silently accepting clips, arming, and being summed twice.
   The constructor's own lane is kept, and the refusal is ANNOUNCED
   (`TW_LOGW`), never silent. Gate: `master_lane_bad_reference.qxa` over the
   committed fixture `tests/master_lane_role_lost.qxp`.

10. **The master lane is addressed by a NEGATIVE INDEX SENTINEL**, spelled
   `$master` (`app/model/sobjectpath.h`, proposal 45 D9). Both directions are
   load-bearing and the WRITE side is the one that would ship broken:
   `strackpath::pathOf()` walks `childLinks()`, so without its sentinel branch
   it answers `{}` for the master lane — which is also the address of the root
   itself, and every track head derives its commit address from it. The READ
   side fails CLOSED: an unparsable component becomes `SPATH_INVALID` rather
   than `QString::toInt()`'s 0, because before that a mistyped `"$mastr"`
   resolved silently to the FIRST USER TRACK.
