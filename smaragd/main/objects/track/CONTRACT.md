# app/objects/track — CONTRACT

Purpose: the track object. STrack (ONE twTrackMix of the project's channel
width — the per-bus mixers retired at proposal 36 B4, see inv. 9 — clip
synchronization to it, MapPosFn wiring), its renderers, strackpath (path
resolution
used by ALL placement actions), and track/placement actions: add/remove/
move/reparent/restore-track, set-track-volume, move-clip, remove-clip.

Public headers: app/objects/track/*.h

Depends on (engine): tw/core, tw/graph, tw/mix, tw/plugins, tw/events. App
edges: per tools/check_layering.py. NOT app/objects/midi, deliberately: the
track consults MIDI-ness only through SObject::contentKind() and
SObject::resolveEventClip() (proposal 37 3.5).

Invariants (normative detail: CLIP_MODEL.md):
1. Clips are keyed by SLink* in every insertClip/updateClip/removeClip —
   never by component.
2. Every insertClip passes the mapPosFn
   (child.getSObject().mapTimelineToComponentPos) — a clip inserted without
   it plays from source position 0.
3. trackChildDurationChanged: sender() is the SObject — resolve links by
   scanning childLinks(); update EVERY link referencing the sender.
4. trackChildWasMoved: sender() IS the SLink (startTimeChanged is a link
   signal).
5. strackpath resolution: comma-separated child indices from the root
   mixer; reparent guards against self/descendant cycles.

5b. EVENT CHILDREN GO INTO THE EVENT CLIP SET, NOT THE TRACK MIX (proposal
   36 3.2). A child whose contentKind() is Event is inserted into the track's
   ONE twEventClipSet - same slots, same SLink* key rule as the track mix. (The
   sentence here used to contrast "one set per track rather than one per bus";
   since B4 a track has one mix, so there is no longer a plural to contrast
   with, and the event set's shape is simply the same as the mix's.) It is
   never inserted as a ClipEntry: a MIDI clip has no page to freeze, and doing
   so would cost a dummy freeze per page per clip plus a twView warning per
   freeze (fact M5/F12). The resolver is the generic
   SObject::resolveEventClip(), resolved ONCE per collect at the window start -
   the same coherence rule twView::resolve gives the audio path.
   Every event edit invalidates [a, INFINITY): the consumer of an event stream
   is class-1 (a synth voice carries state across pages), so a change at one
   position can
   be heard at any later one (F9). The app-side walk is the ONLY path that
   carries a clip change up to the root (F13), so touchClip alone is not enough
   - invalidateRenderPathRange must follow it.

5c. THE FEED IS REBUILT ON EVERY READ (3.2.1). STrack::eventFeed() is a stable
   twEventMerge object whose SOURCE LIST is recomputed per call: its own clip
   set plus the feed of every child track that bubbles events up (no instrument
   slot and no MIDI-out, unless the serialized midiRouting says otherwise), with
   muted and solo-excluded children contributing nothing under the shared
   ssolorules.h resolution. Rebuilding rather than dirty-flagging is deliberate:
   SOLO IS GLOBAL, so a flag would have to be poked from anywhere in the
   project, which is exactly the coupling ssolorules.h exists to avoid. The
   merge object identity is stable, so a consumer may hold it.

6. A track's plugin chain is referenced by the ATTRIBUTE pluginChainId, and
   adopted LATE (proposal 08 M4). SPluginChain is not an SLink child of the
   track — SObject::childEvent treats every child link as a clip placement — so
   the reference cannot use the loader's <SLink objectId> ordering and STrack
   registers a SProjectLoader::deferResolve() lambda instead, which runs when
   the object dictionary is complete. adoptPluginChain() then does what a
   loaded chain's slotInserted signals never could (they were emitted while the
   chain had no owner): CHANNEL count on every slot FIRST — it selects the
   channel-mismatch mapping — then one insert per slot appended to the track's
   twPluginChain in slot order.

8b. VOLUME REACHES THE POST-FX GAIN STAGE, MUTE STAYS STRUCTURAL (proposal 37
   P3a / D5). onTrackVolumeChanged() targets cpGainStage_, the twGainStage
   STrack wires between cpDspChain_ and cpRewire_, so the fader is applied AFTER
   the inserts; twTrackMix::setTrackGain is a no-op until P5 deletes it. The
   gain stage is part of the render chain like any other component:
   setChannels() sets its width, bumpRenderChainEpoch()/…Range() must include
   it, and ~STrack drops it. Missing it from either bump makes a fader change
   inaudible until some unrelated edit invalidates.
   MUTE IS NOT ROUTED THERE. onTrackMuteChanged/solo keep nulling the parent's
   input plug (twMixer) and skipping the clip entry (twTrackMix, for folder
   lanes), because mute belongs to the summing CHANNEL — an asset windowing a
   muted track must still capture its material. twGainStage HAS a ramped audio
   mute; nothing calls it until P5's `self:Muted` lane does.

9. A TRACK IS ONE twTrackMix + ONE twPluginChain + ONE twGainStage + ONE
   twRewire, N CHANNELS
   WIDE, AND N IS THE PROJECT'S (proposal 36 B4). It used to be N parallel
   width-1 twTrackMix + twPluginChain pairs, one per "bus", built by
   setNBusses() — which could only GROW (a shrink was Q_ASSERT_X( false, ... ),
   compiled out of the shipped build, so it returned SILENTLY and left stale
   wiring) and had to back-fill every existing clip into each new mixer without
   double-inserting it. setChannels() replaces it: the components are built on
   the FIRST call and a later call only re-widths them, so nothing is created or
   destroyed and 8 -> 2 is an assignment. It is a slot, connected to
   SProject::channelsChanged, and it is the ONLY writer of the width.

10. THE nBusses ATTRIBUTE IS NO LONGER WRITTEN, AND IS READ AND IGNORED
   (proposal 36 B4). A track has no bus count; it has the project's width,
   which is persisted once, as <SProject channels='N'>. A second per-track copy
   would be a second authority, and that exact drift has already cost this
   project once — the loader defaulted the attribute to "1" while the
   constructor built 2, so a file omitting it asked for a shrink. A file that
   still carries one gets exactly one warning per track and no effect; pinned by
   project_channels_test's testTrackIgnoresLegacyBusCount.
7. STrack holds an SLink REFERENCE to its chain (cpPluginChainRef_), for the
   constructor-made chain as much as an adopted one. An SObject whose refcount
   reaches zero deleteLater()s itself, and the loader's temporary handle links
   die in ~SProjectLoader — so without a reference of our own an adopted chain
   would be destroyed moments after adoption. Holding one in BOTH paths also
   keeps nRefs (a serialized attribute) identical between a new and a loaded
   project, which is what lets a save/load/save comparison be byte-equivalent.
   Dropping the old reference is also what retires the empty chain the
   constructor made.
   Because it is NOT a child link, STrack publishes it from
   ownedRefLinks() (model/CONTRACT.md 6b) — the track -> chain edge is
   otherwise invisible to anything walking the reference graph, and
   ~SProject's survivor ordering then freed the chain before the track.
8. A slot's wire schema is descriptor_ VERBATIM, never effective_
   (spluginslot.cpp). effective_ carries the registry's resolved module path;
   serializing it would silently absolutize a relative path and make the
   project machine-specific. SPluginSlot::serializeSelfAttributes calls
   SObject::serializeSelfAttributes FIRST — that is what emits id=, and without
   it SProjectLoader::createObjects aborts the WHOLE load.
9. MIDI output (proposal 37 P7b) is three serialized attributes and nothing
   more: `midiOutPort` is a PORTABLE device NAME (never a machine-local id —
   `SSettings` maps the name to a WinMM index / CoreMIDI uniqueID / ALSA
   "client:port" per machine, the same split the audio output device uses),
   `midiOutChannel` is 0-BASED 0..15 with -1 meaning "as authored", and
   `midiOutOffsetMs` is a signed +-500 ms send offset where POSITIVE means SEND
   EARLIER. All three are written ONLY when they are not the default, so every
   project written before proposal 37 re-serializes byte-identically. The track
   itself opens no port and sends nothing: `SMidiOutPump` (main/shell) reads
   `hasMidiOut()` and the feed.
10. hasMidiOut() feeds `bubblesEventsUp()`, so GAINING OR LOSING A PORT changes
   the `auto` routing rule ("consumed here, or bubbled up") and therefore what
   the PARENT's feed — and its instrument — receives. `setMidiOutput` range-
   invalidates `[0, inf)` on that transition and only on it; a channel or
   offset change moves no audio and invalidates nothing.

11. THE INSTRUMENT IS SLOT 0 WITH A ROLE, AND THE ROLE AND THE POSITION ARE ONE
   FACT (proposal 37 P3b, design D3). `STrack::instrumentSlot()` is the ONLY
   definition: slot 0 of the plugin chain when `getDescriptor().isInstrument`,
   else null. It reads the STORED descriptor, not the resolved one, so a track
   whose instrument is not installed on this machine still looks like an
   instrument track (the placeholder keeps the declared shape — plugins inv.
   17). Everything derived hangs off it and nothing is stored twice: the head's
   "I" glyph, the FX strip's `kind=instrument` row, `bubblesEventsUp()` ("an
   instrument CONSUMES the events here, so they do not bubble"), and the tail in
   `getDuration()`. The three rules the ACTIONS enforce: an instrument
   descriptor lands at index 0 whatever `slotIndex` asked for; a SECOND
   instrument is REFUSED (one event feed per track — a second one would either
   duplicate every note or silently take none); `reorder-plugin` across slot 0
   is refused in both directions.
12. SLOT 0 GETS THE TRACK'S FEED; EVERY OTHER SLOT HAS IT TAKEN AWAY
   (`syncInstrumentSlot`, proposal 37 P3b). The processor holds a
   `twEventSource*` and never walks the model, so the merge's SOURCE LIST — which
   moves when a child is added, re-parented, muted, solo-excluded or re-routed —
   is rebuilt on the MAIN thread by `refreshInstrumentFeed()`, called from
   `bumpRenderChainEpoch()` and `bumpRenderChainEpochRange()`. Those are exactly
   the points every model change that reaches this track already passes through,
   and the call is guarded on there BEING an instrument, so a project without one
   pays two pointer hops. An event edit reaches the slot-0 insert's pages by the
   same route a bypass does — the track's own `bumpRenderChainEpochRange` into
   `cpDspChain_->invalidatePagesInRange`, because an `SPluginChain` is not an
   `SLink` child and the root-down walk cannot find a slot (inv. 5's pitfall).
   The range is OPEN-ENDED on the right for an event edit: the consumer is
   class-1 (design F9), and `applyChildTrackAudibility()` widens to
   `EVENT_DIRTY_END` too when this track holds an instrument, because a muted
   child stops contributing EVENTS as well as audio.
13. THE PROJECT END OF AN INSTRUMENT TRACK IS "LAST EVENT CLIP END +
   tailFrames()" (proposal 37 P3b, design 3.1). `eventEndTime()` is the event
   extent alone — own Event children plus the same question of every child that
   bubbles up, WITHOUT the mute/solo resolution, because a project must not get
   shorter because a lane happens to be muted right now — and `getDuration()`
   takes the max of the audio extent and `eventEndTime() + tailFrames()`. The
   tail is added to the EVENT extent only: an audio clip must not gain a synth's
   release. `SPluginSlot::tailFrames()` never materializes a processor, because
   `getDuration()` is reachable from a render thread.
14. **The RUN BARRIER finds its instrument tracks by WALKING, not by a
   registry** (proposal 37 P3c). `sinstruments::collectInstrumentTracks(root,
   out)` (`app/objects/track/sinstrumenttracks.h`) is a depth-first walk of the
   LANE tree — `isLane()` only (proposal 41 D3 split lane-state from the more
   general `isPathContainer()`), exactly as `ssolo`'s walks do, so a
   folder track's own instrument and a leaf's are both found, and a fragment
   (which is a path container but never a lane) is never descended into. It
   lives here and not in the shell because it is a fact about how a track
   tree is shaped.
   Deliberately not a maintained list: a registry would have to be kept in step
   with insert/remove/reorder-plugin, the undo of each, track add/remove/
   reparent and project load — nine places, every one of them a chance to hand
   the barrier a dangling pointer — and the walk runs once per transport start,
   never per page, per block or per edit.

Self-registration (Phase 5): strack.cpp registers "STrack" and
spluginslot.cpp registers "SPluginSlot" with SProjectLoader from a static
initializer (spluginchain.cpp registers "SPluginChain").

How to test: render_split_slip_offset.qxa (move-clip across tracks +
removeClip), render_sawtooth_with_effects.qxa (reparent),
test_track_*.qxa (UI), plugin_slot_roundtrip.qxa + plugin_missing_placeholder.qxa
(the slot/chain round trip and the missing-plugin placeholder);
track_list_view_roundtrip.qxa (inv. 24 — fold state is a plain STrack
attribute now, and survives save/load exactly);
mc_track_width.qxa (the track root really carries N distinct channels, and the
master is their sum), mc_width_change.qxa (2 -> 8 -> 2 including the undo
direction), mc_legacy_pull_wide.qxa (the same, with SMARAGD_REVAL_WORKERS=0 so
there is no scheduler at all) and project_channels_test (the project's width
reaches the track, and the legacy attribute does not).
The instrument slot (proposal 37 P3b): instrument_sine_render.qxa (three
formats, closed-form level and pitch per second), instrument_mixed_track.qxa
(the pass-through sum, and `x + 0.0f == x` as a byte compare),
instrument_edit_reaches_render.qxa (an event edit reaches the slot-0 insert's
pages, and the reposition before it is inaudible),
instrument_transpose_and_velocity.qxa, instrument_bypass_keeps_voices.qxa,
instrument_slot_rules.qxa (the three slot rules, the derived glyphs and the
project end) and instrument_folder_drums.qxa (one instrument on a folder playing
its children's patterns, with mute and the two-overlapping-notes rule).
STEREO IS GATED SINCE 2026-08-17 -- the cases above read CHANNEL 0 only,
because P3b predates the wide sink (36-B5): instrument_stereo_render.qxa walks
the four generator mapping rows per channel out of a rendered file (DirectGen,
GenFold, the refused narrow-generator row, and MonoSpread's deliberately EQUAL
channels), and automation_stereo.qxa does the same for a self:Volume ramp and a
slot param: step.
The run barrier's walk (proposal 37 P3c) is exercised by
instrument_render_determinism.qxa, its cross-process driver
instrument_render_determinism_xproc and instrument_locate_continuity.qxa.

Known debt: strackpath being here forces objects/track edges from every
action slice — a path-resolution service extraction is a Phase 6 candidate.

## Automation lanes (proposal 37 P5, design D5 / §3.3)

11. **LANES ARE OWNER-HELD AND ARE NEVER `SLink` CHILDREN.** An `SAutomationLane`
    is a plain `QObject` in a `std::vector<std::unique_ptr<>>` on `SObject`, and
    it is serialized INLINE as `<automation><lane …><p …/></lane></automation>`.
    That is the persistence contract, not a shortcut: the project loader orders
    and resolves on `<SLink>` children only, so an older build reads the element,
    recognises nothing and ignores it. A lane as an `SObject` would need an id, a
    link, a place in the load order and a policy for what happens when its owner
    is dropped — for a breakpoint table with no independent existence.

12. **A TRACK OWNS `self:Volume` AND `self:Muted`, AND BOTH REACH `twGainStage`
    (POST-FX).** `STrack::pushTrackAutomation()` is the only writer;
    `onAutomationChanged()` pushes and then calls `invalidateRenderPathRange()`
    with the EXACT range, because a gain stage is class infinity and pure.
    `applyAutomationToEngine()` is the load-path replay, called once the chain
    exists. `self:Pan` is deliberately absent, and the ORIGINAL reason has
    expired: it read "until the sink is stereo (36-B5)", and the sink went wide
    at B5 — `automation_stereo.qxa` now asserts a volume lane on both channels,
    so there IS somewhere to hear one. What is missing is the pan itself: no pan
    law and no pan stage in the graph, no clip/track model carrying one, and
    `SObject::pan_` still serialized with **zero consumers** (proposal 36 §7
    trap 3). Proposal 36 §8 names panning a non-goal and proposal 37 §12 leaves
    it to a later proposal. A pan lane would still store a number nothing could
    hear — but it is now **unblocked rather than impossible**.

13. **A CLIP's `cut:Gain` REACHES THE MIX THROUGH THE TRACK, NOT THROUGH THE
    CUT.** The curve lives on the WINDOW (an `SCut`), which is not allowed to
    know its track, and it is consumed by the track's `twTrackMix` clip ENTRY.
    `STrack::refreshClipGainCurves()` re-reads every child's lane and pushes it,
    and it runs from `bumpRenderChainEpoch[Range]()` — the one place on the MAIN
    thread that every model change already funnels through, exactly as
    `refreshInstrumentFeed()` does and for the same reason. A take stack is
    asked for its ACTIVE take (`windowTakeAt(-1)`), so an inactive take keeps
    its own envelope and contributes nothing until it is selected.

14. **A SLOT's `param:` LANE INVALIDATES `[a, INT64_MAX)`, AND IT NEEDS THE
    TRACK TO DO THE WALK.** `SPluginSlot::onAutomationChanged()` pushes the
    curves into the processor and emits `audioInvalidatedRange(start, end)`;
    `STrack::onPluginSlotAudioInvalidatedRange()` turns that into
    `invalidateRenderPathRange()`. `SObject::invalidateRenderPathRange()` FROM
    THE SLOT is a no-op — the walk goes down from the project root through
    `childLinks()` looking for `this`, and an `SPluginChain` is deliberately not
    an `SLink` child of its track (the proposal 08 M5 finding, pluginui inv. 6).
    The range is open-ended because a plugin is CLASS 1: its DSP state at any
    position depends on everything before it (design F9).

15. **A STATIC EDIT ON A READ-FAMILY LANE BECOMES A POINT AT THE LOCATOR.**
    `set-track-volume` / `set-track-mute` on a track whose lane's mode is
    read/touch/latch/write commit a one-point `set-automation-points` instead of
    writing the static value, and return ITS inverse. Without that the fader
    moves, the render does not, and the undo stack carries a step nobody can
    hear. Trim and Off are NOT redirected — there the static value is still the
    thing being edited (design §11 decision 3). The locator comes from
    `SAppContext::getGlobalLocatorPos()`, which P5 added for exactly this.

## Live input / monitoring (proposal 21 L1b, design D9)

16. **`trackInput` is a PORTABLE STRING and `isLiveOwnedLane` is a WIRING
    PREDICATE.** The input selector is stored as written
    (`none | audio:<device>:<mask> | midi:<port>:<ch|any> | keyboard`) and
    parsed once, by the plan builder, on the main thread — the model's job is
    to round-trip it, and an unknown spelling must survive a save/load rather
    than be normalised away. `monitorMode` is `auto|on|off` with AUTO the tape
    machine (input while STOPPED or RECORDING). Both are written only when they
    are not the default, so every project written before this phase
    re-serializes byte-identically, and `liveOwnedLane_` is NEVER written: it
    describes this session's monitoring, not the project.

17. **`isLiveOwnedLane()` is consulted at the two places solo already is and
    NOWHERE ELSE** — `SStdMixer::reconnectTracksToMixer` nulls a top-level
    closure member's plug, `STrack::applyChildTrackAudibility` `setClipMuted`s
    a nested one — and it is deliberately kept OUT of `ssolo::isLaneAudible`.
    Folding it in would drop a live child's EVENTS from a folder instrument's
    feed and darken its meters, which are exactly the two things a monitored
    track must keep doing.

18. **`ArmedForRecording` is INERT ON LOAD.** The flag round-trips through the
    file untouched, but a track that arrived armed is not a monitoring source
    until the user arms it in THIS session (`SLiveMonitor::projectChanged()`
    records the set). Opening a project must not open the microphone.

19. **A LIVE RECORDING GOES INTO NEITHER THE BUS MIXERS NOR THE EVENT CLIP
    SET** (proposal 21 L3b, design D7). `trackChildWasAdded` /
    `WasRemoved` / `DurationChanged` route on `SObject::isLiveRecording()`
    BEFORE they route on `contentKind()`, and the route is "do nothing but the
    duration bookkeeping". The argument is exactly the one invariant 5b makes
    for a MIDI clip: a growing recording has no root component, so inserting it
    as a clip entry would cost a dummy freeze per page per clip AND make
    `twView::getComponent() returned nullptr` fire once per freeze forever —
    and its length moves ten times a second, which is the traffic design D7
    says must not reach the root. The predicate is on the BASE CLASS for the
    same reason `contentKind()` is: the track must route by it without knowing
    the concrete type, and `objects/track` has no edge to `objects/wave`.

20. **WHILE A LANE IS LIVE-OWNED, THE ROOT WALK IS DEFERRED AND ISSUED ONCE AT
    DISARM.** `invalidateRootWalkOrDefer()` accumulates the union of the ranges
    and `setLiveOwnedLane(false)` flushes exactly one
    `invalidateRenderPathRange()` (design D7). The walk is the expensive half
    of a clip edit — from the project root down, per hop, mapping domains —
    and while the pump owns the lane its frozen output is not being summed at
    all, so a walk per edit buys nothing. Note this is the GENERAL rule for
    edits during monitoring; the growing recording clip is covered by 19 and
    never reaches it.

19. **A `midi:` / `keyboard` `trackInput` makes the CONSUMER a live source, not
    the track** (proposal 21 L2, design D4). `trackInputMidiPort()` /
    `trackInputMidiChannel()` parse the spelling; who SOUNDS the notes is the
    live monitor's question and it answers it with `sliveplan::midiConsumerFor`,
    which walks the routing up the way `eventFeed()` walks it down. `keyboard`
    is a whole spelling rather than a scheme because there is exactly one such
    port and it exists on every machine; `midi:keyboard:any` means the same
    thing.

21. **`SPluginSlot::reportedLatencyFrames()` is REPORTED, never compensated**
    (proposal 21 L5). It exists here for the same reason `paramRows()` does: the
    FX strip and the transport readout have to show it, and the number belongs
    to the plugin the slot owns. **Nothing anywhere compensates for it** —
    plugin delay compensation is out of scope (proposal 37 P9) — so every mount
    that displays it must also say that it is not compensated. 0 for a Missing
    or Unsupported slot, and 0 for the many plugins that report nothing.

22. **`hasChildTracks()` IS THE ONE DEFINITION OF "THIS IS A FOLDER"**
    (proposal 39 M3). Any child link whose object is an `STrack`. The arranger
    spelled it locally in `sstdmixerview.cpp` to decide whether a row gets a
    fold triangle; the folder-sum overlay asks the same question, and two
    spellings of it is one more than there should be. The local copy is gone.

23. **`collectChildSumEnvelope()` IS A SUM OF ENVELOPES, AND IT NEVER TOUCHES
    THE ENGINE** (proposal 39 M3, design D3). The walk, in full:

    - Direct children first: each starts at ITS OWN linear gain
      (`pow(10, volumeDbSnapshot()/20)`), and recursion multiplies each further
      level in. So a descendant's contribution carries the product of the gains
      of every track from its own track up to but **EXCLUDING** the track being
      asked. **Our own fader is nowhere in our own answer**, which is M2's rule
      applied to the lane the overlay is drawn on; a child one level down is
      not that lane, so its fader does count.
    - **Audibility is `ssolo::isLaneAudible`**, resolved once per walk against
      the project root, never a local `isMuted()`/`isSolo()` chain. An
      inaudible lane contributes nothing. `main/timeline/CONTRACT.md` inv. 10
      records what the local copies cost the last time: a solo nested inside a
      folder was a no-op and the meters disagreed with the ear. Note this is
      audibility ONLY — `isLiveOwnedLane()` is deliberately not consulted, for
      the same reason inv. 16-18 give: a live-owned lane's clips still exist.
    - A clip is asked through `SObject::getInlineRenderer()->collectEnvelope()`
      — never by concrete type — and is given **its own pixel span**, sized the
      way `STrackRendererInline`'s clip loop sizes the rect it draws that clip's
      waveform into. Handing a clip the whole lane window would be silently
      wrong rather than imprecise: a cut's collect clamps a negative
      clip-relative position to 0, so a clip starting after the window's left
      edge would smear its audio across every column. Gate:
      `envelope_offset_window.qxa`, the only case whose clips AND window both
      start somewhere other than 0 — which is why `folder_sum_preview.qxa`
      passes with the span reverted and this one fails 18 assertions.
    - **The accumulator is `int32` and the clamp to [-127,127] happens ONCE, at
      the end.** `preview_t` is a `signed char`: accumulating in it wraps, and a
      wrap makes two loud children draw QUIETER than one — a failure that looks
      like a feature. `folder_sum_preview.qxa` gates both directions (exact
      doubling where it fits, exactly 127 where it does not).
    - **False, writing nothing, when nothing contributed**, so the painter draws
      nothing at all rather than a flat line at zero.

    It runs on the MAIN thread from `paintEvent` and must never block, freeze,
    demand a page or wait — every probe is an index into an array a child's
    preview already built. The exact answer (this track's own `getPreview()`,
    which really is the summed output) goes through `requestPage()` and is
    therefore unusable here; see `main/timeline/CONTRACT.md` inv. 22.

24. **FOLD STATE IS A PLAIN SERIALIZED ATTRIBUTE ON `STrack`** (fix/
    track-list-polish m), `isCollapsed()`/`setCollapsed()`, written as
    `collapsed='true'` only when true — same non-default-only rule as
    `midiOutPort`/`trackInput`/`monitorMode` above, so every project saved
    before this attribute existed re-serializes byte-identically. It used to
    live only in a view-owned `QSet<STrack*>` on `SStdMixerView`
    (`main/timeline/CONTRACT.md` inv. 18's "fold set"), which meant it reset
    to expanded on every load and needed its own entry in `pruneUiState()`'s
    per-track pruning walk to avoid a dangling key when a track was removed.
    Neither is true any more: being an ordinary object attribute, it survives
    save/load for free and dies with the object automatically. It changes no
    audio and nothing in the render path reads it — purely a UI fact that
    happens to be worth remembering across a reload.

## Feel Flow (proposal 40 M2)

25. **`SFeelFlowTrackBounce::feelFlowForUi()` is an atomically-swapped
    shared_ptr UI cache, the `SPlainWave::onsetsForUi()` pattern verbatim**
    (`main/objects/wave/CONTRACT.md`). The FIRST call after a fresh,
    successful bounce does ONE `twSidecarStore::loadAny()` (never a demand);
    a MISS or "no bounce yet" caches an EMPTY result so a repaint never
    re-hits the store; the analysis job's completion resets the slot to null,
    forcing exactly one reload — whether that job succeeded, found nothing
    analyzable, or the content was already valid and only the epoch snapshot
    moved. `STrack::feelFlowForUi()` forwards to the holder and returns
    nullptr ONLY when no bounce has ever been started (the holder itself does
    not exist yet); freshness is a SEPARATE question, deliberately not folded
    in — a caller (the lane painter, `main/timeline/CONTRACT.md` inv. 25)
    checks `feelFlowStale()` itself.

    **The band's colour law is PALETTE MEMBERSHIP from ONE authoritative LUT,
    `STrackRendererInline::feelFlowPalette()`** (2026-08-21 follow-up, over
    the requester's explicit request for AGGRESSIVE visibility — "rainbow, in
    case I miss shades of grey" — against the original partial-alpha tint
    mixed from the lane fill). A quantized 24-step hue ramp, red (low
    compliance) through yellow to green (high compliance), full saturation,
    value ~0.85, fully OPAQUE (alpha 255); `feelFlowPaletteIndex(compliance)`
    is the one quantization function. Both the painter
    (`drawFeelFlowBand`) and the pixel gate (`SMainWindow::describeLaneOverlay`'s
    band-mode exact-RGB classification, `main/timeline/CONTRACT.md` inv. 25)
    read this SAME array — never a second copy of the ramp. Chosen over the
    old lane-fill-relative interpolation deliberately: the palette no longer
    depends on a track's own fill colour, which is what makes an exact-RGB
    LUT match possible at all. The gate got STRONGER for it, not weaker: an
    absent-or-stale band is now asserted as an EXACT zero LUT pixels
    (`maxLutPixels="0"`), where the pre-existing luminance-relation gate could
    only name a measured noise floor with margin (a clip's own waveform paint
    lands inside that relation too). Computed once (function-local static);
    no per-column allocation anywhere in the paint path (inv. 1).

## Feel Flow tuning panel + trained mode (proposal 40 M3)

26. **Trained-mode state is serialized on `STrack` as an INLINE, NON-`SLink`
    element** (`<feelflow mode='adaptive'|'trained'><trained data='...'/>
    </feelflow>`, base64 = `twGrooveTrainedStructureSerialize`'s wire
    format) — the automation-lane discipline (inv. 11), not a second one:
    `STrack` overrides `serialize()`/`readPostChildrenAttributes()` FULLY
    (mirroring `SObject::serialize()`'s own body, the way
    `SMidiSequence::serialize()` does for its `<events>` payload) rather than
    hooking into a shared seam, because `SObject` offers none there. Written
    ONLY when `feelFlowMode_ != Adaptive` or a trained structure exists, so a
    project that has never touched Feel Flow's mode re-serializes
    BYTE-IDENTICAL to a pre-M3 build and every existing golden is untouched.
    An older build simply ignores the element, exactly as it already ignores
    `<automation>`.
27. **The analysis params blob carries `mode`/`trained` ADDITIVELY, and ONLY
    for `mode == Trained`** (`twGrooveAnalysisParams::serialize`,
    `tw/sidecar/twgrooveaspect.h`) — a default-constructed params blob (every
    M1/M1b/M2 caller, and every track that has never touched the mode) stops
    at exactly the pre-M3 byte sequence, so its `paramsHash` and every cached
    `groove.res`/`groove.ev` store entry keyed from it are UNCHANGED. Only a
    track actually switched to Trained mode mints a new (and by construction
    DIFFERENT) key — which is also what makes AC 4's param-aware staleness
    work with no engine-side epoch bump: `SFeelFlowTrackBounce::isStale()`
    rebuilds the track's CURRENT effective params (`STrack::feelFlowMode()` +
    `feelFlowTrainedStructure()`), hashes them the same way, and compares
    against `paramsHashAtBounce_` — recorded from the SAME `twGrooveAnalysisParams`
    the bounce+analysis job actually ran under, built on the calling
    (main) thread by `SFeelFlowTrackBounce::start()`, NEVER re-read from the
    track inside the background job (which runs arbitrarily later, on
    another thread, with no synchronization of its own over `STrack`'s
    mode/trained members).
28. **`twGroovePendulumTrainStructure`'s `trainedHasRegion` is ALWAYS sized
    to the front end's region count, never empty, even on total training
    failure** (`twgroovependulum.cc`) — so "has this track EVER been
    trained" (`STrack::feelFlowHasTrainedStructure()`, a null check on the
    `unique_ptr`) and "did THIS training pass recover anything"
    (`std::any_of` over `trainedHasRegion`, `SLearnFeelFlowAction::apply()`)
    are two DIFFERENT questions answered two different ways — conflating
    them (checking `.empty()` for the second) would make `learn-feel-flow`
    silently accept a selection with no recoverable groove and store an
    all-false structure that never falsifies. The SAME `.empty()` check is
    correct at the `twGrooveAnalysisParams::trained`/`buildEffectiveGrooveParams`
    layer, because there it is asking the FIRST question (a
    default-constructed `twGrooveTrainedStructure` has `trainedHasRegion`
    genuinely empty — no `TrainStructure` call has ever populated it).
