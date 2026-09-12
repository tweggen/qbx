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

`assert-mixer-pane` (the strip list and each strip's `describe()`, plus
`colorMismatch=` / `colors=`), `assert-mixer-layout` (the geometry audit, BOTH
axes, plus `scrolled=` / `masterPinned=`), `mixer-strip-toggle`,
`mixer-strip-set` (the fader, including `gesture="wheel"`),
`mixer-meter-tick` and `mixer-strip-menu`.
Nothing here is gated by a screenshot: `screenshot` grabs the SCREEN's root
window, blank under `QT_QPA_PLATFORM=offscreen`.

**inv. 8 — THE STRIP STAMPS ITS OWN ARRANGEMENT ON EVERY ACTION, never
`stimeline::submitActive`.** That helper asks which editor TAB is active and
stamps that root — right for a widget living inside one arranger, wrong for a
pane that can be showing a different arrangement than the tab in front. The
failure is SILENT: an action stamped with the wrong root resolves an empty
path and does nothing, with no refusal and no log line. `SMixerStrip::submit_`
is the only route out. Gated by `mixer_path_root`, which asserts both halves —
the gesture lands in the right tree AND the identically-positioned track in the
other one is untouched.

**inv. 9 — BOTH MOUNTED WIDGETS ARE ASKED FOR COMPACT MODE, and the Track
Detail dock is not.** `SPluginEffectStrip::setCompact` and
`SSendStrip::setCompact` (proposal 48 D5a). Measured: the FX strip's two Add
buttons alone carry a 113 px layout minimum against a 96 px wide strip.
Nothing becomes unreachable — Edit is already the row's double-click and
Reload / Remove stay on its context menu.

**inv. 10 — THE PER-STRIP NARROW FLAG LIVES ON `STrack`, and the pane owns no
`STrack*`-keyed container at all.** Proposal 48 D9 says it cannot, because
"narrow is a per-USER view preference and `STrack` attributes are serialized
with the arrangement", and prescribes a pane-owned set joined to
`SStdMixerView::pruneUiState`. That walk was RETIRED by proposal 46 M3, and
the three sets D9 names as still being in it all moved onto `STrack` in that
same milestone — every one a per-user view preference serialized with the
arrangement. The flag is a fifth sibling of those four: serialized only when
true, not undoable, not dirtying the project.

**A change that reintroduces a `STrack*`-keyed map here breaks this
invariant.** The hazard it would bring back — a dangling key inherited by a
later track allocated at the same address — is what proposal 46 M3 spent a
milestone removing.

**inv. 11 — THE SECTION MASK IS RE-READ ON EVERY REBUILD, not only at
construction.** `SOpt::MixerSections`, a bitmask (1 inserts, 2 sends, 4 meter,
8 fader). Every test seam builds a fresh pane (shell inv. 62) and so does a
project open, so a pane that honoured only the mask it was born with would
show the defaults forever. Not undoable, and a SOpt key rather than a raw
`SSettings` write because that module owns key NAMES (D4).

**inv. 12 — THE PANE TAKES THE METER BROADCAST AND ANSWERS THE DOCK GATE
BEFORE IT WALKS.** Strips do not connect to `SApplication::meterTick`
themselves. Inv. 5 says a hidden dock does no work "not even the model walk",
and a gate inside each strip has already walked by the time it runs.
`tickWork()` counts the strips that did probe work and is what AC3.5 asserts —
counted, never timed.

**A strip's own visibility is NOT a reason to skip its tick.** A strip scrolled
out of the `QScrollArea` is still `isVisible()` and inv. 5 requires it to keep
ticking; what legitimately stops the work is the meter SECTION being off (a
user's explicit choice) and the dock being hidden (the pane's question).

**inv. 13 — `rebuildStrips()` IS CALLED ONLY WHEN THE LANE LIST DIFFERS.**
`SProject::arrangementChanged` fires after every action and is connected to
`rebuildIfStructureChanged()`, never to the rebuild directly. Connecting it
directly is what M2 did, and it made inv. 7's hazard live in shipped code —
found because a rebuilt strip also throws away its meter's ballistics and its
probe's window (the probe measured peak 0.399994 and the widget reported −60 dB
one line later). The section mask is applied SEPARATELY, because it is a
per-strip visibility change and not a structure change.

**inv. 14 — A STRIP'S COLOUR IS `sclipcolors`, RESOLVED FROM THE PROJECT ROOT**
(proposal 48 AC4.3). `sclipcolors::indexForLane( *project->getRootComponent(),
track )` then `body( idx, false, muted )` — the same two calls
`strackrndrinline.cpp`, the arranger's take lanes and the shell's own
`sClipBodyOf()` (the classifier `assert-take-lane` and `assert-lane-overlay`
use on grabbed PIXELS) all make. Not "the same palette": the same function.
The muted variant follows the flag, so the header is re-resolved when the mute
changes rather than cached at construction.

