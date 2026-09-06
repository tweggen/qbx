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

11. **THE MASTER LANE'S CHAIN IS IN THE SIGNAL PATH, AND THE ROOT COMPONENT IS
    STILL THE `twRewire`** (proposal 45 M2 / D3). `wireMasterChain()` puts the
    lane's `twPluginChain` and `twGainStage` between the bus sum and the
    rewire. What every consumer resolves — the master meter, `RenderSession`,
    `AudioEngine`, `twSpeaker`, all through
    `SProject::getRootComponent()->getRootComponent()` — does not move, and
    the master meter becomes post-master-FX by construction rather than by a
    second tap. Making the LANE's own rewire the project root was rejected: it
    moves what all of those resolve to, for nothing.

12. **THE LANE'S OWN `twTrackMix` AND `twRewire` ARE INERT AND MUST NOT BE
    "WIRED FOR CONSISTENCY"** (T5). An `STrack` builds trackmix → chain → gain
    → rewire; M2 re-points the CHAIN's input at the mixer's sum and takes the
    GAIN's output into the mixer's rewire. The lane has no clips (D6), so its
    trackmix has nothing to sum; a second rewire in the path would be a second
    page cache to invalidate.

13. **INVALIDATION MUST BE ISSUED AT THREE LEVELS, NOT TWO, AND THE THIRD IS
    SILENT WHEN MISSING** (D3a). `bumpRenderChainEpoch[Range]()` covers the bus
    mixers and the rewire; the master lane's chain and gain stage sit BETWEEN
    them and carry their own page caches. Without `bumpMasterChainEpoch[Range]`
    the rewire re-freezes, `fetchInputPage` serves the gain stage's still-valid
    stale page, and an ordinary clip or track edit is **inaudible** — no error,
    no log line. This is the commonest path in the app (every user has an empty
    master chain and edits clips). Measured: `mc_golden_stereo`'s own mute
    negative control flipped, its re-render coming back byte-identical to the
    unmuted golden.

14. **THE MIXER OWNS ITS MASTER LANE'S RENDER PATH, and says so through
    `SObject::renderPathOwner()`** (D11). `invalidateRenderPath()` walks down
    from the project root through `childLinks()`, and a system lane is
    deliberately not among them, so the walk does not find it and bumps only
    the lane's own caches — the fader moves and the render does not change.
    The mixer sets itself as the owner when it mints the lane and again when it
    adopts one from a file; a lane that lost that pointer would go silent in
    exactly this way. It is a DIFFERENT edge from invariant 13 and neither
    subsumes the other: 13 carries an ordinary track edit PAST the master, 14
    carries a master edit DOWN to the rewire. Each is watched failing with the
    other reverted.

15. **`remove-arrangement` PINS ITS ROOT, and its inverse re-registers THE SAME
    object** (proposal 45 AC1.6). Rebuilding an empty root by name loses every
    piece of state that hangs off the root rather than off a child link — which
    since M1 means the MASTER LANE and everything on it: inserts, fader,
    automation, hidden flag. The `childCount() > 0` refusal cannot cover that,
    because a system lane is deliberately not a child link. The pin lives on
    the REMOVE action, not on the inverse, so it survives an undo/redo cycle;
    the inverse releases it once the registry has taken its own reference, or
    the arrangement could never be destroyed again.

