# ARCHITECTURE — module map and how to work here

Smaragd is split into modules (proposal 14, executed 2026-07-12). Every
module has a `CONTRACT.md` next to its sources; cross-module protocols live
in `docs/contracts/`. This page is the map.

## Engine (`smaragd/tw303a/`) — build-enforced DAG

One `tw_<module>` static library each; a module cannot include a header of
a module it does not link (declared in `tw303a/CMakeLists.txt`). Includes
are path-qualified: `#include "tw/graph/twcomponent.h"`.

```
core ── pages ── graph ─┬─ sources ─┐
  │        │            ├─ dsp      │
  │        │            ├─ mix      │
  │        │            ├─ plugins  │
  │        │            └─ schedule │
  ├─ devices ────────────────┐      │
  ├─ sinks ──────────────┐   │      │
  ├─ analysis            │   │      │
  └──────────── playback ┴───┴──────┘── render
                    └─ record (devices+sinks+sources)
```

Not part of the dataflow above: `tw/analysis` and `tw/sidecar` (→ core) and
`tw/metering` (→ core+pages+graph) are leaves consumed by the app and the tests.
`tw/metering` READS frozen pages by position and never freezes or demands, so it
hangs off the graph without joining the audio path. `tw/events` (→ core ONLY,
proposal 37) is the MIDI/event model leaf: events are model data, not pages, so
it has no place in the dataflow DAG at all — and it must stay core-only because
`tw/plugins` (which will consume its event clip set) may not include `tw/mix`.

| Module | One-liner | Contract |
|---|---|---|
| tw/core | value types, format, fraction, exact rationals, TwLog | tw303a/core/CONTRACT.md |
| tw/pages | frozen pages, IOVector, page pool | tw303a/pages/CONTRACT.md |
| tw/graph | **the component contract**, latches, twView, env | tw303a/graph/CONTRACT.md |
| tw/sources | sample data, readers, grain, resampling | tw303a/sources/CONTRACT.md |
| tw/dsp | oscillators, filters, noise | tw303a/dsp/CONTRACT.md |
| tw/mix | track mix (clip model), mixer, rewire | tw303a/mix/CONTRACT.md |
| tw/plugins | plugin ABI, registry, hosting chain | tw303a/plugins/CONTRACT.md |
| tw/devices | WASAPI/ALSA/CoreAudio backends + inputs; MIDI in/out (WinMM/CoreMIDI/ALSA-seq/capture/null) + `MidiOutScheduler` | tw303a/devices/CONTRACT.md |
| tw/sinks | file writers (WAV/OGG/MP3), frame sinks | tw303a/sinks/CONTRACT.md |
| tw/playback | speaker, audio engine, readahead | tw303a/playback/CONTRACT.md |
| tw/render | **the rendering engine** (offline) | tw303a/render/CONTRACT.md |
| tw/record | capture bridge (one input pump, three sinks) + recording session | tw303a/record/CONTRACT.md |
| tw/schedule | async revalidation (IRevalidatable) | tw303a/schedule/CONTRACT.md |
| tw/analysis | WAV metrics for tests | tw303a/analysis/CONTRACT.md |
| tw/sidecar | derived-data QAF container + LRU store | tw303a/sidecar/CONTRACT.md |
| tw/metering | level meters: page probe + ballistics | tw303a/metering/CONTRACT.md |
| tw/events | events, tempo map, SMF, curves, clip set | tw303a/events/CONTRACT.md |

## App (`smaragd/main/`) — one SCC, checker-enforced boundaries

**17 modules** (the table below; `objects/` holds five of them) with
`app/<module>/…` includes, built as FOUR
layered OBJECT libraries (OBJECT is load-bearing: actions and the loader/
editor/extern-file registries self-register via static initializers, which
a STATIC lib would drop):

    app_model < app_core < app_objects < app_ui
    (model)     (actions,     (objects/cut,   (timeline, pluginui,
                persistence,   wave, midi,     eventui, servicesui,
                selection,     track, mixer)   shell, testkit,
                media)                         mediabrowser)

The layer boundaries are COMPILE-TIME ENFORCED: each layer target publishes
only its own include dirs and links only the lower layers plus its declared
engine modules — a cross-layer include (model→actions, core→objects,
anything below the UI→shell) fails to compile. The core modules reach the
application only through `app/model/sappcontext.h`.

**What the layer boundary does NOT enforce, verified rather than assumed:** it
does not stop a WIDGET include. `app_model` links `Qt::Widgets` **PUBLIC** and
`app_core` links `app_model` PUBLIC, so every Qt widget header is available at
every layer — indeed `SExternFileList` is a `QTreeWidget` in the LOWEST one.
What is enforced is the `app/<module>/…` include GRAPH. So `app/media`'s "no
widget in a provider" rule is a contract plus a grep (proposal 38 §B.1, gate 1
AC 11), not a compiler error, and a subagent told otherwise gets no error at
all.

