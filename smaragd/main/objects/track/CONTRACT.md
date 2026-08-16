# app/objects/track — CONTRACT

Purpose: the track object. STrack (bus mixers, clip synchronization to
twTrackMix, MapPosFn wiring), its renderers, strackpath (path resolution
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

5b. EVENT CHILDREN GO INTO THE EVENT CLIP SET, NOT THE BUS MIXERS (proposal
   36 3.2). A child whose contentKind() is Event is inserted into the track's
   ONE twEventClipSet - same slots, same SLink* key rule as the mixers, but one
   set per track rather than one per bus, because events are not per bus. It is
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
   LANE tree — `isPathContainer()` only, exactly as `ssolo`'s walks do, so a
   folder track's own instrument and a leaf's are both found. It lives here and
   not in the shell because it is a fact about how a track tree is shaped.
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
    exists. `self:Pan` is deliberately absent until the sink is stereo (36-B5) —
    a pan lane today would store a number nothing could hear.

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
