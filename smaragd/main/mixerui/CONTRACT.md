# app/mixerui — the mixer pane

**Layer:** `app_ui`. **Proposal:** `plan/proposed/48_MIXER_PANE.md` (M1).

The mixer PANE and nothing else — a horizontal dock holding one channel strip
per track lane, on the media browser's precedent one rank over. It owns no
model, defines no verb and stores no audio state.

## The governing rule

> **The mixer is a second MOUNT of rules the arranger already owns — never a
> second COPY of them.**

Lane order, the selection broadcast, the solo/mute audibility rule, the fader
curve, the meter's lane cap and the clip palette each have exactly one spelling
in this tree. This module adds a mount, not a spelling. The repository has paid
for the alternative four times — the meter and the ear disagreeing about a
nested lane (`main/timeline/CONTRACT.md` inv. 10), paint and hit-test
disagreeing about z-order (inv. 32, green for two milestones), the loop-marker
geometry, and the take-lane body fill (inv. 35).

Concretely, and each of these is an invariant rather than a preference:

| The pane asks | It must NEVER |
|---|---|
| `slaneorder::flattenTrackLanes()` (proposal 48 M0) | walk `childLinks()` itself |
| `strackbroadcast::targetsFor()` | decide its own multi-selection rule |
| `ssolo::isLaneAudible()` | re-derive a local mute/solo chain |
| `sfadercurve.h` | invent a second dB↔tick mapping |
| `sclipcolors.h` | hardcode a colour |
| `SLevelMeter`, `SPluginEffectStrip`, `SSendStrip` | re-implement a meter, an insert list or a sends list |

## Invariants

**inv. 1 — THE PANE HONOURS `laneHidden()` FOR USER LANES AND SHOWS THE MASTER
REGARDLESS.** This is the one place the two mounts deliberately disagree about
a model flag (proposal 48 D6a). `SObject::laneHiddenByDefault()` is TRUE for
every system role, so on a project nobody has touched the master lane has no
ARRANGER row — correct there, since a master row costs vertical space in a list
of lanes. A mixer without its summing point is not a mixer, so the pane passes
`slaneorder::Options::alwaysShowMaster`. The exemption covers the master lane
ITSELF and never its children: a hidden conductor lane stays hidden in both
mounts. Stated here, in the strip's tooltip, and gated by `laneorder_test`.

**inv. 2 — THE PANE IGNORES THE ARRANGER'S FOLD STATE** (D2). A folder
collapsed in the arranger keeps its children's STRIPS. Fold answers "show this
lane's children as ROWS", and a mixer has no rows. **Consequence: the strip
count does not match the arranger's visible row count, and no gate may assert
that it does.**

**inv. 3 — EVERY MUTATION IS AN ACTION, AND THE PATH ROOT IS THE ACTIVE TAB'S**
(D11/D12). Selection is per-`SStdMixer` and every action carries a `pathRoot`.
A pane that submitted blind through `submitActive` while showing a different
arrangement would broadcast over the wrong selection and resolve its paths
against the wrong tree — the shape `fix/editor-ui-and-shortcuts` already shipped
once, where `SClearSelectionAction` honours `pathRoot_` and the convenience
helper did not set one.

**inv. 4 — THE PANE JOINS THE PROJECT-CLOSE DETACH SEAM** (D13). `closeProject()`
clears the undo stack, calls `destroyDocksToolbars()` — which explicitly detaches
the three track-holding panels — and then deletes the project. A pane holding one
`STrack *` and one `twLevelProbe` per strip that is NOT on that list dereferences
freed tracks on the next meter tick. The strips hold their tracks as `QPointer`
and `detachProject()` drops every strip and every probe. Judged by EXIT CODE, the
`plugin_native_editor_teardown_safe` precedent.

**inv. 5 — A HIDDEN DOCK DOES NO WORK, NOT EVEN THE MODEL WALK.** The Feel Flow
puppet's rule, and it matters more here because the walk is per strip. A strip
scrolled out of the viewport still ticks (its meter is a few px of paint and
stopping it would freeze a bar the user scrolls back to), but a page MISS idles
it — never holds. Proposal 34: "a page miss must DECAY the meter."

**inv. 6 — NO WIDGET IN THE PANE THAT CARRIES A LAYOUT SETS AN EXPLICIT MINIMUM
HEIGHT OR WIDTH.** `main/timeline/CONTRACT.md` inv. 45: `qSmartMinSize()`
REPLACES a layout-derived minimum with an explicit one rather than taking the
larger, and a `QBoxLayout` handed less than its minimum distributes the
shortfall and lets its children overlap. The pane is that situation once per
strip plus once for the pane.

**inv. 7 — REBUILD ON STRUCTURE, UPDATE IN PLACE OTHERWISE.** A pane that
rebuilt every strip on every model signal deletes the fader mid-drag. Structure
= tracks added / removed / reordered / hidden.

## How to test

`assert-mixer-pane` (the strip list and each strip's `describe()`),
`assert-mixer-layout` (the geometry audit, BOTH axes) and `mixer-strip-toggle`.
Nothing here is gated by a screenshot: `screenshot` grabs the SCREEN's root
window, blank under `QT_QPA_PLATFORM=offscreen`.

## Known debt

Filled in by M1 as executed.
