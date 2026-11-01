# Proposal 55 — Return tracks and sidechain inputs

**Ticket:** QBX-241. **Status:** PROPOSED (2026-10-26), not built. Written
against `origin/main` `509f386c`; every `file:line` below was re-read there.

**Why now.** NassauDigital, the sampler being built for qbx, has eight stereo
aux outputs, and a drum kit is mixed through them: the kick on its own channel
ducks the bass, the snare gets its own reverb (nassau-digital
`docs/DESIGN.md` §3.1). That needs two things qbx does not have: a track fed by
a plugin's aux output, and a plugin input fed from another track. The author
decided on 2026-10-26 that **both come first, as a qbx proposal of their own**:
a qbx feature every multi-output plugin and every ducking compressor uses, not
a sampler feature (DESIGN §3.1, §13 "Settled"; nassau-digital `docs/GATES.md`
X2). This is that proposal. The sampler's own multi-output gate (GATES.md Q8)
waits for it.

**Prior art.** Proposal 37 §5.4 designed multi-output and left it for P9
(`37_ORCHESTRATION.md` P9). Proposal 45 built system lanes, 47 the send tap,
the send bus and the cycle refusal, 48 the mixer pane. This proposal reuses
47's shape wherever it can, and says where it cannot.

---

## 0. Prerequisite reading

1. `plan/proposed/37_MIDI_INSTRUMENTS_AUTOMATION.md` §4.3 (the generator rows),
   §5.1–§5.2 (the bus half of the ABI), **§5.4** (the design this proposal
   finishes).
2. `plan/proposed/47_SEND_ROUTING.md` D1, D2, D4, D5, **D6** (and M3's
   correction of it), D9, and the traps T1–T9. Most of this proposal's traps
   are 47's, paid again.
3. `plan/proposed/45_SYSTEM_LANES.md` D2, D6, D9, D10 — what a system lane is
   and what it refuses.
4. `smaragd/tw303a/plugins/CONTRACT.md` invariants 13, 14, 16, 33, **34**, 37,
   and the "Known debt" entry on the WideGen surplus.
5. `docs/contracts/FREEZE_PROTOCOL.md` "Caching and concurrency" and "Page
   geometry".
6. `smaragd/main/objects/mixer/src/sstdmixer.cpp` `reconnectTracksToMixer()`
   and `rewireSendBuses()` — the one pass that owns all mixer wiring.

---

## 1. What is true today

Every claim here was read in the code at `509f386c`.

### 1.1 Aux output buses are reported, and only bus 0 reaches the host

- The ABI reports every audio output bus: `twPlugin::audioOutBusCount()` /
  `audioOutBus(i)` (`tw303a/plugins/include/tw/plugins/twplugin.h:113-131`),
  with `twPluginBusInfo { channels, isMain, name }`
  (`tw303a/plugins/include/tw/plugins/twpluginevents.h:235-247`). The
  event-aware `process()` takes `outBuses[bus][channel]`; its default forwards
  only `outBuses[0]` to the legacy call (`twplugin.h:138-164`).
- `twPluginIoLayout` is the MAIN bus only: `audioInputs`, `audioOutputs`
  (`twplugin.h:14-17`). Everything the slot processor decides is decided from
  it.
- **CLAP** reads every port of both directions and takes the `IS_MAIN` one as
  the layout (`tw303a/plugins/src/twclapplugin.cc:841-876`). At `prepare()` it
  builds a zero-filled scratch buffer for every non-main port, in and out
  (`twclapplugin.cc:1011-1034`). `process()` reads `outBuses[0]` only
  ("Only the MAIN bus is wired", `twclapplugin.cc:1523-1528`), points the main
  ports at the caller's buffers (`twclapplugin.cc:1581-1586`) and hands the
  plugin every port (`twclapplugin.cc:1635-1638`). So an aux output is
  rendered into scratch and dropped, and an aux INPUT is fed the silence the
  scratch was initialised with.
- **VST3** reads every bus the same way (`twvst3plugin.cc:480-509`) and, at
  `prepare()`, activates the main buses only: "Main buses on, everything else
  off: we have no sidechain routing" (`twvst3plugin.cc:717-726`). Non-main
  buses get scratch buffers (`twvst3plugin.cc:753-788`).
- **AU** renders element 0 only (`twauplugin.cc:311`, `:644-646`), and the
  whole AU backend is unverified (plugins CONTRACT inv. 35).
- **The slot processor** hands every plugin a ONE-element bus array: the
  effect rows at `tw303a/plugins/src/twpluginslotproc.cc:611`, `:626`, `:645`
  and the generator core at `:968`. The non-automated Direct path calls the
  legacy three-argument `process()` (`twpluginslotproc.cc:615`).
- plugins CONTRACT **inv. 34** states it: "AUX OUTPUT BUSES ARE DISCOVERED AND
  NOT YET ROUTED" (`tw303a/plugins/CONTRACT.md:511-518`).

**Why only the first.** Nothing downstream could take a second bus. Proposal 37
§5.4 assumed the per-bus TAPS that existed when it was written: "`Output.bus[M]`
on the processor + a tap `(processor, out ≥ C)` placed as the content component
of a return track IS the multi-out design with zero new engine types"
(`37_MIDI_INSTRUMENTS_AUTOMATION.md:855-860`). **Proposal 36 B4 then retired
those taps.** Before B4 a slot was "one processor plus N per-bus TAPS plus a
private (startPos, len, stamp) -> all-bus page cache: the first tap to ask
rendered every bus by reaching SIDEWAYS through its siblings", and that sideways
gather is what deadlocked (plugins CONTRACT inv. 13,
`tw303a/plugins/CONTRACT.md:177-196`; shape note at `:47-51`). Since B4 a slot
is ONE `twPluginInsert` with one port in and one port out
(`twpluginchain.cc:171-200`, "a plain series wiring of port 0"), the processor
caches nothing (inv. 15), and the component page cache above the insert "is the
only cache there is". **So §5.4's "zero new engine types" no longer holds**:
there is no per-bus tap left to place, and re-creating one with a private cache
would re-create inv. 13's hazard. §3.1 below replaces it.

The generator row **WideGen** is a different thing and stays as it is: it is a
MAIN bus wider than the page (`0 -> M, M > C`), whose surplus channels are
"rendered and DROPPED" (`twpluginslotproc.cc:454-457`; plugins CONTRACT `:259`
and Known debt `:880-882`). An aux bus is a separate bus, not surplus channels
of the main one.

### 1.2 Pages are sized per channel, per component

- A page is planar, carries its own immutable channel count, and costs
  256 kB per channel (`tw303a/pages/include/tw/pages/tw_output_page.h:70`,
  `:123-132`; FREEZE_PROTOCOL "Page geometry", `docs/contracts/FREEZE_PROTOCOL.md:47-66`).
  A component's width is `getOutputChannels()`, and a page whose width
  disagrees with its producer's declared width is a MISS (`FREEZE_PROTOCOL.md:58-61`).
- Each component caches its own pages in `outputPages_`, keyed by start
  position (`tw303a/graph/include/tw/graph/twcomponent.h:773`). The track path
  and the master are all `SProject::channels()` wide (`FREEZE_PROTOCOL.md:56-58`).
- The insert renders `page.channels()` channels from ONE upstream page:
  `renderCore_` gathers from `fetchInputPage( 0, startPos )`
  (`tw303a/plugins/src/twplugininsert.cc:143-176`, `:190`).
- The chain forwards its last insert's page verbatim
  (`tw303a/plugins/src/twpluginchain.cc:334-348`).

So there is today exactly one place a slot's output can live: the insert's
`project-width` page. An aux bus has nowhere to go, which is the "cache sizing"
37 §5.4 named as missing.

### 1.3 Plugin input buses: one, the main one

- The ABI has no input-bus API at all: `process()`'s `in` is
  "`ioLayout().audioInputs` channels" (`twplugin.h:140`).
- The insert has one input port and the chain wires port 0 only
  (`twpluginchain.cc:171-200`). Plugins CONTRACT inv. 37: "THE HEAD INSERT
  ALWAYS HAS ONE AUDIO INPUT".
- CLAP feeds a non-main input port zeros (§1.1); VST3 leaves it inactive
  (`twvst3plugin.cc:717-726`). A real sidechain compressor loaded today
  therefore sees a silent key and never ducks.
