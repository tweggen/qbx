# ACTIONS — the command surface (generated reference)

Every user-visible mutation goes through an `SAction` (see
`smaragd/main/actions/CONTRACT.md`). This maps each registered verb to its
class, source location, and XML attributes, for writing `.qxa` scripts and
headless tests. Keep it truthful when adding or changing an action.

Conventions (see also `smaragd/main/testkit/CONTRACT.md`):
- positions/durations are FRAMES at the project rate; fraction strings like
  `"48000/1"` parse via `parseFractionOrDouble`.
- clip/track paths are comma-separated child indices from the root mixer,
  e.g. `clip="0,1"` = track 0, child 1.
- a rejected `apply()` FAILS a headless test unless the action element has
  `expectReject="true"`.

| Verb | Class | Source (under smaragd/main/) | Attributes (name = default) |
|---|---|---|---|
| `add-sample` | SAddSampleAction | objects/cut/src/saddsampleaction.cpp | `trackPath` = "0" (index-path from the root mixer, so a lane nested in a folder track is addressable; the legacy top-level-only `trackIndex` is still accepted on read), `filePath` = "", `timePos` = "0" |
| `add-take` | SAddTakeAction | objects/cut/src/saddtakeaction.cpp | `clip`, `filePath`, `startOffset` = "0", `index` = "-1", `activate` = "1", `stretch` = "1.0", `pitchCents` = "0" |
| `add-to-selection` | SAddToSelectionAction | selection/src/saddtoselectionaction.cpp | `paths` = "" |
| `add-track` | SAddTrackAction | objects/mixer/src/saddtrackaction.cpp | `index` = "-1" |
| `assert-audio-energy` | SAssertAudioEnergyAction | testkit/src/sassertaudioenergyaction.cpp | `filename` = "", `minRms` = "0.01", `maxRms` = "0.95", `startFrame` = "0", `frameCount` = "-1", `channel` = "-1" |
| `assert-audio-frequency` | SAssertAudioFrequencyAction | testkit/src/sassertaudiofrequencyaction.cpp | `filename` = "", `minHz` = "0", `maxHz` = "0", `startFrame` = "0", `frameCount` = "-1", `channel` = "-1" (autocorrelation f0 — the pitch gate) |
| `assert-audio-peak` | SAssertAudioPeakAction | testkit/src/sassertaudiopeakaction.cpp | `filename` = "", `maxPeak` = "0.95", `startFrame` = "0", `frameCount` = "-1", `channel` = "-1" |
| `assert-meter` | SAssertMeterAction | testkit/src/smetertestactions.cpp | `trackIndex` = "0", `position` = "0" (project frames), `minRms`/`maxRms`/`minPeak`/`maxPeak` = "-1" (< 0 = not checked), `expectMiss` = "false", `headHeight` = "0", `contains` = "" — freezes the page covering `position` itself (no transport needed) and runs the production `twLevelProbe`; with `headHeight` > 0 it also builds the REAL track head off screen via `SMainWindow::describeTrackMeter` and matches `SLevelMeter::describe()`, which reads `vis=…\|orient=…\|len=…\|peak=…\|rms=…\|hold=…\|clip=…\|db=…`. Measures on the LEGACY PULL path, so set a track's gain BEFORE first probing a position (see the caveat in smetertestactions.cpp) |
| `assert-plugin-strip` | SAssertPluginStripAction | testkit/src/spluginuitestactions.cpp | `trackPath` = "" (index-path from the root mixer — the only way to reach a track NESTED in a folder; falls back to `trackIndex` when absent), `trackIndex` = "0" (legacy top-level), `slotCount` = "-1" (rows the FX strip rendered), `slotIndex` = "-1", `contains` = "", `absent` = "" — builds the REAL `SPluginEffectStrip` off screen and matches `describeSlot(slotIndex)`, which reads `name=…|state=…|mode=…|bypass=…|nameEnabled=…|bypassEnabled=…|editEnabled=…|reload=…|trackPath=…|tooltip=…`. `trackPath=` is the strip's RESOLVED track path: it is the strip's single point of failure (every button handler early-returns when it is empty, so a wrong answer disables the whole strip silently) and is otherwise invisible from outside |
| `clear-selection` | SClearSelectionAction | selection/src/sclearselectionaction.cpp | (none) |
| `create-asset` | SCreateAssetAction | objects/mixer/src/screateassetaction.cpp | `container`, `startOffset` = "0", `duration` = "0", `assetName` |
| `cycle-disable` | SCycleAction | actions/src/scycleaction.cpp | (none) |
| `cycle-enable` | SCycleAction | actions/src/scycleaction.cpp | (none) |
| `cycle-toggle` | SCycleAction | actions/src/scycleaction.cpp | (none) |
| `drag-clip-edge` | SDragClipEdgeAction | testkit/src/sdragclipedgeaction.cpp | `track` = "0", `clip` = "0", `edge` = "end" / "start" / "body", `toTime`, `half` = "lower" (or "upper" = the loop half), `modifiers` = "" ("ctrl"/"alt"/"shift", "+"-joined) — drives the REAL mouse handlers, the only way to test clip gestures. `edge="body"` is required for slip/duplicate/move; drop is pixel-quantised, so assert on ranges |
| `duplicate-clip` | SDuplicateClipAction | objects/cut/src/sduplicateclipaction.cpp | `source`, `destTrack`, `startTime` = "0" |
| `grid-disable` | SGridAction | actions/src/sgridaction.cpp | (none) |
| `grid-enable` | SGridAction | actions/src/sgridaction.cpp | (none) |
| `grid-toggle` | SGridAction | actions/src/sgridaction.cpp | (none) |
| `insert-plugin` | SInsertPluginAction | objects/track/src/sinsertpluginaction.cpp | `trackPath`, `slotIndex` = "0", `format`, `uid`, `name`, `vendor`, `path` = "", `nIn` = "0", `nOut` = "0", `state` = "" (base64 plugin state chunk, written only when non-empty; this is what makes remove-plugin's inverse restore the user's parameters) |
| `load-project` | SLoadProjectAction | persistence/src/sloadprojectaction.cpp | `path` = "" |
| `metronome-disable` | SMetronomeAction | actions/src/smetronomeaction.cpp | (none) |
| `metronome-enable` | SMetronomeAction | actions/src/smetronomeaction.cpp | (none) |
| `metronome-toggle` | SMetronomeAction | actions/src/smetronomeaction.cpp | (none) |
| `move-clip` | SMoveClipAction | objects/track/src/smoveclipaction.cpp | `clip`, `destTrack`, `startTime` = "0", `broadcast` = "1" (edit groups; same-track moves only) |
| `move-track` | SMoveTrackAction | objects/mixer/src/smovetrackaction.cpp | `source`, `toIndex` = "-1" |
| `place-asset` | SPlaceAssetAction | objects/mixer/src/splaceassetaction.cpp | `assetName` = "", `trackPath` = "", `timePos` = "0" |
| `place-clip` | SPlaceClipAction | objects/cut/src/splaceclipaction.cpp | `trackPath`, `filePath`, `timePos` = "0", `startOffset` = "0", `duration` = "0" (0 = full wave) |
| `place-recording` | SPlaceRecordingAction | objects/cut/src/splacerecordingaction.cpp | `trackPath`, `filePath`, `timePos` = "0" (plans takes for covered columns + plain cuts for gaps; one atomic composite) |
| `plugin-editor-set-param` | SPluginEditorSetParamAction | testkit/src/spluginuitestactions.cpp | `trackIndex` = "0", `slotIndex` = "0", `paramId` = "0", `value` = "0" — drives the `SPluginParamEditor` slider a double-click opens (never shown), so the resulting `set-plugin-param` is what lands on the undo stack |
| `remove-asset` | SRemoveAssetAction | objects/mixer/src/sremoveassetaction.cpp | `assetName` |
| `remove-from-selection` | SRemoveFromSelectionAction | selection/src/sremovefromselectionaction.cpp | `paths` = "" |
| `remove-plugin` | SRemovePluginAction | objects/track/src/sremovepluginaction.cpp | `trackPath`, `slotIndex` = "0", `format`, `uid`, `name`, `vendor`, `path` = "", `nIn` = "0", `nOut` = "0", `state` = "" (captured in `apply()` from the live plugin and handed to the inverse; a hand-written element carries none) |
| `remove-sample` | SRemoveSampleAction | objects/cut/src/sremovesampleaction.cpp | `trackPath` = "0" (index-path from the root mixer, so a lane nested in a folder track is addressable; the legacy top-level-only `trackIndex` is still accepted on read), `clipIndex` = "0", `filePath` = "", `timePos` = "0" |
| `remove-take` | SRemoveTakeAction | objects/cut/src/sremovetakeaction.cpp | `clip`, `take` = "0", `thenActivate` = "-2" |
| `remove-track` | SRemoveTrackAction | objects/mixer/src/sremovetrackaction.cpp | `index` = "0" |
| `render` | SRenderAction | actions/src/srenderaction.cpp | `filename` = "", `format` = "wav", `quality` = "10" |
| `reorder-plugin` | SReorderPluginAction | objects/track/src/sreorderpluginaction.cpp | `trackPath`, `fromIndex` = "0", `toIndex` = "0" (both validated against the chain; the inverse is the reverse move, not a swap) |
| `reparent-track` | SReparentTrackAction | objects/mixer/src/sreparenttrackaction.cpp | `source`, `destParent`, `destIndex` = "-1" |
| `resize-clip` | SResizeClipAction | objects/cut/src/sresizeclipaction.cpp | `clip`, `startTime` = "0", `startOffset` = "0", `duration` = "0", `loopLength` = "0", `stretch` = "1.0", `take` = "-1" (stacks: which take the slip targets), `broadcast` = "1" (edit groups) |
| `save-project` | SSaveProjectAction | persistence/src/ssaveprojectaction.cpp | `path` = "" |
| `select-take` | SSelectTakeAction | objects/cut/src/sselecttakeaction.cpp | `clip`, `take` = "-1", `broadcast` = "1" (edit groups: same take index on every member) |
| `screenshot` | SScreenshotAction | testkit/src/sscreenshotaction.cpp | `filename` = "", `resolution` = "100%" |
| `set-clip-name` | SSetClipNameAction | objects/cut/src/ssetclipnameaction.cpp | `clip`, `name` = "" (ABSOLUTE; the SObject SName drawn on the clip body), `take` = "-1" (stacks: which take is renamed; the name is per-take), `broadcast` = "1" (edit groups) |
| `set-edit-group` | SSetEditGroupAction | objects/track/src/seteditgroupaction.cpp | `trackPath`, `group` = "0" (0 = ungrouped) |
| `set-formant-preserve` | SSetFormantPreserveAction | objects/cut/src/ssetformantpreserveaction.cpp | `clip`, `on` = "0" (ABSOLUTE; keeps the spectral envelope fixed while the vocoder's pitch stage moves the harmonics), `take` = "-1" (stacks: which take is flagged; the flag is per-take), `broadcast` = "1" (edit groups) |
| `set-pitch` | SSetPitchAction | objects/cut/src/ssetpitchaction.cpp | `clip`, `cents` = "0" (ABSOLUTE, clamped to ±2400), `take` = "-1" (stacks: which take is transposed; pitch is per-take), `broadcast` = "1" (edit groups) |
| `set-plugin-bypass` | SSetPluginBypassAction | objects/track/src/ssetpluginbypassaction.cpp | `trackPath`, `slotIndex` = "0", `bypassed` = "false" (ABSOLUTE, not a toggle) |
| `set-plugin-param` | SSetPluginParamAction | objects/track/src/ssetpluginparamaction.cpp | `trackPath`, `slotIndex` = "0", `paramId` = "0", `value` = "0" (ABSOLUTE, clamped to the plugin's declared range; an unknown `paramId` is REJECTED; coalesces by (trackPath, slotIndex, paramId) so one slider drag is one undo entry; refused on a Missing/Unsupported slot) |
| `set-property` | SSetPropertyAction | actions/src/ssetpropertyaction.cpp | `key`, `value` |
| `set-selection` | SSetSelectionAction | selection/src/ssetselectionaction.cpp | `paths` = "" |
| `set-track-mute` | SSetTrackMuteAction | objects/track/src/ssettrackmuteaction.cpp | `trackPath` (index-path from the root mixer, so a lane nested in a folder track is addressable; the legacy top-level-only `trackIndex` = "0" is still accepted on read and wins only when no path is given), `muted` = "0" (ABSOLUTE, not a toggle) |
| `set-track-solo` | SSetTrackSoloAction | objects/track/src/ssettracksoloaction.cpp | `trackPath` (index-path from the root mixer — solo on a NESTED lane is the case this verb exists for), `solo` = "0" (ABSOLUTE, not a toggle; audibility is resolved by app/model/ssolorules.h at every summing container) |
| `set-track-volume` | SSetTrackVolumeAction | objects/track/src/ssettrackvolumeaction.cpp | `trackPath` = "0" (index-path from the root mixer, so a nested lane's fader is addressable; the legacy top-level-only `trackIndex` is still accepted on read), `volume` = "0" (dB). Coalesces consecutive drags on the same lane — `mergeKey` is the PATH, so two faders at the same index in different folders no longer merge into one undo step |
| `snap-to-grid-disable` | SSnapToGridAction | actions/src/ssnaptogridaction.cpp | (none) |
| `snap-to-grid-enable` | SSnapToGridAction | actions/src/ssnaptogridaction.cpp | (none) |
| `snap-to-grid-toggle` | SSnapToGridAction | actions/src/ssnaptogridaction.cpp | (none) |
| `split-clip` | SSplitClipAction | objects/cut/src/ssplitclipaction.cpp | `clip`, `splitTime` = "0", `broadcast` = "1" (edit groups) |
| `toggle-playback` | STogglePlaybackAction | actions/src/stoggleplaybackaction.cpp | `play` = "0" |
| `toggle-selection` | SToggleSelectionAction | selection/src/stoggleselectionaction.cpp | `paths` = "" |