`Qt6::Network` and `Qt6::Concurrent` are linked on **app_core** (for
`app/media`) and therefore propagate upward. That is the one impurity in the
module story and it is accepted: layers, not modules, are the CMake targets.

Diagnostics: every module logs through `TW_LOG*` (`tw/core/twlog.h`) or the
`syslog()` shim, both of which land in the one `TwLog` ring that feeds the
console tee, the rotating file, and the in-app log dock. Nothing writes to
stderr/stdout directly — `python tools/check_logging.py` enforces that
(proposal 24).

`python tools/check_layering.py` guards the finer grain the build cannot:
per-MODULE engine deps and the declared intra-layer edge set. Since the
placement service (`app/model/splacements.h`) the object slices form a DAG
— wave < cut < track < mixer, with midi at the RANK of cut (a second
window/content pair, not a layer above one) — leaving UI+shell as the only
cyclic group. Do not
add SApplication::app() call sites below the UI layer, and keep SAppContext
minimal.

| Module | One-liner | Contract |
|---|---|---|
| app/model | SObject/SLink/SProject document tree | main/model/CONTRACT.md |
| app/media | media SOURCES (local FS, Nextcloud/WebDAV), the fetch cache, the `media:` drop helper. **No widget, ever** | main/media/CONTRACT.md |
| app/objects/cut | clip window (SCut) + renderer + window actions | main/objects/cut/CONTRACT.md |
| app/objects/midi | event clip (SMidiSequence/SMidiCut) + renderer + event verbs | main/objects/midi/CONTRACT.md |
| app/objects/wave | sample object + renderer + sample actions; the GROWING recording content (proposal 21 L3b) | main/objects/wave/CONTRACT.md |
| app/objects/track | track + clip sync to engine + placement actions | main/objects/track/CONTRACT.md |
| app/objects/mixer | root mixer, plugin chain model, asset actions | main/objects/mixer/CONTRACT.md |
| app/actions | command framework + generic verbs | main/actions/CONTRACT.md |
| app/persistence | project load/save | main/persistence/CONTRACT.md |
| app/selection | selection state + actions | main/selection/CONTRACT.md |
| app/timeline | the arrangement canvas + chrome | main/timeline/CONTRACT.md |
| app/mediabrowser | the Media Browser DOCK and nothing else — source picker, tree, filter, search, drag out | main/mediabrowser/CONTRACT.md |
| app/pluginui | plugin browser/editor widgets | main/pluginui/CONTRACT.md |
| app/eventui | event editor (piano roll) + virtual keyboard | main/eventui/CONTRACT.md |
| app/servicesui | render/record/options dialogs | main/servicesui/CONTRACT.md |
| app/shell | SApplication, SMainWindow, main() — composition root; the live monitor, the MIDI-out pump, the AUDIO RECORDER, `SSecretStore` and the Nextcloud accounts model | main/shell/CONTRACT.md |
| app/testkit | qxa runner, audio assertions | main/testkit/CONTRACT.md |

## Cross-module protocols (read these before touching audio paths)

- `docs/contracts/POSITION_DOMAINS.md` — who speaks which time domain.
- `docs/contracts/FREEZE_PROTOCOL.md` — random-access page rendering.
- `docs/contracts/THREADING.md` — thread inventory; the no-Qt-off-main rule.
- `docs/contracts/CLIP_MODEL.md` — SLink/SCut/ClipEntry and their sync.
- `docs/ACTIONS.md` — every action verb with its attributes (the scripting
  API). It is a hand-maintained mirror of the registry, so treat a verb that
  is missing from it as an omission in the DOC, not as proof the verb is gone.
- `docs/MEDIA_BROWSER_MANUAL_GATE.md` — the manual runbook for the parts of
  the media browser no headless gate can reach (a real Nextcloud server, TLS,
  the credential store on real hardware).

## Working agreement (humans and AIs)

1. A task names ONE module it may modify (plus its CONTRACT.md). Touching
   another module's sources escalates the task.
2. Recipe: read the module's CONTRACT.md → read the PUBLIC headers of its
   dependencies (not their src/) → run the tests it names → implement →
   green: `ctest` from smaragd/build/ (runs the module unit tests AND every
   qxa case) plus `python tools/check_layering.py` and
   `python tools/check_logging.py`.
3. Changing a public header or an invariant is its own, human-reviewed
   change — land it before dependent work.
4. Update the module's CONTRACT.md "Known debt" when you add or retire debt;
   update `tools/check_layering.py` when a declared edge genuinely changes.