- `SPluginSlot::serialize` already leaves room: "A slot has no SLink children
  today; keep the base class's shape so it stays correct if one is ever added
  (a side-chain source, say)" (`main/objects/track/src/spluginslot.cpp:392-393`).

### 1.4 Render order, dependency and cycles

- **The scheduler plans from input plugs.** The base `planPage()` adds one dep
  per wired input plug (`tw303a/graph/src/twcomponent.cc:1157-1172`), and
  `CaptureRevalidator::expandNode_` expands deps recursively, dedups by
  `(component, pageStart)` and guards depth at **64**
  (`tw303a/schedule/src/capture_revalidator.cc:459-497`; 47 D6 quoted 32, the
  guard has since been raised). So a new plug is a new scheduled dependency
  with no scheduler change.
- **A render-time cycle is broken, not hung.** `FreezeContext` returns silence
  when a component's render re-enters itself (`FREEZE_PROTOCOL.md:38-40`;
  `twcomponent.cc:1184-1189`). 47 M3 measured a cyclic send graph rendering in
  about a second, deterministically (47 D6 as corrected). The refusal of a cycle
  is therefore about DEFINED AUDIO, not about a hang.
- **Cycles are refused at the verb and broken at the wiring pass**, for sends
  only: `reaches()` in `main/objects/mixer/src/ssendtapactions.cpp:64-66`, and
  the deterministic break in `rewireSendBuses()`
  (`main/objects/mixer/src/sstdmixer.cpp:433-452`). Both walk SEND edges only.
- **Staleness is per component and the walk is scoped to containment.** A page
  is current when its stamp is at least the component's own `contentEpochNow()`
  (`twcomponent.cc:966-971`; `twcomponent.h:483-492`): the stamp carries no
  upstream epochs. The app's walk bumps the chains that CONTAIN the edited
  object, up to the root (`main/model/src/sobject.cpp:1097-1108`, `:1140-1163`).
  `SStdMixer::bumpRenderChainEpoch` bumps the master sum, the master lane and
  the rewire (`sstdmixer.cpp:161-171`) — not the send lanes, which are not
  `childLinks()` members.

  **Consequence for this proposal**: a return track or a keyed track is not on
  its source's containment path, so an edit on the source would leave it
  serving pages frozen before the edit. That is a hole to close (D7), and it
  raises a question about sends that §8 M0 measures before anything is built
  on top of it: whether an edit to a send's SOURCE reaches the send lane's
  pages today. 47's gates show `invalidateRenderPath()` is what makes a
  ROUTING edit audible (`sstdmixer.cpp:706-725`); none of them edits a source
  after the first render. Not claimed either way here; M0 measures it.

  **Measured in M0 (2026-10-27, QBX-248): the hole is real and total.** See
  §8 M0's record: an edit to a send's source left the send lane
  byte-identical to the pre-edit render.

### 1.5 Mute, solo and live ownership

- A user track's mute is STRUCTURAL: its summing parent nulls its plug; its own
  pages keep its material (`main/objects/track/src/strack.cpp:1861-1867`;
  `sstdmixer.cpp:282-284`). A system lane's mute is its gain stage, because it
  has no summing parent.
- A system lane cannot be soloed: `ssolo::anySoloInTree` walks `childLinks()`,
  which a system lane is not in (`sstdmixer.cpp:319-321`;
  `main/model/include/app/model/ssolorules.h:56`, `:108`).
- A live-owned slot answers every non-pump render with silence and counts it
  (`twpluginslotproc.cc:1032-1044`). 47 D9 decided a live lane does not feed a
  send, for that reason.

### 1.6 The fixtures

- `tw.test.clap.sine` already has a mono AUX out next to its stereo main
  (`tw303a/plugins/tests/twtestclap.c:18`, ports at `:571-631`), carrying
  `s * 0.5f` where main channel 0 carries `s` (`twtestclap.c:1058-1065`).
  Nothing consumes it.
- No fixture has a second INPUT bus. No compressor exists anywhere in the tree:
  the native plugins are `twPassThrough` (a bit-crusher) and
  `twNativeInstrument` (the 303) (`tw303a/plugins/src/twpluginregistry.cc:50-51`).

---

## 2. Decisions

### D1. A return track is a USER TRACK with a source, not a system lane and not a kind

A return track is an ordinary `STrack` in the mixer's `childLinks()`, carrying
one attribute: its **aux source** `(source track, slot, bus)`. "Is a return
track" is a derived fact (the attribute is set), in the spirit of proposal 37
D3 ("no track kind; an instrument is a slot with a role").

Argued from what 45, 47 and 48 built:

| A return needs | As a user track | As a new system role (45/47 shape) |
|---|---|---|
| Solo ("solo the kick") | `ssolo::isLaneAudible` already answers it | **refused** by construction: system lanes are not in `childLinks()`, so solo cannot reach them (`sstdmixer.cpp:319-321`, 45 D6) |
| Its own sends (the snare's own reverb, DESIGN §3.1) | already a tap source: `rewireSendBuses` collects `childLinks()` (`sstdmixer.cpp:405-409`) | a second special case beside send→send |
| Folder membership (a "Drums" folder holding the returns) | already | impossible: a system lane has no summing parent |
| Arranger row, mixer strip, meter, automation, M/S, colour | already, all of 48's mounts | 45 M4 and QBX-104 had to add each one by hand for send lanes |
| Addressing | `trackPath`, like every plugin verb | a new `$return:` spelling in 45 D9's grammar |
| Removal anywhere in the list | already | 45 restricts send-lane removal to the LAST lane because the address is an index |

What a system role would have bought — "owned by the project, cannot be
deleted by accident" — is not wanted: a return track the user deletes is a
routing the user removed. The two things a user track must NOT do as a return
are refused explicitly: it takes no clips (`acceptsClips()` already exists as
the seam, `main/objects/track/include/app/objects/track/strack.h:136`) and it
has no child tracks (its `twTrackMix` is disconnected, D3).

A send lane is not reused either: a send lane's input is a SUM of taps from
whole lanes (47 D1), a return's input is ONE bus of ONE slot. Folding the two
would put a plugin bus into 47's tap graph, whose sources are lanes.

### D2. The aux bus lives on the insert's page, as a side page; one new engine component reads it

The insert renders every routed bus in the same `process()` call it already
makes and keeps the aux channels in a **side page** attached to its own cached
page (`twOutputPage::aux`, a `shared_ptr<twOutputPage>` whose width is the sum
of the ROUTED aux buses' channels). The main page keeps its width, so nothing
downstream sees anything new.

A new component, **`twPluginAuxTap`** (tw/plugins), has one input plug wired to
the source insert's output and one output of the project's width. Its render
calls `fetchInputPage( 0, startPos )` — the same seam every consumer uses — and
copies its bus's channels out of the side page through the mapping in D4.

Why this shape:

- **One render, one cache, one stamp.** Main and aux come out of one
  `process()` call (they must: a plugin renders its buses together), and they
  are cached in one entry, so they go stale together and are evicted together.
  There is no second cache to keep coherent, which is exactly what inv. 13 and
  inv. 15 retired.
- **No sideways gather.** The tap reaches the insert through an ordinary input
  plug, so the scheduler plans it (§1.4: base `planPage()` covers every plug)
  and the lock order stays downstream → upstream.
- **Rejected: widen the insert's own page to `C + A` channels.** It would
  change the insert's declared width, the chain forwards that page verbatim
  (`twpluginchain.cc:334-348`), and FREEZE_PROTOCOL makes a width disagreement a
  MISS (`FREEZE_PROTOCOL.md:58-61`): every consumer of the chain would have to
  learn to clamp, for a feature none of them uses.
- **Rejected: a per-bus tap with its own cache** (37 §5.4 as written). It is the
  pre-B4 shape and its deadlock.

The cost is the **cache sizing** 37 §5.4 named: per cached page of a routed
insert, `256 kB × (routed aux channels)` (`tw_output_page.h:70`). An unrouted
bus costs nothing beyond the backend's per-block scratch it costs today. The
sampler fully routed (eight stereo pairs) adds 16 channels, 4 MB per cached
page of that one insert. M1 measures it.

### D3. A return track's chain reads the tap; its own track mix is disconnected

`STrack::wireAsReturnTrack( tap )`: `tap -> cpDspChain_ -> cpGainStage_ ->
cpRewire_ -> the master sum`, the shape of `wireAsSendLane`
(`strack.cpp:812-822`, declared `strack.h:353-378`) with the tap in place of the
send bus. The return keeps its own rewire and needs no `getRootComponent()`
override, for 47 D1's reason. Its `twTrackMix` is DISCONNECTED, not left
connected and empty: 45 T5 / 47 T2 measured an inert connected component
carrying the identical signal for two milestones.

The wiring happens in the mixer's pass and nowhere else (47 D4, 45 T3): a third
half, `rewireReturns()`, called from `reconnectTracksToMixer()` beside
`rewireSendBuses()` (`sstdmixer.cpp:327`). Because `app/objects/mixer` may not
name a tw/plugins type (`strack.h:338-352`), the mixer calls STrack methods and
the track builds and owns the tap.

### D4. Bus-width mapping is a fixed table, decided once, never guessed per page

The same discipline as plugins inv. 16. For an aux bus of width `A` onto a
return of page width `C`, and for a key page of width `C` into a sidechain bus
of width `S`:

| | rule | precedent |
|---|---|---|
| `A == C` / `S == C` | channel for channel, a byte copy | Direct |
| `A == 1`, `C > 1` | the one channel on every channel | MonoSpread |
| `A == 2`, `C == 1` | average the pair | GenFold / MonoFold |
| `S == 1`, `C > 1` | the mean of the `C` key channels | MonoFold's arithmetic, generalised |
| `S == 2`, `C == 1` | the one key channel on both inputs | MonoFold's input side |
| anything else | **refused**: silent, logged once, reported by `describe` | the Unsupported row |

The `A == C` row is a COPY with no arithmetic, which is what lets X2.1 compare
bytes.

### D5. A sidechain is a second input plug on the insert, fed from a lane's tap point

The key source is a LANE (any user track, a return track or a send lane; not
the master, whose output contains every lane) and a tap point: `pre` = after
its inserts, before its fader (`cpDspChain_` output); `post` = after the fader
(`cpGainStage_` output). These are exactly 47 D3's two points and
`STrack::sendTapComponent( preFader )` (`strack.h:391`) already returns them.
**The default is `pre`**, so pulling the key track's fader down does not stop
the ducking.

`twPluginInsert` gains an OPTIONAL port 1. Wired, its render gathers the key
from `fetchInputPage( 1, startPos )` into a chunk-sized scratch (the
`gather_` pattern, `twplugininsert.cc:149-153`), maps it (D4) and passes it as
the plugin's first non-main input bus. Unwired, the insert is byte-for-byte
what it is today. The chain's series wiring stays port 0 only
(`twpluginchain.cc:171-200`); port 1 is wired by the mixer pass (D3) and
re-asserted after every chain rebuild, because an add/remove/reorder of plugins
re-runs `rebuildWiring_nolock()` (`twpluginchain.cc:90-133`).

The ABI gains the input half of what 37 P2 gave outputs:
`audioInBusCount()` / `audioInBus(i)`, defaulting to the main bus alone, and a
bus-aware `process( inBuses, outBuses, n, events, eventsOut, ctx )` whose
DEFAULT forwards `inBuses[0]` to today's event-aware call. That is 37 P2's
byte-identity trick again (plugins inv. 33, "THE LEGACY process() IS THE SAME
CODE"): no backend that does not override it changes by one instruction.

### D6. A key is NOT an audibility consumer; a return is not muted by its source

Two decisions where 47 D5 does NOT carry over, because a key and a return are
not sends:

- **The key ignores mute and solo.** Soloing the bass to hear its ducking must
  not stop the ducking, and a muted "ghost kick" keying a bass is a standard
  technique. Mechanically this is free for user tracks: their mute is
  structural (§1.5), so a pre or post tap of a muted track still carries the
  material; and the key plug is not wired through `ssolo::isLaneAudible` at
  all. A send lane used as a key keeps its own gain-stage mute on the `post`
  point; that is the send lane's choice, not the key's.
- **Muting the source track does not silence its return tracks.** An aux
  output is a separate signal, not a copy of the track's (unlike a send, which
  47 D5 rightly kills on mute): muting the instrument's main output must leave
  the kick on aux 1 alone. Again free: the source's mute nulls its plug at the
  parent, downstream of the insert the tap reads.

Solo of the SOURCE, by contrast, is a genuine choice and is left open (Q3).

### D7. One routing graph: four edge kinds, one acyclicity walk, and invalidation that follows it

A lane's output now depends on lanes that are not under it. The edges:

| edge | from → to | stored on |
|---|---|---|
| containment | child lane → its folder | `childLinks()` |
| send | source lane → send lane | `SSendTap` (47 D2) |
| aux | source track → return track | the return's aux source (D8) |
| key | key lane → the track whose slot it keys | the slot's sidechain (D8) |

- **Refused at the verb, broken at the pass** — 47 D6's two layers, over the
  UNION of the four kinds. `add-return-track`, `set-return-source` and
  `set-plugin-sidechain` refuse an edge that closes a cycle, naming both ends;
  the mixer pass accepts edges in a fixed order (lane order = file order, aux
  and key edges after send edges) and drops, with a `TW_LOGW`, the first edge
  that would close a cycle among those already accepted, because the loader
  does not go through the verbs (47 M3). Containment is the edge 47 never needed
  and this proposal cannot skip: keying a track from its own folder, or placing
  a return inside its own source track, is a cycle with no routing edge in it.
  The walk is shared: one helper both the verbs and the pass call, replacing
  the two send-only copies (`ssendtapactions.cpp:64`, `sstdmixer.cpp:433-452`).
- **Invalidation follows the graph.** After the containment walk
  (`sobject.cpp:1140-1163`) bumps the chains containing an edit, every lane it
  bumped hands the SAME range to its aux, key and send dependents, transitively
  (the graph is acyclic by the rule above, and a visited set guards the walk
  anyway). Without it a source edit is inaudible on its return (47 T4, from
  the other side), and the failure is silent. **Only `invalidateRenderPath(Range)`
  is load-bearing**, as 47 M2 measured; no epoch bump beside it.
- **The routed mask is state.** Adding or removing a return changes which aux
  buses a slot renders, which bumps that slot's param epoch (plugins inv. 15),
  so the next render hears the change from its first frame — GATES.md Q8.3's
  requirement, met generically.

### D8. Persistence: object identity, not paths or indices

- A return track writes `<auxSource track='<objectId>' slot='<slotId>' bus='1'/>`
  inline, **only when set**, so every existing project re-serializes
  byte-identically (47 T8, persistence's "written only when not the default"
  rule). The track is an object id resolved by `deferResolve` exactly as
  `pluginChainId` is (`strack.cpp:102-104`, `:1721-1748`), which is the
  "serialized like `pluginChainId`" 37 §5.4 asked for. The bus is the plugin's
  own bus index.
- A slot gains a stable **`slotId`**, written only once something references it.
  A slot index would move under `reorder-plugin`, and the slot OBJECT does not
  survive remove + undo: `remove-plugin`'s inverse inserts a NEW slot
  (`main/objects/track/src/sremovepluginaction.cpp:100-112`). So the inverse
  must carry the `slotId` back, or an undo silently unroutes every return of
  that slot (the PIN problem of proposal 52, in a new place).
- A keyed slot writes `<sidechain source='<objectId>' tap='pre'/>` inside its
  `<SPluginSlot>`, only when set.
- Verbs take `trackPath` and `slot`, as every plugin verb does (`set-plugin-param`);
  the file stores identity. A source that does not resolve fails CLOSED: the
  return is silent, the key is silent, both are announced once (47 T6).

### D9. Which slots may route

Aux routing and a sidechain are defined for SINGLE-INSTANCE mappings (Direct,
MonoFold, and the generator rows). On **DualMono** (`twpluginslotproc.cc:478-491`:
one plugin instance per channel) "the plugin's bus 1" has N candidates, so both
verbs refuse and the slot says why. A slot that is Unsupported or Missing has
no buses to route: the return is silent and announced, and re-resolves when the
plugin comes back (`setFactory()`, plugins inv. 17).

One return per `(slot, bus)`: a second is refused, naming the first. Parallel
processing of a bus is a send from its return.

### D10. PDC is out of scope, as everywhere else

A latency-reporting insert on the source's path before the aux, or on the key's
path, makes the return or the key late by exactly that amount. No delay line is
added (47 D8, 37 P9). Every surface that shows a latency already says so.

---

## 3. Design

### 3.1 Engine (tw/plugins, tw/pages)

```
source track:  twTrackMix -> insert0 -> [insert1: plugin, buses 0..K] -> ... -> gain -> rewire -> master
                                              |  main page (C wide) ............^
                                              |  side page (routed aux channels)
                                              v
return track:            twPluginAuxTap(bus k) -> chain -> gain -> rewire -> master

keyed track B: twTrackMix -> [insert: compressor] -> ... -> gain -> rewire -> master
                                   ^ port 1 (key)
key lane:      ... -> chain (pre) / gain (post) --'
```

- `twOutputPage` gains `std::shared_ptr<twOutputPage> aux` (null by default; a
  page without it is unchanged in size and behaviour).
- `twPluginSlotProcessor` gains `setRoutedOutBuses( mask )` and, when the mask
  is non-empty or a key is wired, calls the bus-aware `process()` with an
  `outBuses` array whose routed entries point into the side page and whose
  unrouted entries are null; a backend substitutes its own scratch for a null
  entry, as it does today for every non-main bus. With an empty mask and no key
  it makes exactly the calls it makes today (`twpluginslotproc.cc:605-616`), so
  every golden stays byte-identical by construction.
- `twPluginInsert` allocates the side page when the mask is non-empty (width =
  routed channels), passes its channel pointers, and owns the optional port 1.
- `twPluginAuxTap { bus, channelOffset, channels }`: input plug 0, base
  `planPage()`, render = `fetchInputPage(0)` → `page->aux` → D4 mapping. If the
  side page is absent (mask changed under a render) it renders silence and the
  next epoch heals it; that window is what verify-at-publish already covers.
- Backends: CLAP and VST3 honour a non-null `outBuses[k]` / `inBuses[k]` for
  `k > 0`. VST3 activates exactly the routed aux outputs and the keyed aux
  input at `prepare()` (it re-prepares on a mask change, which is a slot
  rebuild, never a per-page event). AU is out of scope (§7).
- Scanner: the descriptor gains `nInBuses` / `inBusChannels` beside
  `nOutBuses` / `outBusChannels` (`twplugindescriptor.h:40-44`);
  `kScannerVersion` 2 → 3 (`twplugindescriptor.h:97`), which re-probes every
  module once.

### 3.2 Model (objects/track, objects/mixer)

- `STrack`: `auxSource()` / `setAuxSource()`; `isReturnTrack()`;
  `acceptsClips()` false when set; refuses child tracks; `wireAsReturnTrack()` /
  `unwireReturnTrack()` (the latter while the source insert is alive — 47's
  `unwireSendLane` segfault, `strack.h:393-406`).
- `SPluginSlot`: `slotId()`; `sidechain()` / `setSidechain()`;
  `routedOutBuses()` derived from the returns that name it.
- `SStdMixer`: `rewireReturns()` and the key wiring in the same pass; the shared
  routing-graph walk (D7); `routingChanged()` = rewire + `invalidateRenderPath()`,
  the shape of `sendRoutingChanged()` (`sstdmixer.cpp:706-725`).
- Verbs (objects/mixer, beside `ssendtapactions`):
  - `add-return-track trackPath= slot= bus= [name=] [at=]` — creates the user
    track right after the source (same level) and routes it; undo removes it.
  - `set-return-source trackPath= sourcePath= slot= bus=` / `clear-return-source trackPath=`.
  - `set-plugin-sidechain trackPath= slot= source= [tap=pre|post]` /
    `clear-plugin-sidechain trackPath= slot=`. `source` takes any lane path,
    `$send:<name>` included (45 D9).
  - Refusals, each with a message naming both ends: bus 0 (the main bus is the
    track's own output), a bus the plugin does not have, a duplicate return, a
    DualMono slot, the master as a key, a self edge, any cycle (D7), clips or
    child tracks on a return.
- Testkit: `assert-return-track trackPath= source= slot= bus= routed=` and
  `assert-plugin-sidechain trackPath= slot= source= tap= wired=`, reading the
  live wiring, not the model alone (47's `assert-master-inputs` lesson: a wired
  and a wiped route are both silent on a quiet fixture).

### 3.3 UI (pluginui, mixerui, timeline)

- The insert row's context menu (`SPluginEffectStrip::buildRowMenu_`,
  `main/pluginui/include/app/pluginui/splugineffectstrip.h:275-284`) gains
  **Outputs ▸** one item per aux bus ("Aux 1 → new return track"; a routed bus
  shows its return and is checked) and **Sidechain ▸** the lanes a key may come
  from, plus `pre`/`post` and "None". Both go through the three seams the
  repo's convention requires (build, describe, trigger by name) and the
  existing `describeRowMenu()` / `triggerRowMenuItem()`
  (`splugineffectstrip.h:106-125`).
- A return track's head and mixer strip show their source
  (`Drums · slot 1 · Aux 1`) with the full text in the tooltip and an elided
  label (the QLabel-in-a-fixed-column rule). A keyed insert row shows a key
  glyph with the source in its tooltip.
- Nothing else: the strip, row, meter, solo and sends of a return are a user
  track's, already mounted by 45/48.

---

## 4. Invariants (new)

1. **An aux bus is rendered in the same `process()` call as the main bus and
   cached in the same page entry.** No component other than the insert calls
   the plugin, and nothing caches a bus on its own.
2. **An unrouted slot is unchanged.** No side page, no port-1 plug, the same
   `process()` overload: byte-identical renders.
3. **The routing graph — containment, send, aux, key — is acyclic as handed to
   the scheduler**, refused at the verb and broken deterministically at the
   pass, with the break announced.
4. **An edit invalidates its routing dependents over the same range.**
5. **A key ignores mute and solo; a source's mute does not reach its returns**
   (D6).
6. **All routing wiring happens in `reconnectTracksToMixer()`**, and survives
   any number of repetitions of it.
7. **Bus-width mapping is the D4 table**, chosen once per wiring, never per page.
8. **A dangling source fails closed and is announced**, never re-attached by a
   later object that happens to match.

---

## 5. Fixtures and the ducking compressor

**X2.1's second output bus: a new CLAP fixture, `tw.test.clap.split`**, in
`tw303a/plugins/tests/twtestclap.c` — an effect with a stereo main in, a stereo
main out and a stereo AUX out:

- `aux[c][i] = in[c][i]` — a COPY, no arithmetic, so the return's render can be
  compared byte for byte with a plain track playing the same clip;
- `main[c][i] = in[c][i] * MainLevel` (param id 0, default 1.0), so a case can
  silence the main path with `set-plugin-param` and nothing but the aux reaches
  the master.

The existing `tw.test.clap.sine` aux (mono, `s * 0.5f`) is used too, for the
generator row and the MonoSpread mapping, because it is the instrument shape
the sampler has. It cannot carry a `cmp` (its reference would need a gain of
exactly 0.5 somewhere else in the path), so it is held to a closed-form RMS.

**X2.2's compressor: a new CLAP fixture, `tw.test.clap.duck`**, not a native tw
plugin. Stereo main in/out plus a stereo SIDECHAIN input (non-main port). A
keyed hard gate: the key is "on" from the first frame whose absolute key value
exceeds `Threshold` (param id 0, default 0.01) until `Hold` frames (2048,
fixed) after the last such frame; while on, `out = in * DuckGain` (param id 1,
default 0.25, i.e. −12.04 dB), otherwise `out = in`. No attack or release
curve, so the gain is a closed form: inside a key region (more than `Hold`
frames from its edges) the output RMS is exactly `0.25 ×` the dry RMS; outside
it, exactly the dry RMS.

Why a fixture and not a native compressor:

- **The risk is in the format backends**, not in compressor DSP: CLAP feeding a
  non-main input port, VST3 activating an aux input bus (today deliberately
  off, `twvst3plugin.cc:717-726`). Every real ducking compressor a user loads
  is CLAP or VST3. A native `twPlugin` would gate the ABI and skip exactly the
  code that is new.
- **A native compressor is a product feature** — attack, release, knee, ratio,
  a UI — and deserves its own design. qbx has none today, and this proposal
  would be a strange place to start one.
- **A closed form beats a band.** Attack and release would make the measured
  gain depend on window placement; the hard gate does not.
- **It follows the house rule** that every plugin case loads an in-repo
  fixture, never a real bundle (the `plugin_*` convention), and needs no
  install on CI.

Both fixtures get VST3 twins in `tw303a/plugins/tests/twtestvst3.cpp`
(`TestSplit` with a `kAux` output bus, `TestDuck` with a `kAux` input bus that
it reads only when ACTIVATED, the way `TestSine` ignores an unactivated event
bus, plugins inv. 31). They are driven in `plugins_test` so both backends'
bus handling is gated; the `.qxa` cases use CLAP.

---

## 6. Traps

| # | Trap |
|---|---|
| T1 | **Wiring beside the pass is clobbered** (45 T3, 47 T1). Returns and keys are wired inside `reconnectTracksToMixer()` only, and survive a solo toggle, gated. |
| T2 | **An inert component left connected carries the identical signal** (45 T5, 47 T2). The return's `twTrackMix` is disconnected. |
| T3 | **Invalidating the source alone leaves the return and the keyed track stale** (47 T4, from the other side; §1.4). D7's follow-the-graph walk, gated by an edit after the first render. |
| T4 | **The chain rebuild re-runs series wiring** on every add/remove/reorder of plugins (`twpluginchain.cc:90-133`); port 1 must be re-asserted, or a reorder silently drops the key. Gated: reorder a keyed slot, the duck survives. |
| T5 | **`remove-plugin` + undo makes a new slot** (`sremovepluginaction.cpp:100-112`). Without the `slotId` carried by the inverse, an undo unroutes every return of that slot. Gated. |
| T6 | **A source deleted or renamed** must fail closed and stay closed (47 T6/T7): a later track or slot that happens to get the same id must not inherit the route. |
| T7 | **The depth guard is 64** (`capture_revalidator.cc:466`). Routing lengthens paths: a key from a return of a nested instrument inside folders. A dep past the guard is silently not planned. Measure the deepest realistic project in M4. |
| T8 | **Goldens.** With no return and no key, nothing new is allocated, wired or called; both committed goldens and every `.qxp` stay byte-unchanged. The negative control for the whole proposal. |
| T9 | **A live-owned source answers silence** (`twpluginslotproc.cc:1032-1044`), so an armed instrument's returns go quiet, and a live-owned keyed track's pump has no key path. 47 D9's situation; the answer is the author's (Q1). Until decided, the behaviour is gated as it is and announced once per arm. |
| T10 | **A side page from an older mask.** A render in flight when the mask changes can produce a page whose side page lacks the new bus; the tap renders silence and the epoch bump re-renders. Must not crash, must not read past the side page. Unit-gated. |
| T11 | **Memory.** 256 kB per routed aux channel per cached page (D2). Measure with `twComponent::describePageMemory` on the sampler-shaped case before claiming it is fine. |
| T12 | **XML comments**: no `--` inside a `.qxa` comment (the parse fails on the whole script). |

---

## 7. Out of scope

- **PDC** across an aux or a key (D10).
- **AU** aux outputs and sidechain inputs: AU renders one element today and the
  backend is unverified as a whole (plugins inv. 35). Filed as a follow-up when
  M1 lands; until then an AU plugin's aux buses stay unroutable and the menu
  greys them with the reason.
- **The WideGen surplus** (a main bus wider than the page) stays dropped; it is
  not an aux bus.
- **The live path** beyond announcing its silence, pending Q1.
- **A native compressor or gate** (§5).
- **More than one sidechain bus per plugin.** Rare in practice; the ABI added
  here is bus-indexed, so a later proposal needs no ABI change.
- **The sampler's own routed-output fallback** (DESIGN §3.1, "an aux output
  nobody listens to falls back to main"): this proposal provides the mask,
  generically (`SPluginSlot::routedOutBuses()`, and the processor's
  `setRoutedOutBuses()`); handing it to the sampler's `twSamplerHost` is GATES.md
  Q8.

---

## 8. Milestones

One PR each, in order. Each PR runs `./ci/gates.sh` (the full suite), keeps
the goldens byte-identical, updates the CONTRACT.md of every module it touches
and `docs/ACTIONS.md` for every verb it adds, and has each new case watched
failing first (a sabotage of the line it pins), as the house rules require. A
milestone that finds this document wrong corrects it in place, dated, rather
than editing the error away.

### M0 — The routing graph and invalidation that follows it (no new routing)

Builds D7 on the one edge kind that exists today (send) plus containment, so
M2 and M4 add edge kinds to a tested walk.

- **AC0.1** First, MEASURE: a case renders a project with a send, moves a clip
  on the send's SOURCE, re-renders, and asserts the send lane's contribution
  moved (`assert-file-identical expectReject` over the wet region, from a
  fixture where the dry path is silent). The result, whichever it is, is
  recorded in this file and in thinktank. If it shows the hole, M0 closes it
  with the follow-the-graph walk; if not, M0 records which mechanism already
  covers sends and extends that one instead.
- **AC0.2** The shared walk replaces both send-only copies
  (`ssendtapactions.cpp:64`, `sstdmixer.cpp:433-452`) with no behaviour change:
  `send_cycle_broken`, `send_tap_model`, `send_lane_survives_rewire` and every
  other send case pass unchanged.
- **AC0.3** A unit test drives the walk over synthetic graphs with all four
  edge kinds (aux and key as stub edges), asserting refusal of each cycle shape
  in §2 D7 — including the containment-only ones (key from own folder, return
  inside its source) — and the deterministic drop order.
- **AC0.4** Goldens byte-identical; `./ci/gates.sh` green against the platform
  gate docs.

**M0 as built (2026-10-27, QBX-248).**

- **AC0.1, measured on `main` `a3671ad9` before any change: THE HOLE IS REAL.**
  `send_source_edit_heard.qxa` (a pre-fader send at 0 dB, the source's fader
  at −1000 dB so the dry path is exactly 0.0; render; `move-clip` the source's
  clip from 0 s to 2 s; re-render): the second render's `[0 s, 2 s)` was
  **byte-identical** to the first, RMS 0.23096 where silence was due. The send
  bus and the send lane's chain, gain stage and rewire kept every page frozen
  before the edit. Routing edits were audible only because
  `reconnectTracksToMixer()` re-wires every plug, which re-stamps (this is why
  47 M2 could ablate the bus and lane bumps from `sendRoutingChanged()`); an
  edit on the SOURCE re-wires nothing. No existing mechanism covered sends, so
  M0 built the follow-the-graph walk of D7: after the containment walk under a
  root has found an edit, the root (`SStdMixer::invalidateRoutingDependents`)
  hands the same ranges to the routing dependents of every chain the walk
  bumped, transitively, over the ACCEPTED graph the pass wired. The system-lane
  branch (an edit ON a send lane, which no containment walk finds, 45 D11)
  hands the lane's own dependents the same, so send → send is covered from
  both ends. Five sections of the case cover the range walk, the transitive
  walk, the whole-chain walk, and both system-lane branches; every one failed
  on the baseline.
- **A trap found while measuring:** a −144 dB fader is not a silent dry path
  for a byte comparison. It flips 16-bit LSBs, which alone satisfied the
  case's `assert-file-identical expectReject` over a render that was otherwise
  stale. −1000 dB (1e-50, exactly 0.0 in a float) is.
- **"Only `invalidateRenderPath(Range)` is load-bearing" (D7) does not carry
  over to dependents.** A send lane is in no root's `childLinks()`, so
  `invalidateRenderPath()` on it can only reach its own chain (whole, not
  ranged) and its owner — never its bus, which is the mixer's. The walk
  therefore bumps the dependent's bus and chain over the range directly; each
  of the two was ablated alone and each is load-bearing (see the mixer
  CONTRACT's inv. 29 for the measured sabotages).
- **AC0.2:** the shared walk is `sroute::Graph` (`app/model/sroutinggraph.h`),
  the lowest slice both the verbs and the pass can see. `add-send`/`set-send`
  ask `SStdMixer::routingGraph().wouldCloseCycle()` (the model's edges, every
  tap, cyclic or not); `rewireSendBuses()` wires the subgraph
  `sroute::acceptInOrder` accepts from the same candidate list, in the same
  order as before. Containment edges are in both and can change nothing for
  sends (a send edge only enters a send lane, and a send lane is in no
  folder). Every send case passes unchanged.
- **AC0.3:** `routinggraph_test` (unit), every shape in D7 including the two
  containment-only ones, the not-cycles (sibling key, diamond), and the drop
  order forwards and reversed.

### M1 — Aux buses reach the host (engine only)

D2, D4 (aux rows), §3.1 outputs half; `tw.test.clap.split`, `TestSplit`.

- **AC1.1** `plugins_test`: a `twPluginInsert` running `tw.test.clap.split`
  with bus 1 routed, read through a `twPluginAuxTap` on a 2-channel page:
  the tap's page is **memcmp-equal** to the insert's input page, both channels,
  over a full 65536-frame page at a non-zero position. Same for `TestSplit`
  (VST3).
- **AC1.2** `tw.test.clap.sine`'s mono aux through a tap on a 2-channel page:
  both channels equal (MonoSpread) and each sample exactly `0.5 ×` main
  channel 0 (the fixture's `s * 0.5f`, `twtestclap.c:1065`).
- **AC1.3** With the mask empty, the insert's page has no side page and the
  same byte size as today (`describePageMemory`), and the processor makes the
  legacy call (a counting test double).
- **AC1.4** Changing the mask stales the insert's pages (epoch), and a render
  in flight across the change neither crashes nor reads past the side page (T10).
- **AC1.5** VST3: the routed aux output is activated at `prepare()`, an unrouted
  one is not (asserted through `TestSplit`, which writes its aux only when
  activated).
- **AC1.6** Goldens byte-identical (T8). Scanner version 3 re-probes every
  module exactly once (the 37 P2 gate's shape) and the probe JSON carries
  `nInBuses`/`inBusChannels`.

**M1 as built (2026-10-27, QBX-248).** Every AC holds as written; what the
build found that this document did not say:

- **The bus array's length is a promise, and nobody kept it.** `twPlugin`'s
  event-aware `process()` always documented `outBuses` as `audioOutBusCount()`
  entries, but every caller in the tree passed ONE (both backends' legacy
  overloads, the processor's automation path, the generator path), harmless
  only while nothing read past entry 0. §3.1's "a backend substitutes its own
  scratch for a null entry" is therefore not enough on its own: a backend now
  reads `outBuses[b]` only for a bus in the mask it was given
  (`twPlugin::setRoutedOutBuses`, new), and once any bus is routed every
  event-aware call carries a full-length array (the processor's
  `busArrayForChunk_nolock`, a member array in each legacy overload).
  plugins CONTRACT inv. 61.
- **The mask applied to the plugin is the set the slot renders, not the set
  asked for.** A DualMono, Transparent, Unsupported or Missing slot keeps its
  mask (it comes back with the plugin, D9) but tells the plugin nothing, so no
  backend is promised an array the slot will not hand it. inv. 60.
- **The width-1 page needed a hook.** At a project width of 1,
  `freezePage_nolock` called `renderFrames()` with a bare channel pointer, which
  cannot carry a side page. `twComponent::renderPageNarrow( page, ... )` now
  stands there, defaulting to exactly that `renderFrames()` call; only the
  insert and the tap override it. graph CONTRACT inv. 11.
- **CLAP and VST3 `audioOutBus(i)` were PORT-indexed**, which agreed with
  twPlugin's "bus 0 is the main bus" only while the main port was port 0. They
  are bus-indexed now (and the new `audioInBus(i)` likewise), part of why the
  scanner went to version 3. D5's input ABI is otherwise M3's: only the bus
  REPORTING (`audioInBusCount()`/`audioInBus(i)`) landed here, because AC1.6's
  `nInBuses` needs it.
- **AC1.5 needed a plugin-side witness for "not activated".** An unrouted bus
  gets the backend's scratch, so its audio can never show whether it was
  switched on. `TestSplit` reports its aux activation as one frame of tail
  (`getTailSamples()`, which `twPlugin::tailFrames()` reads live); the routed
  half is shown by the aux audio itself, which TestSplit writes only when
  activated.
- **The tap's pages are stamped against the tap's own epoch** (inv. 15's
  rule), so a mask change stales the insert and not the tap. Harmless in M1
  (nothing re-points a tap); M2's pass must re-wire the tap when it re-points
  it, which re-stamps.
- **Measured (T11):** a stereo routed aux costs exactly one more 2-channel
  page per cached page of the insert: 1,048,576 B for main + side against
  524,288 B unrouted, now counted in the insert's page statistics
  (`twOutputPage::accountedBytesWithAux`) and so in `describePageMemory`.
- **T10, measured as a race:** a tap rendered 60 times while another thread
  flipped the routed bus between 1 and 2 (~25,000 flips per run, three runs):
  every page was the exact aux or silence (e.g. 32 / 28 / 0 other).

### M2 — Return tracks: model, verbs, persistence (X2.1)

D1, D3, D6 (second half), D8, D9; §3.2 return half; the aux edge in D7.

- **AC2.1 = GATES.md X2.1.** `return_track_split_cmp.qxa`: track A plays
  `test_sawtooth.wav` (or another committed stereo fixture) through
  `tw.test.clap.split` with `MainLevel` 0; `add-return-track trackPath="0"
  slot="0" bus="1"`; render. A reference project plays the same clip on a plain
  track with no plugin; render. **`assert-file-identical` passes** on the two
  files: the return track's render equals the bus's signal, byte for byte.
  Watched failing with the tap reading bus 0's side-page offset +1 channel and
  with the return's `twTrackMix` left connected.
- **AC2.2** `return_track_instrument.qxa`: `tw.test.clap.sine`, NoteOn vel 100,
  its aux routed to a return; with the source track muted (D6), the
  render's RMS per channel is `0.5 × 100/127/√2` ±1 %, and both channels are
  equal (MonoSpread).
- **AC2.3** Source edit after the first render: move A's clip, re-render, the
  return's region moves with it (T3).
- **AC2.4** Muting A leaves the return audible at the same RMS; soloing the
  return silences A's main and keeps the return (D6, user-track solo).
- **AC2.5** Round trip: save, reload, render byte-identical; a project with no
  return re-saves byte-identically (T8). `remove-plugin` + undo keeps the
  return routed (T5); `reorder-plugin` keeps it routed; deleting the source
  track makes the return silent and announced, and undo restores the route (T6).
- **AC2.6** Every refusal in §3.2 has a case with `expectReject="true"` whose
  log assertion matches a message ONLY that refusal produces.
- **AC2.7** A solo toggle elsewhere leaves the return wired (T1), asserted with
  `assert-return-track routed=1` reading the live plug.

**M2 as built (2026-10-27, QBX-251).** Every AC holds as written (numbers in
the PR and in thinktank); what the build found that this document did not say:

- **A routing edit on a return needs `invalidateRenderPath()` on the return,
  not only a re-wire.** The pass re-points the tap on every run, which
  re-stamps the tap, the return's chain, fader and rewire (each `setInput`
  bumps its consumer) -- and NOT the master lane chain and mixer rewire behind
  them. Measured: deleting a return's source track ran the pass, nulled the
  tap's input, announced the return silent, and the next render was
  **byte-identical to the routed one**. The pass now invalidates a return's
  render path whenever its route moved (a different source insert, or a new
  tap). 47 M2's lesson, paid again for the aux edge.
- **The pass must not route returns while a project is loading.** Tracks are
  inserted, and the pass runs, before the deferred resolvers have given a
  return its source or a source its plugin chain, so it announced loaded
  returns silent that were not. The return half skips while
  `SProject::isLoading()`, and a post-load pass registered from
  `objects/mixer` (`SProjectLoader::registerPostLoadPass`, the take-column
  pass's mechanism) routes every loaded return once, with every chain
  adopted. D8 said "resolved by `deferResolve` exactly as `pluginChainId`
  is"; that holds for the TRACK, while the SLOT needs no lookup at all -- it
  is a number the pass finds in the source's chain.
- **`slotId` is minted random (63 bits), not max+1**, and is written from the
  first time something names the slot, staying written after the return goes
  ("written only once something references it" read literally). A counter
  would hand a new slot the number of a removed one that a return -- alive on
  the undo stack -- still names, and the return would re-attach to it (T6).
  remove-plugin's inverse carries the id (`insert-plugin slotId=`), as D8
  required.
- **An unroutable return keeps the tap as its chain's input, with nothing
  wired into it** -- silent, not its own (empty) clip mix. A source that did
  not resolve on load keeps the bus with no track, so the return stays a
  return and fails closed, and re-saves as `track='0'`.
- **The master as a source is refused by ROLE**, as add-send refuses it: the
  routing graph has no edge for the master sum, so the walk cannot see that
  master -> return closes a cycle. A SEND lane may be a source (its return is
  appended at the top level); a system lane may never be the return.
- **A return that sits in another folder than its source** is reached by the
  D7 walk over the aux edge as a containment walk from the root TO the
  return, which also bumps every folder above it; whatever that walk bumps is
  followed on, so a send FROM a return is covered.
- **AC2.1's fixture is `test_stereo.wav`, not `test_sawtooth.wav`.** Every saw
  fixture in `tests/` has byte-identical channels, and against one the
  required sabotage (the tap reading its bus one channel to the right) went
  UNBITTEN: the cmp passed. `test_stereo.wav`'s channels differ (RMS 0.5 and
  0.25), and with it both required sabotages bite.
- **§5 named two fixtures; M2 needed a third.** D9's DualMono refusal needs a
  1-in/1-out plugin and none existed, so `tw.test.clap.monosplit` is the split
  with every port mono.
- **Q1 (a) is announced in the pass, once per arm** -- `[RETURN] '<return>' is
  silent while its source '<source>' is armed`; the return stays WIRED through
  the arm (the slot renders only for the pump), gated by
  `return_track_live_source` on the wiring and the log, as sends are.
- **T1 for returns is weaker than for sends.** A return's wiring is internal to
  the return track (tap -> chain -> fader -> rewire); the master half of the
  pass re-sets only the return's root in the master sum, so nothing the pass
  does elsewhere can clobber it. AC2.7 is gated as written; the sabotage that
  bites it is the plausible regression, applying the send's audibility rule
  to returns, under which a solo elsewhere unwires the return.

### M3 — Sidechain inputs (engine only)

D5's ABI and insert half, D4 (key rows); `tw.test.clap.duck`, `TestDuck`.

- **AC3.1** `plugins_test`: `tw.test.clap.duck` on a stereo insert, a constant
  sine as main input, a key page that is a sine burst from frame 16384 to
  40960. RMS over frames `[16384+2048, 40960)` = `0.25 × dry` ±0.5 %; RMS over
  `[0, 16384)` and over `[40960+2048, 65536)` = dry ±0.5 %. Same for `TestDuck`
  (VST3).
- **AC3.2** The same slot with port 1 unwired is byte-identical to today's
  insert (a 2-in/2-out effect golden).
- **AC3.3** VST3 activates the aux INPUT bus only when keyed; `TestDuck` with
  the bus left inactive does not duck (the gate has teeth, inv. 31's pattern).
- **AC3.4** Key width mapping: a mono key into the stereo sidechain duplicates,
  a stereo key into a mono sidechain bus averages (a synthetic plugin in
  `test_plugin_insert.cc`, as WideGen is gated there).
- **AC3.5** Goldens byte-identical.

**M3 as built (2026-10-27, QBX-252).** Every AC holds as written, at the
closed form: the duck is a hard gate, so the measured ratios are exact. What
the build found that this document did not say:

- **Measured.** AC3.1, out/dry RMS on both channels, CLAP `tw.test.clap.duck`
  and VST3 `TestDuck` alike: **0.25** over `[18432, 40960)`, **1** over
  `[0, 16384)` and over `[43008, 65536)` (the bound is ±0.5 %; the gate is
  `out = in * 0.25f`, so nothing is left to round). AC3.2: the unwired slot's
  page is memcmp-equal to its input, and a counting double sees only the
  legacy `process()` call, no `setRoutedInBuses()`, and one planned
  dependency (two once keyed). AC3.3: `TestDuck` reports its sidechain
  activation as one frame of tail (0 unkeyed, 1 keyed, 0 again), and down the
  deliberately broken host path it measures out/dry **1** inside the key
  window. AC3.4: MEAN and DUPLICATE are bit-exact against the page (and COPY,
  the refused row, D9 and a generator are gated beside them). AC3.5: the
  committed goldens and the full suite pass unchanged.
- **The bus-aware call is `processBuses()`, not a third `process`.** D5 named
  it `process( inBuses, outBuses, ... )`. The generator path passes a literal
  `nullptr` for `in`, and two pointer overloads make that ambiguous; a backend
  overriding one `process` would also hide the other. Its default forwards
  `inBuses[0]` to the event-aware call, as D5 asked. `setRoutedInBuses( mask )`
  is the input twin of M1's `setRoutedOutBuses`.
- **The key is wired through one call, `twPluginInsert::setKeySource( component )`**,
  which wires port 1 AND tells the processor the key's width
  (`twPluginSlotProcessor::setKeyChannels`). The width is the key PRODUCER's
  `getOutputChannels()`, so "chosen once per wiring" (inv. 7) is literal: the
  D4 row is decided there and re-decided on a rebuild, never per page. A key
  gathered under another width than the one decided (a rewire racing a
  render) is fed as silence. M4's mixer pass calls `setKeySource` with
  `STrack::sendTapComponent( preFader )`.
- **`setInput()` keeps one teardown "parent" per component**, and it is the
  producer of whatever was wired last; unwiring clears it. A key wired after
  the main input would have made teardown call `removeInput(1)` on the KEY's
  source, so `setKeySource` preserves the main input's bookkeeping (gated by
  a wire + unwire + teardown).
- **A change of key RESETS the plugin**, not only re-prepares it. D5 and the
  M1 mask both say "the DSP does not continue across it", but a `prepare()`
  at an unchanged rate is a no-op in CLAP and `haveLastEnd_ = false` resets
  nothing on the effect path: a CLAP duck keyed by a loud key and then
  unkeyed ducked the first 2048 frames of the unkeyed render. `setKeyChannels`
  now calls `reset()` on every instance when the applied key changes.
  **M1's `setRoutedOutBuses` has the same shape** (re-prepare, no reset) and
  was left as it is: a routed aux bus changes no DSP state in any fixture, so
  nothing measures it. Recorded here for whoever touches it next.
- **AC3.3's "gate with teeth" is an env knob**, invariant 31's pattern:
  `SMARAGD_VST3_NO_SIDECHAIN_BUS=1` makes `prepare()` skip the activation of a
  fed input while still handing it the key, and `TestDuck` (which reads its
  sidechain only when activated) then does not duck.
- **A keyed generator is fed too** (D9 lists the generator rows): bus 0 is
  null, bus 1 the key. A pre-roll and a live catch-up run with no key in
  hand, so their sidechain hears silence; a key-dependent instrument's
  pre-roll is therefore not the state a continuous run would reach. No
  fixture is such an instrument.
- **The insert now reports two input ports** (`getNInputs() == 2`, port 1
  `kKeyPort`). An unwired port 1 is a null plug that nothing plans, negotiates
  or reads; `plugins_test`'s port-count check moved from 1 to 2.
- **Not gated:** the processor's own epoch bump in `setKeyChannels`. On the
  one path that reaches it (`setKeySource`), `setInput()` has already bumped
  the insert's epoch, so removing it breaks no case; it covers only the race
  of a render between the two calls. Kept, and recorded as not gated.

### M4 — Sidechain: model, verbs, persistence (X2.2)

D5's model half, D6 (first half), D7's key edge, D8 (`<sidechain>`).

- **AC4.1 = GATES.md X2.2.** `sidechain_duck_from_return.qxa`: track B plays a
  steady stereo fixture through `tw.test.clap.duck`; track A's
  `tw.test.clap.split` routes its aux to return track R, and A plays a key clip
  over a known region; `set-plugin-sidechain trackPath="<B>" slot="0"
  source="<R>"`; render. Measured on the file with `assert-audio-energy`
  (`channel=0`, then `channel=1`) over a window inside the key region (more
  than 2048 frames from its edges): B's RMS is **`0.25 ×` its dry RMS ±1 %**
  (−12.04 dB). Over a window with no key: B's RMS equals the dry RMS ±1 %.
  **And not without it:** the same project with `clear-plugin-sidechain`, and
  again with the key clip removed, measures dry ±1 % in the key window. (The
  return's own output is silenced in the case, `MainLevel` 0 on A and a muted
  R, so the file holds B alone; muting R must not stop the key, which is D6.)
- **AC4.2** Soloing B keeps the ducking (D6); `tap="post"` with R's fader at
  −inf stops it, `tap="pre"` does not.
- **AC4.3** A key edit after the first render (move the key clip) moves the
  duck (T3, through two routing edges: aux then key).
- **AC4.4** `reorder-plugin` on B's chain keeps the key (T4); `remove-plugin` +
  undo keeps it (T5); save/reload round-trips it and re-renders byte-identically.
- **AC4.5** Cycles refused at the verb, each shape a case: B keyed from itself,
  from its folder, R's source keyed from R, a send loop closed by a key; and a
  hand-edited `.qxp` carrying one renders with exactly one edge dropped and a
  `[ROUTE] cycle` warning (47 M3's shape).
- **AC4.6** Depth: the deepest case of the suite plans every dep — no node
  dropped by the 64 guard (T7) — asserted from the revalidator's counters.

**M4 as built (2026-11-01, QBX-262).** Every AC holds as written, at the
closed form; what the build found that this document did not say:

- **Measured.** AC4.1 (`sidechain_duck_from_return`), B's RMS inside the key
  window `[28000, 116000)`: **0.124997** on channel 0 and **0.062499** on
  channel 1, against a dry **0.499987** / **0.24999** before and after the key
  (`[0, 23000)`, `[124000, 144000)`): out/dry **0.25000** on both channels,
  the band ±1 %. Cleared, and again with the key clip removed: **0.499987** /
  **0.24999** inside the key window. AC4.2: soloing B renders byte-identical
  to the unsoloed render; `post` with R's fader at −1000 dB measures dry
  (0.499987), `pre` ducks (0.124997, byte-identical to the reference). AC4.3:
  after moving the key clip from frame 24000 to 72000, `[28000, 70000)` is
  dry and `[76000, 144000)` ducked. AC4.4: reorder, remove + undo, the key
  lane deleted + undo, and save + reload each render byte-identical to the
  reference; the deleted key lane renders dry. AC4.5: the four cycle shapes
  are refused at the verb with distinct messages, and the hand-edited
  `tests/sidechain_cycle.qxp` renders **2A = 0.461913** with exactly the key
  edge dropped, and **A = 0.230956** once the other edge is cleared and the
  freed key edge wired. AC4.6: see the depth note below.
- **The key lives on the SLOT, the seams on the TRACK.** `SPluginSlot` holds
  the source (`QPointer<SObject>`), the tap point and a "set" flag (a source
  that did not resolve on load keeps the slot keyed with a null source, so it
  fails closed and re-saves as `source='0'`, as D8 asks of the aux source).
  `objects/mixer` may not name tw/plugins, so the pass calls
  `STrack::wireSlotKey()`, which calls `SPluginSlot::applyKey()`, which calls
  M3's `twPluginInsert::setKeySource()` with `STrack::sendTapComponent( pre )`
  as M3 predicted.
- **The wiring is IDEMPOTENT, and it has to be.** M3 made a key change stale
  the insert AND reset the plugin, so a pass that re-asserted port 1
  unconditionally (the return half re-points its tap on every pass) would
  re-render every keyed slot from its first frame on every solo toggle.
  `applyKey()` calls `setKeySource()` only when the plug does not already read
  that producer at that width, and holds the producer while it is wired
  (`keyHold_`), so a key lane deleted off the undo stack cannot leave port 1
  plugged into a dead latch (47's unwireSendLane lesson).
- **A key change needs `invalidateRenderPath()` on the keyed track**, M2's
  lesson for the aux edge paid again: `setKeySource()` stales the insert only,
  and the chain, fader, rewire, folders and master above it would serve the
  old key.
- **T4's premise does not hold today.** "The chain rebuild re-runs series
  wiring; port 1 must be re-asserted, or a reorder silently drops the key" —
  `rebuildWiring_nolock()` wires port 0 only and never touches port 1, so a
  reorder keeps the key with no help. Measured: removing the routing pass a
  reorder asks for left `sidechain_persistence` green. The pass is kept as the
  defence (it is what "all routing wiring happens in the pass" means), and the
  case bites a rebuild that DOES drop port 1 only together with that removal.
- **Key edges come LAST in the routing graph** (after containment, send and
  aux), so a cycle that mixes kinds loses its key edge and keeps the sends and
  returns that were there first; announced as `[ROUTE] cycle: the key edge
  from '<source>' into '<track>' is DROPPED`. The verb's cycle check asks the
  model's graph with the slot's CURRENT key edge still in it, which is exact:
  an edge entering the keyed track cannot lie on a path out of it.
- **The invalidation walk follows a key edge as an aux edge** (a containment
  walk from the root to the keyed track; the keyed insert sits inside its
  chain, whose invalidation forwards to its inserts), a key on a SEND LANE
  through the send-lane branch, and a key on the MASTER LANE through the
  master chain, which the root's own bump already covers.
- **Any lane with a chain may be keyed**, the master lane's and a send lane's
  inserts included (D5 names the SOURCE kinds and is silent on the keyed
  one); the master and the conductor are refused as a SOURCE by role.
- **Two refusals §3.2 did not list**: a plugin with no sidechain input bus
  (the M3 engine would ignore the key, logged once; the verb says so up
  front, as `add-return-track` refuses a bus the plugin does not have), and a
  `tap` other than `pre`/`post`.
- **`remove-plugin`'s inverse carries the sidechain** (T5) as live state on
  `insert-plugin` (`SInsertPluginAction::setSidechain`), not serialized: the
  source is an object that may have left every root, which no path spells.
- **Q1 (a) for keys** is announced once per arm, for an armed keyed track or
  an armed key lane; the key stays wired. **Not gated**: like
  `return_track_live_source` it needs a real input backend to make a lane
  live-owned, and M4 added no live case.
- **Depth (T7, AC4.6).** The planner's guard counted nothing, so M4 added
  `graphStats().depthDrops` and `maxDepth` and `assert-render-policy
  maxDepthDrops= minPlanDepth=`. Measured over the whole suite (one ctest
  `-j4` run with a temporary per-process dump): **no case drops a
  dependency**; the deepest is the new `sidechain_depth` at **27** (a key from
  the return of an instrument three folders deep into a track two folders
  deep, unity gains on every level, a send lane behind it), the deepest
  before M4 `group_nested_track` at **16**. The guard is 64. Watched failing
  with the guard lowered to 20.

### M5 — UI

§3.3.

- **AC5.1** `describeRowMenu()` lists `outputs.aux1=1` (and the rest) for
  `tw.test.clap.split`, absent for a plugin with no aux bus, greyed with the
  reason on a DualMono or AU slot; `triggerRowMenuItem("outputs.aux1")` creates
  the return through the verb (undoable), and the menu then shows it checked.
- **AC5.2** `triggerRowMenuItem("sidechain.<lane>")` keys the slot; the row
  reports the key; "None" clears it.
- **AC5.3** The return's head and strip label name the source, elide in a
  narrow strip, and carry the full text in the tooltip (layout structure
  asserted, not geometry).
- **AC5.4** UI-only: goldens byte-identical by construction.

### After M5

GATES.md X2 is met. Q8 (the sampler's multiple outputs) can start; its
`setRoutedOutputs(mask)` reads `SPluginSlot::routedOutBuses()`.

---

## 9. Open questions (the author's)

- **Q1 — The live path.** While a source track is armed (live-owned), its slot
  answers every frozen render with silence (`twpluginslotproc.cc:1032-1044`),
  so **its return tracks go silent for the arm**, and a live-owned keyed track
  has no key. 47 D9 accepted the same for sends ("a live lane does not feed a
  send"). For NassauDigital this means playing pads live with the kick on aux 1
  would drop the kick from the return until the arm ends (DESIGN §9.4 makes
  live play a first-class use). Options: **(a)** accept and announce, as 47 D9;
  **(b)** extend the live pump to render the source's routed aux buses into the
  return's ring, which is proposal 21's "second component graph in the pump"
  that 47 D9 (b) rejected for sends; **(c)** (a) now, (b) as its own proposal
  before Q8. This proposal implements (a) unless told otherwise.
- **Q2 — Make returns automatically?** Inserting a plugin that declares aux
  buses could create its return tracks at once (eight for the sampler), or
  only when asked (Outputs ▸ per bus, as designed). The sampler's fallback to
  main makes "only when asked" safe.
- **Q3 — Solo of the source.** Soloing the instrument track: should its return
  tracks stay audible (implicitly soloed, the way a folder containing a soloed
  lane stays wired), or follow the solo rule literally and go quiet? Literal is
  what a user track does by default and what this proposal builds; implicit
  needs `ssolo::isLaneAudible` to learn about aux edges.
- **Q4 — More than one return per bus.** Refused here (D9) in favour of sends
  from the return. Say so if parallel returns of one bus are wanted.