**THE PADDING IS IN THE ELISION BUDGET, and forgetting it broke three cases at
once.** The tint is a stylesheet, and a `QLabel`'s minimum width is its text
PLUS its padding — so 2 px each side put the NARROW strip's own layout minimum
at 61 against the 60 D9 names (`SMixerStrip(w 60<61)`). `NAME_PAD_PX` is one
constant used by the stylesheet and by the elision, and inv. 6's audit is what
found it.

**inv. 15 — ONE WHEEL NOTCH OVER A FADER IS ONE dB, AND THE STEP IS SHARED**
(AC4.4). `sFaderWheelValue()` in `app/timeline/sfadercurve.h`, filtered by both
the strip and `SSMVMixerControl` rather than left to `QAbstractSlider` — whose
own wheel handling is `wheelScrollLines() * singleStep` in SLIDER units, which
on this curve is ~1.9 dB a notch at unity and ~5 dB at −60. The arranger head
carried a comment claiming 1.0 dB per notch that was wrong from the day it was
written; M4 made it true in both mounts at once rather than in this one.

**inv. 16 — THE STRIP'S CONTEXT MENU IS A SUBSET OF THE ARRANGER'S, SHARED BY
BODY AND NOT BY COPY** (AC4.2). Remove, group and ungroup are
`app/timeline/strackgestures`, which the arranger's own `ct*` slots now call
too; the SUBMIT is injected so each mount stamps its own root (inv. 8 / D12).
Everything else in the head menu is excluded for a reason that is measured
rather than aesthetic — indent and outdent resolve the preceding sibling
through the arranger's ROW list, take lanes / lane height / the automation
picker are row concepts outright, "create asset from range" needs the ruler
RANGE and "insert sample" a click POSITION. A pane has none of those. See
`strackgestures.h`.

**A SYSTEM LANE IS OFFERED NONE OF THEM.** Proposal 45 D6 refuses remove, move
and reparent on the master; refusing to OFFER is that rule read forwards, and
`mixer-strip-menu expect="false"` gates it — an accidental refusal is not
gated by hoping.

## Five layout floors, measured

Every one of these was found by measuring, not by reading, and each would
silently reappear if the next change re-introduced it.

| Floor | The number | What it forces |
|---|---|---|
| A widget the author PINNED with `setFixedSize` is not crushed when it is shorter than its own hint | a 20×20 button whose hint is 28, ×12 | `sHonestMinHeight` honours an explicit fixed size as the author's number. The detail-pane gate is unaffected: its defect was `setMinimumHeight()` alone, where min != max |
| Three 20 px squares plus gaps and margins | 68 px against D9's 60 | narrow uses 16 px squares, drops the dB readout, tightens margins to 1 |
| **A `QScrollArea` carries a large minimum size hint of its own, whatever it holds** | ~113 px — wider than a WIDE strip | the name header lives OUTSIDE the scroll area (a deliberate departure from D5's diagram, and the better shape: a strip's name is its identity, so D5's own "what a user looks at while the transport runs stays put" applies to it), and narrow hides the scroll area outright |
| **A `QLabel`'s minimum width is its FULL TEXT** — the one that actually held the wide strip hostage | 91 px for an ordinary generated track name; with the toggle button and margins, exactly the 113 the strip was reported as owing | the name ELIDES at both widths, as a fixed-width column must. The full name stays in the tooltip and in `describe()` |
| **A tinted `QLabel`'s minimum width is its text PLUS its stylesheet padding** (proposal 48 M4 / AC4.3) | 2 px each side put the NARROW strip at 61 against D9's 60 — `SMixerStrip(w 60<61)`, and THREE cases failed at once | `NAME_PAD_PX` is one constant, used by the stylesheet and by the elision budget. Found by inv. 6's own audit within minutes of the tint landing |

## Known debt

- **The four section BUTTONS and the strip's narrow button are not gated.**
  M2 wires the mask and the flag they write; there is no testkit verb for a
  toolbar any more than for a context menu, so the cases drive `set-option`
  and `mixer-strip-toggle` and the buttons themselves are hand-verified.
- **Each strip scrolls its own FX/sends section independently.** D5 puts the
  fader block outside the scroll area, which means one scroll area per strip;
  scrolling one strip's inserts does not scroll its neighbour's. Acceptable
  while the sections are short and worth revisiting if they grow.
- **`SystemLanes::All` has no caller**, so a SEND lane gets no strip: send
  lanes have no arranger row either (a proposal 45 M7 gap `slaneorder.h`
  records). Closing it changes the strip list and belongs with whatever closes
  the arranger half.
- **The LIVE dock is not what any gate measures.** A `--test-case` run never
  binds its project into the window, so every seam builds a pane on demand and
  what is gated is the VERB PATH rather than widget-state persistence. The
  dock, its View-menu item, its shortcut and its detach are hand-verified.
