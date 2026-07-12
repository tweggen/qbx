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

| Module | One-liner | Contract |
|---|---|---|
| tw/core | value types, format, fraction, AudioFrame | tw303a/core/CONTRACT.md |
| tw/pages | frozen pages, IOVector, page pool | tw303a/pages/CONTRACT.md |
| tw/graph | **the component contract**, latches, twView, env | tw303a/graph/CONTRACT.md |
| tw/sources | sample data, readers, grain, resampling | tw303a/sources/CONTRACT.md |
| tw/dsp | oscillators, filters, noise | tw303a/dsp/CONTRACT.md |
| tw/mix | track mix (clip model), mixer, rewire | tw303a/mix/CONTRACT.md |
| tw/plugins | plugin ABI, registry, hosting chain | tw303a/plugins/CONTRACT.md |
| tw/devices | WASAPI/ALSA/CoreAudio backends + inputs | tw303a/devices/CONTRACT.md |
| tw/sinks | file writers (WAV/OGG/MP3), frame sinks | tw303a/sinks/CONTRACT.md |
| tw/playback | speaker, audio engine, readahead | tw303a/playback/CONTRACT.md |
| tw/render | **the rendering engine** (offline) | tw303a/render/CONTRACT.md |
| tw/record | recording session | tw303a/record/CONTRACT.md |
| tw/schedule | async revalidation (IRevalidatable) | tw303a/schedule/CONTRACT.md |
| tw/analysis | WAV metrics for tests | tw303a/analysis/CONTRACT.md |

## App (`smaragd/main/`) — one SCC, checker-enforced boundaries

13 module directories with `app/<module>/…` includes, built as FOUR
layered OBJECT libraries (OBJECT is load-bearing: actions and the loader/
editor/extern-file registries self-register via static initializers, which
a STATIC lib would drop):

    app_model < app_core < app_objects < app_ui
    (model)     (actions,     (objects/cut,   (timeline, pluginui,
                persistence,   wave, track,    servicesui, shell,
                selection)     mixer)          testkit)

The layer boundaries are COMPILE-TIME ENFORCED: each layer target publishes
only its own include dirs and links only the lower layers plus its declared
engine modules — a cross-layer include (model→actions, core→objects,
anything below the UI→shell) fails to compile. The core modules reach the
application only through `app/model/sappcontext.h`.
`python tools/check_layering.py` guards the finer grain the build cannot:
per-MODULE engine deps and the declared intra-layer edge set (the remaining
cyclic groups: the four object slices among themselves; UI+shell). Do not
add SApplication::app() call sites below the UI layer, and keep SAppContext
minimal.

| Module | One-liner | Contract |
|---|---|---|
| app/model | SObject/SLink/SProject document tree | main/model/CONTRACT.md |
| app/objects/cut | clip window (SCut) + renderer + window actions | main/objects/cut/CONTRACT.md |
| app/objects/wave | sample object + renderer + sample actions | main/objects/wave/CONTRACT.md |
| app/objects/track | track + clip sync to engine + placement actions | main/objects/track/CONTRACT.md |
| app/objects/mixer | root mixer, plugin chain model, asset actions | main/objects/mixer/CONTRACT.md |
| app/actions | command framework + generic verbs | main/actions/CONTRACT.md |
| app/persistence | project load/save | main/persistence/CONTRACT.md |
| app/selection | selection state + actions | main/selection/CONTRACT.md |
| app/timeline | the arrangement canvas + chrome | main/timeline/CONTRACT.md |
| app/pluginui | plugin browser/editor widgets | main/pluginui/CONTRACT.md |
| app/servicesui | render/record/options dialogs | main/servicesui/CONTRACT.md |
| app/shell | SApplication, SMainWindow, main() — composition root | main/shell/CONTRACT.md |
| app/testkit | qxa runner, audio assertions | main/testkit/CONTRACT.md |

## Cross-module protocols (read these before touching audio paths)

- `docs/contracts/POSITION_DOMAINS.md` — who speaks which time domain.
- `docs/contracts/FREEZE_PROTOCOL.md` — random-access page rendering.
- `docs/contracts/THREADING.md` — thread inventory; the no-Qt-off-main rule.
- `docs/contracts/CLIP_MODEL.md` — SLink/SCut/ClipEntry and their sync.
- `docs/ACTIONS.md` — all 41 action verbs with attributes (the scripting API).

## Working agreement (humans and AIs)

1. A task names ONE module it may modify (plus its CONTRACT.md). Touching
   another module's sources escalates the task.
2. Recipe: read the module's CONTRACT.md → read the PUBLIC headers of its
   dependencies (not their src/) → run the tests it names → implement →
   green: `ctest` from smaragd/build/ (runs the module unit tests AND every
   qxa case) plus `python tools/check_layering.py`.
3. Changing a public header or an invariant is its own, human-reviewed
   change — land it before dependent work.
4. Update the module's CONTRACT.md "Known debt" when you add or retire debt;
   update `tools/check_layering.py` when a declared edge genuinely changes.
