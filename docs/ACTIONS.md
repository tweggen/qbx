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
| `add-sample` | SAddSampleAction | objects/cut/src/saddsampleaction.cpp | `trackIndex` = "0", `filePath` = "", `timePos` = "0" |
| `add-take` | SAddTakeAction | objects/cut/src/saddtakeaction.cpp | `clip`, `filePath`, `startOffset` = "0", `index` = "-1", `activate` = "1", `stretch` = "1.0", `pitchCents` = "0" |
| `add-to-selection` | SAddToSelectionAction | selection/src/saddtoselectionaction.cpp | `paths` = "" |
| `add-track` | SAddTrackAction | objects/mixer/src/saddtrackaction.cpp | `index` = "-1" |
| `assert-audio-energy` | SAssertAudioEnergyAction | testkit/src/sassertaudioenergyaction.cpp | `filename` = "", `minRms` = "0.01", `maxRms` = "0.95", `startFrame` = "0", `frameCount` = "-1", `channel` = "-1" |
| `assert-audio-frequency` | SAssertAudioFrequencyAction | testkit/src/sassertaudiofrequencyaction.cpp | `filename` = "", `minHz` = "0", `maxHz` = "0", `startFrame` = "0", `frameCount` = "-1", `channel` = "-1" (autocorrelation f0 — the pitch gate) |
| `assert-audio-peak` | SAssertAudioPeakAction | testkit/src/sassertaudiopeakaction.cpp | `filename` = "", `maxPeak` = "0.95", `startFrame` = "0", `frameCount` = "-1", `channel` = "-1" |
| `clear-selection` | SClearSelectionAction | selection/src/sclearselectionaction.cpp | (none) |
| `create-asset` | SCreateAssetAction | objects/mixer/src/screateassetaction.cpp | `container`, `startOffset` = "0", `duration` = "0", `assetName` |
| `cycle-disable` | SCycleAction | actions/src/scycleaction.cpp | (none) |
| `cycle-enable` | SCycleAction | actions/src/scycleaction.cpp | (none) |
| `cycle-toggle` | SCycleAction | actions/src/scycleaction.cpp | (none) |
| `duplicate-clip` | SDuplicateClipAction | objects/cut/src/sduplicateclipaction.cpp | `source`, `destTrack`, `startTime` = "0" |
| `grid-disable` | SGridAction | actions/src/sgridaction.cpp | (none) |
| `grid-enable` | SGridAction | actions/src/sgridaction.cpp | (none) |
| `grid-toggle` | SGridAction | actions/src/sgridaction.cpp | (none) |
| `insert-plugin` | SInsertPluginAction | objects/track/src/sinsertpluginaction.cpp | `trackPath`, `slotIndex` = "0", `format`, `uid`, `name`, `vendor`, `nIn` = "0", `nOut` = "0" |
| `load-project` | SLoadProjectAction | persistence/src/sloadprojectaction.cpp | `path` = "" |
| `metronome-disable` | SMetronomeAction | actions/src/smetronomeaction.cpp | (none) |
| `metronome-enable` | SMetronomeAction | actions/src/smetronomeaction.cpp | (none) |
| `metronome-toggle` | SMetronomeAction | actions/src/smetronomeaction.cpp | (none) |
| `move-clip` | SMoveClipAction | objects/track/src/smoveclipaction.cpp | `clip`, `destTrack`, `startTime` = "0", `broadcast` = "1" (edit groups; same-track moves only) |
| `move-track` | SMoveTrackAction | objects/mixer/src/smovetrackaction.cpp | `source`, `toIndex` = "-1" |
| `place-asset` | SPlaceAssetAction | objects/mixer/src/splaceassetaction.cpp | `assetName` = "", `trackPath` = "", `timePos` = "0" |
| `place-clip` | SPlaceClipAction | objects/cut/src/splaceclipaction.cpp | `trackPath`, `filePath`, `timePos` = "0", `startOffset` = "0", `duration` = "0" (0 = full wave) |
| `place-recording` | SPlaceRecordingAction | objects/cut/src/splacerecordingaction.cpp | `trackPath`, `filePath`, `timePos` = "0" (plans takes for covered columns + plain cuts for gaps; one atomic composite) |
| `remove-asset` | SRemoveAssetAction | objects/mixer/src/sremoveassetaction.cpp | `assetName` |
| `remove-from-selection` | SRemoveFromSelectionAction | selection/src/sremovefromselectionaction.cpp | `paths` = "" |
| `remove-plugin` | SRemovePluginAction | objects/track/src/sremovepluginaction.cpp | `trackPath`, `slotIndex` = "0", `format`, `uid`, `name`, `vendor`, `nIn` = "0", `nOut` = "0" |
| `remove-sample` | SRemoveSampleAction | objects/cut/src/sremovesampleaction.cpp | `trackIndex` = "0", `clipIndex` = "0", `filePath` = "", `timePos` = "0" |
| `remove-take` | SRemoveTakeAction | objects/cut/src/sremovetakeaction.cpp | `clip`, `take` = "0", `thenActivate` = "-2" |
| `remove-track` | SRemoveTrackAction | objects/mixer/src/sremovetrackaction.cpp | `index` = "0" |
| `render` | SRenderAction | actions/src/srenderaction.cpp | `filename` = "", `format` = "wav", `quality` = "10" |
| `reparent-track` | SReparentTrackAction | objects/mixer/src/sreparenttrackaction.cpp | `source`, `destParent`, `destIndex` = "-1" |
| `resize-clip` | SResizeClipAction | objects/cut/src/sresizeclipaction.cpp | `clip`, `startTime` = "0", `startOffset` = "0", `duration` = "0", `loopLength` = "0", `stretch` = "1.0", `take` = "-1" (stacks: which take the slip targets), `broadcast` = "1" (edit groups) |
| `save-project` | SSaveProjectAction | persistence/src/ssaveprojectaction.cpp | `path` = "" |
| `select-take` | SSelectTakeAction | objects/cut/src/sselecttakeaction.cpp | `clip`, `take` = "-1", `broadcast` = "1" (edit groups: same take index on every member) |
| `screenshot` | SScreenshotAction | testkit/src/sscreenshotaction.cpp | `filename` = "", `resolution` = "100%" |
| `set-edit-group` | SSetEditGroupAction | objects/track/src/seteditgroupaction.cpp | `trackPath`, `group` = "0" (0 = ungrouped) |
| `set-pitch` | SSetPitchAction | objects/cut/src/ssetpitchaction.cpp | `clip`, `cents` = "0" (ABSOLUTE, clamped to ±2400), `take` = "-1" (stacks: which take is transposed; pitch is per-take), `broadcast` = "1" (edit groups) |
| `set-property` | SSetPropertyAction | actions/src/ssetpropertyaction.cpp | `key`, `value` |
| `set-selection` | SSetSelectionAction | selection/src/ssetselectionaction.cpp | `paths` = "" |
| `set-track-volume` | SSetTrackVolumeAction | objects/track/src/ssettrackvolumeaction.cpp | `trackIndex` = "0", `volume` = "0" |
| `snap-to-grid-disable` | SSnapToGridAction | actions/src/ssnaptogridaction.cpp | (none) |
| `snap-to-grid-enable` | SSnapToGridAction | actions/src/ssnaptogridaction.cpp | (none) |
| `snap-to-grid-toggle` | SSnapToGridAction | actions/src/ssnaptogridaction.cpp | (none) |
| `split-clip` | SSplitClipAction | objects/cut/src/ssplitclipaction.cpp | `clip`, `splitTime` = "0", `broadcast` = "1" (edit groups) |
| `toggle-playback` | STogglePlaybackAction | actions/src/stoggleplaybackaction.cpp | `play` = "0" |
| `toggle-selection` | SToggleSelectionAction | selection/src/stoggleselectionaction.cpp | `paths` = "" |