16. **THE CONDUCTOR LANE IS AN ORDINARY CHILD LINK OF THE MASTER LANE, NOT A
    SECOND SENTINEL** (proposal 45 M6 / D7). Its address is `{-1, 0}`
    (`$master,0`): one system step to the master, then a plain index. That is
    what makes it cost almost nothing — no new path machinery (`resolveByPath`
    already walks both halves), no new serialization (it is an `<SLink>` child
    of the master's own element, exactly like a nested user track), no new
    refusal (`STrack::acceptsClips()` has been false for every `systemRole`
    since M5) and no new hiding rule (`SObject::laneHiddenByDefault()` since
    M4). `conductorLane()` finds it BY ROLE rather than by index: index 0 is
    where `ensureConductorLane()` puts it, but a lookup that TRUSTED index 0
    would answer "the conductor" for whatever a later milestone parents to the
    master first.

    **It contributes no audio for TWO independent reasons**, and both are
    recorded because either alone would suffice and a later milestone that
    gives it real content must not lean on the wrong one: it carries no clips
    and no instrument, AND the master lane's own `twTrackMix` drives nothing at
    all (inv. 12) — so a CHILD of the master lane is summed by a component that
    reaches no output.

    `ensureConductorLane()` is idempotent and runs both at construction and
    after `adoptMasterLane()`, so a project written before M6 gains one on load.

17. **A SEND LANE IS WIRED BY `reconnectTracksToMixer` ITSELF, NEVER BESIDE IT**
    (proposal 45 M7 / D10, trap T3). This is the one thing about send lanes
    that had to be got right rather than merely added.

    That function sizes the bus mixer's input count FROM THE TRACK COUNT and
    rewires EVERY input, and it runs on every audibility, solo, mute and arm
    change. So a send lane wired into a spare input from anywhere else — at
    creation, at load, from a verb — is silently CLOBBERED by the next pass, and
    the symptom is a send that works until the user presses a button somewhere
    else entirely. The inputs are therefore RESERVED after the tracks and filled
    in the same loop, which is what makes the wiring idempotent under
    repetition.

    **Every send input is at UNITY, and that is load-bearing rather than a copy
    of the track line above it.** `twlive::checkMasterShape` walks
    `getNInputs()` and refuses the LINEAR master split on ANY non-unity input
    (D4a rule 2), so a send wired at anything else does not merely sound wrong —
    it drops live monitoring into the Closure path for every armed track in the
    project. A send's own level is its `twGainStage`, exactly as a track's is.

    MUTE is deliberately NOT applied here, unlike a track's: a system lane's
    mute is `twGainStage`'s AUDIO mute (inv. 18), so nulling the plug too would
    be the same silence twice and would make the ramp unreachable. SOLO cannot
    be applied at all — `ssolo::anySoloInTree` walks `childLinks()`, which a
    send lane is deliberately not in.

    **No audio assertion can see a clobbered send**, because in M7 nothing can
    feed one and a correctly wired lane is silent too. `assert-master-inputs`
    reads the input count and levels off the live mixer and is the only thing
    that bites; measured, with the send wiring removed, `send_lane_shape` PASSES
    and only `send_lane_survives_rewire` fails.

18. **A SYSTEM LANE'S MUTE IS THE GAIN STAGE'S AUDIO MUTE; ITS SOLO IS REFUSED**
    (proposal 45 M5 / AC5.4 / D6). Both follow from one fact — a system lane has
    no summing parent. A user track's mute and solo are applied BY ITS PARENT
    (this mixer nulls its input plug; a folder `STrack` mutes its clip entry),
    and neither reacts for a lane that is in nobody's `childLinks()`.

    So `set-track-mute` on one drives `twGainStage::setMuted()` — the ramped
    audio mute proposal 37 P3a built and left unwired for everything but a
    `self:Muted` lane — through `STrack::onTrackMuteChanged`, and it is HEARD.
    `set-track-solo` is REFUSED, because `ssolorules::anySoloInTree` walks
    exactly the child links a system lane is not among: the flag would be state
    no audibility rule can ever consult, which is the `SStdMixer::volume_`
    defect in a new field.

    **The invalidation is `invalidateRenderPath()`, never a bare
    `bumpRenderChainEpoch()`** — measured: with the epoch bump alone the muted
    render came back BYTE-IDENTICAL to the unmuted one. USER TRACKS ARE
    UNTOUCHED: their mute stays STRUCTURAL, which is what keeps a muted track's
    own capture full of its material so an asset windowing it is not silence.

19. **`remove-send-lane` PINS THE LANE AND ITS INVERSE RE-ADOPTS THAT OBJECT**
    (proposal 45 M7 follow-up), the `SRemoveTrackAction` idiom unchanged. The
    first version returned `add-send-lane` as its inverse, which re-created the
    lane BY NAME — so a removed send came back EMPTY on Ctrl-Z, losing its
    inserts, their state and its fader. "Nothing can hear a send's chain yet" is
    an argument about the AUDIO, not about the user's work.

    The pin is taken BEFORE `detachSendLane()` deletes the mixer's own `SLink`,
    so the refcount never touches zero; it lives on the FORWARD action, which
    the undo command reuses, so it survives redo. `SRestoreSendLaneAction` is
    deliberately NOT registered with the action registry — it holds a pointer to
    the pinning action, there is no pinned object in a file, and its `readXml()`
    refuses; `action_roundtrip_test` caught the first draft's registration
    immediately.

    Only the LAST send lane may be removed, and the refusal is announced: the
    sentinel `-2 - k` IS the address, so removing one from the middle shifts
    every lane after it and silently re-points every path that named one.

---

## Send ROUTING (proposal 47 M1-M4)

Proposal 45 M7 built a send lane that nothing could feed. These invariants are
what feeds it.

20. **A SEND LANE'S INPUT IS ITS OWN `twMixer`, and this mixer owns it.**
    `sendBuses_` is index-parallel to `sendLanes_`: bus k belongs to the lane
    the sentinel `-2 - k` addresses. It is the MASTER lane's shape one level
    down — `STrack::wireAsSendLane( sum )` takes the bus into the lane's
    plugin chain exactly as `wireAsMasterLane` takes the master sum into the
    master lane's.

    The bus is owned HERE rather than by the lane because the WIRING is owned
    here (inv. 21). A bus the lane owned would be a second place to wire it
    from, which is the whole failure inv. 21 exists to prevent.

21. **ALL send-bus wiring happens inside `reconnectTracksToMixer()`, through
    `rewireSendBuses()`, and nowhere else** — proposal 45's trap T3, re-paid.
    That pass rewires EVERY input on every audibility, solo, mute and arm
    change, so a bus wired from `adoptSendLane`, from a verb, or at load works
    until the user toggles a solo somewhere else entirely. Filling both halves
    in one pass is what makes the wiring idempotent under repetition, which is
    what "survives a rewire" means.

22. **`twMixer` REFUSES ZERO INPUTS by contract** (`if( n <= 0 ) return -2`),
    so "this lane has no taps" is ONE UNWIRED input and never a request for
    none. Measured with `setNInputs( 0 )`: the refusal went unhandled, the bus
    kept the plug it already had, and removing the last tap left the send
    sounding at the pre-removal level forever — a render read 0.34671 where
    0.115752 was due. The refusal is right; expressing the empty case is the
    caller's job.

23. **A LANE IS UNWIRED FROM ITS BUS WHILE THE BUS IS STILL ALIVE.**
    `detachSendLane` calls `STrack::unwireSendLane()` BEFORE dropping the bus,
    and drops the bus at the SAME index rather than leaving `rewireSendBuses()`
    to trim the tail.

    A component holds an input PLUG into its producer's latch. Dropping the bus
    first leaves the lane's plugin chain holding a plug into a destroyed
    `twMixer`, and the next `setInput()` dereferences that dead latch to detach
    it — **SEGFAULT**, found by `qxa.send_lane_remove_undo` (a proposal 45
    case) on the UNDO. Trimming only the tail is also silently wrong for a
    removal from the middle: the two lists stop being index-parallel and lane j
    inherits lane j+1's bus. The verb forbids that today; this code does not
    lean on it.

24. **A TAP EDIT NEEDS `invalidateRenderPath()`, and nothing lighter.**
    `sendRoutingChanged()` re-runs the pass and then invalidates. Without the
    invalidation a `set-send` is wired correctly and INAUDIBLE, because the
    render serves pages frozen before the edit — proposal 45 measured the
    identical shape for the master mute, where an epoch bump alone left the
    muted render byte-identical to the unmuted one.

    Bumping the send lanes' `bumpRenderChainEpoch()` and the buses'
    `bumpContentEpoch()` beside it was tried and REMOVED: each was ablated
    separately and the gate passed without either, so both were code no
    sabotage could bite. 45's rule reached independently — **invalidate, never
    bump**.

25. **A SOURCE THAT IS INAUDIBLE OR LIVE-OWNED FEEDS NOTHING, and those are
    TWO terms.** Audibility is `ssolo::isLaneAudible`, resolved once per pass
    and never re-spelled (`main/timeline/CONTRACT.md` inv. 10 records that two
    local copies of that rule are how the meter and the ear came to disagree
    about a nested lane). Mute therefore kills a PRE-fader send too, which is
    not derivable from the tap point and is a decision: every reference DAW
    silences sends on mute.

    `isLiveOwnedLane()` is the SECOND term and is never folded into the first
    (proposal 21 L1b): a live-owned track is still audible in every other
    sense. **A LIVE LANE DOES NOT FEED A SEND** (proposal 47 D9, requester
    decision): it is excluded from the frozen sum and rendered by the pump, so
    the gain-stage pages a tap would read are not being produced at all. The
    monitored signal is therefore DRY, and that is the behaviour rather than a
    defect. Gated by `qxa.send_live_lane_does_not_feed` — at the WIRING, because
    a render SUSPENDS every live lane and by the time there is audio the
    condition is gone.

26. **A SEND CYCLE IS BROKEN HERE, not only refused at the verb.**
    `add-send` refuses one, but the LOADER does not go through the verb —
    `<sends>` is read by `SObject::readSends()`, so a hand-edited or foreign
    `.qxp` carries whatever it likes. `rewireSendBuses()` accepts an edge only
    when it does not close a cycle among the edges ALREADY accepted, walking
    lanes in index (= file) order, and ANNOUNCES every edge it drops.

    That drops exactly ONE edge of a cycle. Asking the FULL tap graph instead
    finds both edges cyclic and drops BOTH, silencing a lane that has a good
    reason to sound.

    **IT IS NOT ABOUT A HANG, and proposal 47 D6 said it was.** D6 predicted
    the page scheduler would leave two nodes each waiting on the other until
    the watchdog killed the render. MEASURED against `tests/send_cycle.qxp`: a
    cyclic project renders in about one second, completes, and is
    byte-identical across `SMARAGD_REVAL_WORKERS` 1 / 4 / 8 over six runs —
    `FreezeContext::isComponentInStack` breaks the recursion at render time.
    The refusal is right for a weaker and truer reason: the audio a broken
    cycle produces is whatever that break happens to yield.

27. **A SEND BUS'S INPUT LEVELS MAY BE NON-UNITY; THE MASTER SUM'S MAY NOT.**
    The send level IS the bus's own per-input level (`twMixer::setInputLevel`),
    so a send needs no new DSP anywhere. `twlive::checkMasterShape` inspects
    the MASTER mixer and never a send bus, so a send at −6 dB does not disturb
    live monitoring — unlike a non-unity MASTER input, which drops it into the
    Closure path for every armed track (45 D4a rule 2).
