#!/usr/bin/env python3
"""Layering checker (proposal 14 §6.2).

Engine side (build-enforced too; this is the greppable review view):
  1. No engine file includes an app (main/) header.
  2. Every cross-module include inside an engine module respects the declared
     module DAG (transitively).

App side: the LAYER boundaries (app_model < app_core < app_objects <
app_ui) are compile-time enforced by the four OBJECT-library targets in
main/CMakeLists.txt. This checker guards the FINER grain the build cannot:
  3. Each app module's engine includes stay within its declared tw/ modules
     (per MODULE — CMake only scopes per LAYER union).
  4. Intra-layer cross-module includes stay within the DECLARED edge set.
     New edges must be added here consciously. Since the placement service
     the object slices are a DAG (wave < cut < track < mixer); the only
     remaining cyclic group is UI+shell.

Run from the repo root:  python tools/check_layering.py
Exit code 0 = clean, 1 = violations (printed one per line).
"""
import os, re, sys

TW = os.path.join('smaragd', 'tw303a')

# The engine module DAG — keep in sync with smaragd/tw303a/CMakeLists.txt.
DEPS = {
    'core':     [],
    'pages':    ['core'],
    'graph':    ['core', 'pages'],
    # sources → sidecar since proposal 27 M2: twGrainSource caches its
    # finished warp (warp.pcm) in the derived-data store.
    'sources':  ['core', 'pages', 'graph', 'sidecar'],
    'dsp':      ['core', 'graph'],
    # mix -> events since proposal 37 P5: twGainStage consumes a
    # twAutomationCurve (the Volume/Mute lanes) and twTrackMix a per-clip gain
    # envelope. tw/events is core-only and outside the dataflow DAG, so this
    # adds no page dependency (design D5 / F15).
    'mix':      ['core', 'pages', 'graph', 'events'],
    # plugins -> events since proposal 37 P2: the plugin ABI's event list quotes
    # tw/events/twevent.h. tw/events is core-only and NOT in the dataflow DAG,
    # so this adds no page dependency (design F15: plugins may not reach mix).
    'plugins':  ['core', 'graph', 'events'],
    # devices -> events since proposal 21 L2: twLiveEventSource IS a
    # twEventSource - it turns the bytes a MIDI input delivers into the ONE
    # twEvent this codebase has. tw/events is core-only and outside the
    # dataflow DAG, so this adds no page dependency; the alternative
    # (events -> devices) would break the leaf status events is kept at.
    'devices':  ['core', 'events'],
    'sinks':    ['core'],
    # playback → schedule since proposal 19 stage 5: the readahead is a
    # demand consumer of the page scheduler instead of pulling freezes.
    # playback → plugins, mix since proposal 21 L1a: the LiveGraphPump renders
    # live-owned tracks BLOCK-WISE, outside the frozen-page machinery, and the
    # three pieces of the graph that survive block-wise are exactly
    # twPluginSlotProcessor::render, twGainStage::applyGain and twRewire's
    # channel map. Neither plugins nor mix depends on playback, so the DAG stays
    # acyclic; events arrives transitively through both.
    'playback': ['core', 'pages', 'graph', 'devices', 'sources', 'schedule',
                 'plugins', 'mix'],
    # render → schedule since proposal 19 stage 4: the offline render is a
    # watermark CONSUMER of the page scheduler instead of pulling freezes.
    'render':   ['core', 'pages', 'graph', 'sinks', 'playback', 'schedule'],
    'record':   ['core', 'devices', 'sinks', 'sources'],
    'schedule': ['core', 'pages', 'graph'],
    'analysis': ['core'],
    'sidecar':  ['core'],
    # events (proposal 37 P0b): the MIDI/event model leaf - one twEvent, the
    # event sequence, the tempo map (the only tick<->frame converter), SMF I/O,
    # the automation curve, the event clip set and the feed merge. CORE ONLY and
    # deliberately outside the dataflow DAG: events are model data, not pages
    # (D1), and the clip set has to be includable from tw/plugins, which may not
    # include tw/mix (F15) - so it may never grow an edge to pages/graph/mix.
    'events':   ['core'],
    # metering (proposal 34): reads levels out of frozen pages by position.
    # graph for twComponent::getPageIfExists, pages for twOutputPage. It must
    # NOT reach playback or mix — which component is the right tap is the app's
    # decision, not the module's.
    'metering': ['core', 'pages', 'graph'],
}

APP_HEADERS = re.compile(
    r'#\s*include\s*["<](sapplication|sproject|sobject|scut|slink|strack|'
    r'sstdmixer|smainwindow|ssettings|saction)[^">]*[">]')
TW_INCLUDE = re.compile(r'#\s*include\s*["<]tw/([a-z]+)/[^">]+[">]')

def closure(mod):
    seen = set()
    stack = [mod]
    while stack:
        m = stack.pop()
        if m in seen:
            continue
        seen.add(m)
        stack.extend(DEPS.get(m, []))
    return seen

MAIN = os.path.join('smaragd', 'main')

APP_INCLUDE = re.compile(r'#\s*include\s*["<]app/([a-z/]+)/[^">]+[">]')

# Declared app-internal coupling (measured 2026-07-12). The app is one SCC;
# every edge here is real today. Phase 6 shrinks this list — do not grow it
# without a conscious decision.
APP_DEPS = {
    # Phase 5/6: model+persistence name no concrete types; the core modules
    # (model, actions, persistence, selection, objects/*) are SHELL-FREE —
    # they reach the app only through app/model/sappcontext.h. The remaining
    # objects/* cross-edges are semantic (placement actions know the track
    # tree and the types they create).
    'model':          set(),
    # The object slices form a DAG since the placement service:
    #   wave < cut < track < mixer (only downward edges below).
    'objects/wave':   {'model', 'persistence'},
    'objects/cut':    {'actions', 'model', 'objects/wave', 'persistence'},
    # objects/midi sits at the RANK of objects/cut (proposal 37 3.5): it is a
    # second window/content pair, not a layer above one, and it must stay
    # independent of the audio window. Deliberately absent from every other
    # slice's deps except the ones that genuinely need a concrete MIDI type:
    # objects/track must NOT depend on it - the track consults MIDI-ness only
    # through SObject::contentKind() and SObject::resolveEventClip().
    'objects/midi':   {'actions', 'model', 'persistence'},
    'objects/track':  {'actions', 'model', 'persistence'},
    # objects/fragment (proposal 41 M1) sits at the RANK of objects/track, not
    # inside it: a fragment is content an SCut windows (D1), not a track, and
    # must not inherit the track's dependency on plugins/instruments. It must
    # also stay OUT of objects/cut's deps — the dependency runs cut ->
    # fragment (an SCut may window a fragment), never the reverse — so it is
    # declared as its own module rather than folded into either neighbour.
    # {'actions', 'model', 'persistence'} matches objects/track's shape; M1
    # itself uses only 'model' (SLaneFragment names no SAction and does its
    # own loader registration through app/persistence), 'actions' is here
    # ahead of M2's pack-clips/unpack-clips verbs, which will live in this
    # module.
    'objects/fragment': {'actions', 'model', 'persistence'},
    'objects/mixer':  {'actions', 'model', 'objects/cut', 'objects/track',
                       'persistence'},
    'actions':        {'model'},
    # media (proposal 38 gate 1) is the media-browser's SOURCE layer: the
    # provider ABI, the local walk, and later the cache and the WebDAV client.
    # `model` only, and it must never grow an edge to objects/* -- a provider
    # that knows what an SCut is has started doing the dock's job. The DOCK is
    # a separate module (main/mediabrowser, app_ui) that gate 2 adds.
    # `media` gained the cache and the drop helper in gate 3. It still sees
    # `model` ONLY, and that is not a taste -- app/media is app_core and
    # SAddSampleAction is app_objects, one LAYER up, so the placement cannot be
    # constructed here at all. It is a HOOK the shell installs
    # (smediadrop::setPlacementHook), exactly as the shell injects the plugin
    # scan cache's path. A provider that knew what an SCut is would have started
    # doing the dock's job.
    'media':          {'model'},
    # mediabrowser (proposal 38 gate 2) is the DOCK and nothing else. It sees
    # `media` (the provider ABI it drives), `shell` (SSettings, for the four
    # media/* keys it persists) and `servicesui` (SOpt, which is where those key
    # NAMES live -- the same reason the servicesui -> actions edge exists: that
    # module OWNS the per-user option table, and a second spelling of a key is
    # how a setting silently stops round-tripping). `actions` is declared for
    # gate 3, where a deferred placement submits `add-sample`. It must never
    # reach app/timeline: a browser that knows what the arranger looks like has
    # started doing the arranger's job, and the drag payload is the whole
    # interface between them.
    'mediabrowser':   {'actions', 'media', 'model', 'servicesui', 'shell'},
    'persistence':    {'actions', 'model'},
    'selection':      {'actions', 'model'},
    # timeline + objects/midi since proposal 37 P1: the Clip Properties dock
    # grows an SMidiCut page, and the ruler's Set BPM commits through set-tempo.
    # timeline + media since proposal 38 gate 3: dropEvent's ONE new branch
    # (five lines) hands a `media:` payload to smediadrop::placeWhenLocal. The
    # arranger never learns what a source or a cache is -- the payload is the
    # whole interface, the same way it already is for `file:` and `asset:`.
    # timeline + selection (AC-a1): the Ctrl-drag-duplicate finalize submits
    # ONE SSetSelectionAction naming the copies, folded into the SAME undo
    # macro as the SDuplicateClipAction(s) — see sstdmixerview.cpp's
    # mouseReleaseEvent.
    'timeline':       {'actions', 'media', 'model', 'objects/cut',
                       'objects/midi', 'objects/mixer', 'objects/track',
                       'objects/wave', 'pluginui', 'selection', 'servicesui',
                       'shell'},
    'pluginui':       {'model', 'objects/mixer', 'objects/track', 'shell'},
    # eventui (proposal 37 P4) sits at the RANK of pluginui: a UI slice that
    # edits ONE object kind through the action system and is hosted by the
    # shell. It deliberately does NOT reach `timeline`: the arranger's zoom and
    # scroll arrive through the SHELL, which already depends on both, so the
    # editor stays independent of the 4000-line arranger. objects/mixer +
    # objects/track are the virtual keyboard's "first event clip on the
    # SELECTED track" fallback.
    # eventui + servicesui (mouse-wheel pan/zoom follow-up): the piano roll's
    # own wheel-navigation config reads SOpt (its key NAMES and defaults) and
    # writes through SSettings, exactly the reasoning the mediabrowser ->
    # servicesui edge above already gives -- servicesui OWNS the per-user
    # option table, and a second spelling of a key is how a setting silently
    # stops round-tripping.
    'eventui':        {'actions', 'model', 'objects/midi', 'objects/mixer',
                       'objects/track', 'servicesui', 'shell'},
    # servicesui + actions since proposal 21 L5: `set-count-in` / `set-pre-roll`
    # are registered here because this is the module that OWNS the per-user
    # option table (SOpt) and the only one that may include both it and
    # SSettings. Every other option verb in the tree lives beside its own
    # setting for the same reason.
    'servicesui':     {'actions', 'model', 'shell'},
    # theme (the widget style, 2026-08-21) is a LEAF and must stay one. It
    # names no other app module -- not even servicesui for its own option key
    # and not SSettings for the value: the shell reads the preference and
    # passes the resolved name to STheme::apply(). That is what keeps a style
    # out of the app's one big dependency cycle, and what lets the resolution
    # be reasoned about without an INI or a window.
    'theme':          set(),
    # shell + objects/midi since proposal 37 P1: the transport tempo box
    # commits through the set-tempo verb instead of writing the project.
    # shell + eventui since proposal 37 P4: the event editor and the virtual
    # keyboard are docks, created in SMainWindow's ctor like every other one.
    # shell + mediabrowser since proposal 38 gate 2: the Media Browser is the
    # SEVENTH dock, created in SMainWindow's ctor like every other one (shell
    # CONTRACT inv. 4 -- restoreWindowLayout() runs later and can only restore
    # docks that already exist).
    # shell + media since proposal 38 gates 3 and 5b. The shell is the
    # composition root for the whole media layer: it installs smediadrop's
    # placement and status hooks and pushes the cache its root + cap (gate 3 --
    # it is the only place that sees BOTH app/media and SAddSampleAction, which
    # live one layer apart), and SMediaAccountManager implements
    # smedia::CredentialProvider and owns the SWebDavMediaSource instances it
    # registers with SMediaRegistry (gate 5b -- the composition root
    # implementing what `media` only declares the seam for). `APP_DEPS` is NOT
    # transitive, so `shell` needs its own edge here even though `mediabrowser`
    # already has one.
    # shell + pluginui since proposal 33 D2: a project saved with plugin
    # editors open must open with them open, and that restore is ONCE PER LOAD
    # -- so its only honest home is the end of SMainWindow::openProject(),
    # after every dock and the central widget exist. It is a direct call and
    # not a hook, unlike smediadrop's, because the direction is the other way
    # round: the shell is CALLING into the UI slice, not injecting into it, and
    # a std::function installed at static-init time to express one call would
    # be indirection with nothing behind it. This does close a two-module cycle
    # with the `pluginui -> shell` edge below, and that is a shortening of the
    # existing `shell -> timeline -> pluginui -> shell` cycle rather than a new
    # class of problem -- see the UI+shell note at the top of this file.
    'shell':          {'actions', 'eventui', 'media', 'mediabrowser', 'model',
                       'objects/cut', 'objects/midi', 'objects/mixer',
                       'objects/track', 'objects/wave', 'persistence',
                       'pluginui', 'selection', 'servicesui', 'testkit', 'theme',
                       'timeline'},
    # testkit + objects/cut + objects/wave since proposal 27 M1 test verbs:
    # set-render-gate addresses an SCut, wait-analysis reads SPlainWave.
    # testkit + pluginui since proposal 08 M5: assert-plugin-strip and
    # plugin-editor-set-param build the REAL SPluginEffectStrip /
    # SPluginParamEditor off screen, which is the only automated coverage a
    # milestone made almost entirely of widgets can have. Same shape as the
    # existing testkit -> shell edge (drag-clip-edge, assert-lane-alignment):
    # a test verb reaching the widget it is testing.
    # testkit + objects/midi since proposal 37 P1: assert-midi-events reads a
    # cut's frame-domain snapshot and a track's event feed, which is the only
    # way to see mute / solo / midiRouting from a script.
    # testkit + servicesui since proposal 37 P7b: assert-midi-options builds the
    # REAL SOptionsDialog off screen and asserts on describeMidiPage(), the same
    # shape as assert-plugin-strip reaching into pluginui.
    # testkit + media + mediabrowser since proposal 38 gate 2: the six
    # media-browser verbs drive the REAL panel, and gate 5c's WebDAV stub lands
    # here too. Today they reach the panel THROUGH the shell (the drag has to,
    # because testkit may not include app/timeline), so the two edges are
    # declared ahead of the code that needs them rather than discovered later.
    # testkit + objects/fragment since proposal 41 M1: fragment_test builds a
    # real SLaneFragment (and an SCut windowing one) off screen, the same
    # shape as project_channels_test reaching objects/track. testkit +
    # persistence, same milestone: fragment_test's AC1.3 round-trips a
    # hand-authored document through the REAL SProjectLoader, exactly the
    # save/load machinery a project file goes through.
    'testkit':        {'actions', 'media', 'mediabrowser', 'model',
                       'objects/cut', 'objects/fragment', 'objects/midi',
                       'objects/mixer', 'objects/track', 'objects/wave',
                       'persistence', 'pluginui', 'servicesui', 'shell'},
}

# Which engine modules each app module may include (tw/<mod>/... paths).
# core and graph are foundational and allowed everywhere.
_ENG_BASE = {'core', 'graph'}
APP_ENG = {
    # model + events since proposal 37 P1: SProject owns the twTempoMap (the
    # single tempo authority, D2) and SLink's beats timebase converts through
    # it. No other app module may convert ticks to frames.
    # model + events also covers proposal 37 P5's SAutomationLane, whose
    # snapshot IS a twAutomationCurve.
    'model':          _ENG_BASE | {'events', 'pages', 'schedule', 'sources'},
    # objects/cut + events since proposal 37 P5: an SCut owns its `cut:Gain`
    # envelope and hands the twAutomationCurve snapshot to the track's mix.
    'objects/cut':    _ENG_BASE | {'events', 'pages', 'schedule', 'sources'},
    # objects/wave + sidecar since proposal 27 M0: SPlainWave persists its
    # straight preview through the derived-data sidecar store.
    'objects/wave':   _ENG_BASE | {'pages', 'schedule', 'sources', 'sidecar'},
    # objects/midi: the tick-native window and the event content. `events`
    # gives it twEventSeq / twEventClipResolved / twTempoMap / twSmf and
    # nothing else - no pages, no mix, no plugins (proposal 37 3.5).
    'objects/midi':   _ENG_BASE | {'events'},
    # objects/track + events since proposal 37 P1: STrack owns the per-track
    # twEventClipSet and the twEventMerge feed (3.2.1).
    # objects/track + render + sidecar + sources since proposal 40 M1b:
    # SFeelFlowTrackBounce is a track-owned holder (not an SObject — the same
    # standing as an automation lane) that runs a background audio::
    # RenderSession bounce of the track's own root component (render), reads
    # the resulting WAV back with a bare twSampleSource rather than an
    # SPlainWave so the bounce never joins the project's extern-file list
    # (sources), and writes the groove.res/groove.ev aspects straight to
    # twSidecarStore (sidecar) — the same store objects/wave already reaches,
    # granted here too because the holder needs STrack's own chain
    # components (trackMixComponent()/pluginChainComponent()/
    # gainStageComponent()) for AC 3's per-chain pruning scope, and objects/
    # wave may not depend on objects/track (the wave<cut<track<mixer DAG runs
    # the other way).
    'objects/track':  _ENG_BASE | {'events', 'mix', 'plugins', 'render',
                                   'schedule', 'sidecar', 'sources'},
    # objects/fragment + mix (proposal 41 M1): SLaneFragment's root component
    # is ONE twTrackMix at unity (D1) — the minimal engine set that actually
    # compiles. No events (the residual event feed is M3), no plugins/render/
    # sidecar/sources — a fragment has no inserts, no bounce, nothing to
    # persist to the sidecar store, and no random source of its own.
    'objects/fragment': _ENG_BASE | {'mix'},
    'objects/mixer':  _ENG_BASE | {'mix', 'schedule'},
    'actions':        _ENG_BASE | {'render'},
    # media reaches NO engine module beyond the base every app module gets --
    # only tw/core's TW_LOG. A provider that knows what a page is has started
    # doing the engine's job.
    'media':          _ENG_BASE,
    'mediabrowser':   _ENG_BASE,
    # theme reaches NO engine module beyond the base -- only tw/core's TW_LOG.
    'theme':          _ENG_BASE,
    'persistence':    _ENG_BASE,
    'selection':      _ENG_BASE,
    # timeline + metering since proposal 34: the track head owns an SLevelMeter
    # and a twLevelProbe reading the track's frozen pages by position.
    # timeline + events since proposal 37 P1: the ruler and the snap grid read
    # the project's tempo map, which is the single tempo authority (D2).
    'timeline':       _ENG_BASE | {'devices', 'events', 'metering', 'pages',
                                   'playback', 'sources'},
    'pluginui':       _ENG_BASE | {'plugins'},
    # eventui + events since proposal 37 P4: the ruler and the piano roll grid
    # read the project's twTempoMap, the single tempo authority (D2). No other
    # engine module is in reach - the editor never touches a page.
    'eventui':        _ENG_BASE | {'events'},
    # servicesui + plugins since proposal 08 M2: the Options dialog's Plugins
    # page edits the scanner's search paths, and SOpt::def() takes the
    # per-platform defaults from twPluginSearchPaths rather than restating them.
    'servicesui':     _ENG_BASE | {'devices', 'playback', 'plugins', 'record',
                                   'render'},
    # shell + schedule since proposal 19 stage 4: startRender wires the
    # project's CaptureRevalidator (page scheduler) into the RenderSession
    # and quiesces background aspects around a scheduler-driven render.
    # shell + sidecar since proposal 27 M0: SApplication configures the
    # app-global sidecar store root at startup.
    # shell + plugins since proposal 08 M2: SApplication configures the plugin
    # registry at startup (search paths, cache path, probe executable) and polls
    # the background scan from a main-thread timer.
    # shell + metering since proposal 34: SApplication owns the metering pump and
    # the master level probe.
    # shell + events since proposal 37 P7b: the MIDI-out pump slices a track's
    # event FEED (twEventMerge/twEventBlock) on the main thread and hands the
    # bytes to tw/devices' MidiOutScheduler.
    # shell + mix + pages since proposal 21 L1b: the live plan builder
    # SNAPSHOTS the two pure-in-position pieces the pump replays outside the
    # graph - twGainStage::Envelope and twRewire::channelMap() - and checks the
    # master-shape precondition over the master's own twMixer/twRewire
    # (twlive::checkMasterShape, design D3). It reaches tw/pages for one
    # constant, twOutputPage::FRAME_CAPACITY, which is the stride the re-rooted
    # horizon demands are issued on.
    'shell':          _ENG_BASE | {'devices', 'dsp', 'events', 'metering',
                                   'mix', 'pages', 'playback', 'plugins',
                                   'record', 'render', 'schedule', 'sidecar'},
    # testkit + sidecar + schedule since proposal 27 M1 test verbs:
    # assert-sidecar reads twQafReader/twSidecarStore, wait-analysis polls the
    # revalidator's jobsQueued().
    # testkit + metering + pages since proposal 34: assert-meter freezes the page
    # for the position it asks about and runs the production twLevelProbe on it.
    # testkit + devices + playback + sinks since the capture backend:
    # dump-playback-capture reaches the live twSpeaker's backend (playback),
    # checks it is the CaptureBackend (devices), and writes its recording out
    # through the SAME 16-bit WAV writer the render path uses (sinks), so a
    # captured playback and a render are comparable files.
    # testkit + events since proposal 37 P1: assert-midi-file reads twSmf and
    # assert-midi-events drives a twEventSource collect.
    # testkit + sources since proposal 39 M1, and for ONE test only:
    # preview_envelope_test builds a stub twRandomSource so that its fixture
    # SCut is SAMPLE-backed - which is the branch of SCutRendererInline every
    # audio clip in the app takes. The alternative was to give the fixture a
    # non-Audio contentKind, i.e. to gate the collect seam on a path no real
    # clip travels. Nothing else in testkit names tw/sources, and a VERB that
    # wanted to should be questioned: a verb reads the MODEL, and the model
    # already hands out its source through SObject::getRandomSource().
    'testkit':        _ENG_BASE | {'analysis', 'devices', 'events', 'metering',
                                   'pages', 'playback', 'schedule', 'sidecar',
                                   'sinks', 'sources'},
}

# A file under <module>/tools/ is NOT part of that module's library.
#
# The DAG this script enforces is a constraint on LIBRARIES: tw_<mod> may only
# link what its row in DEPS allows, so that no cycle and no back-edge can exist
# between the static libs. A tools/ file is never compiled into tw_<mod>. It is
# its own add_executable with its own link line — `audio_backend_probe` is
# `add_executable(audio_backend_probe devices/tools/audio_backend_probe.cc)`
# followed by `target_link_libraries(... PRIVATE tw_devices tw_record tw_core)`
# (tw303a/CMakeLists.txt:860-864), and depending on two libraries is exactly
# what an executable is for.
#
# Attributing such a file to `tw_devices` made the check report a violation that
# the build does not have and could not have. That is a false positive, and a
# false positive in a mandatory gate is expensive: it fails `main` itself, so it
# fails every branch cut from main until someone works out it is not theirs.
#
# THE EXEMPTION IS THE MODULE-DAG CHECK ONLY. The app-header check still applies
# here, deliberately: an engine tool including an app header would be a real
# inversion whatever target it is compiled into, and no tools/ file does it
# today. Verified by putting one there and watching this script still report it.
#
# (Pre-existing and NOT changed here: APP_HEADERS only matches the LEGACY bare
# spelling — `#include "sobject.h"` — so the modern `#include
# "app/model/sobject.h"` slips past it anywhere in the engine, tools/ or not.
# Widening it is a separate change with its own blast radius.)
#
# `tests/` is deliberately NOT exempt — a test's includes have been treated as
# in-scope throughout (proposal 39 M1 added an explicit `testkit -> sources`
# edge rather than exempting the test that needed it).
TOOLS_DIR = os.sep + 'tools' + os.sep


def main():
    bad = []
    for mod in DEPS:
        allowed = closure(mod)
        moddir = os.path.join(TW, mod)
        for root, _dirs, files in os.walk(moddir):
            in_tools = TOOLS_DIR in (root + os.sep)
            for fn in files:
                if not fn.endswith(('.h', '.cc', '.cpp', '.mm')):
                    continue
                p = os.path.join(root, fn)
                for i, line in enumerate(open(p, encoding='utf-8',
                                              errors='replace'), 1):
                    if APP_HEADERS.search(line):
                        bad.append(f'{p}:{i}: engine file includes app header: '
                                   f'{line.strip()}')
                    if in_tools:
                        continue
                    m = TW_INCLUDE.search(line)
                    if m and m.group(1) not in allowed:
                        bad.append(f'{p}:{i}: {mod} may not include tw/'
                                   f'{m.group(1)} (allowed: '
                                   f'{", ".join(sorted(allowed))})')
    # ---- app side ----
    for mod in APP_DEPS:
        moddir = os.path.join(MAIN, *mod.split('/'))
        allowed_app = APP_DEPS[mod] | {mod}
        allowed_eng = APP_ENG[mod]
        for root, _dirs, files in os.walk(moddir):
            for fn in files:
                if not fn.endswith(('.h', '.cc', '.cpp')):
                    continue
                p = os.path.join(root, fn)
                for i, line in enumerate(open(p, encoding='utf-8',
                                              errors='replace'), 1):
                    m = TW_INCLUDE.search(line)
                    if m and m.group(1) not in allowed_eng:
                        bad.append(f'{p}:{i}: app/{mod} may not include tw/'
                                   f'{m.group(1)} (allowed: '
                                   f'{", ".join(sorted(allowed_eng))})')
                    a = APP_INCLUDE.search(line)
                    if a and a.group(1) not in allowed_app:
                        bad.append(f'{p}:{i}: app/{mod} -> app/{a.group(1)} is '
                                   f'not a declared edge (add it here '
                                   f'consciously or remove the include)')
    if bad:
        print('\n'.join(bad))
        print(f'\n{len(bad)} layering violation(s).')
        return 1
    print('layering clean.')
    return 0

if __name__ == '__main__':
    sys.exit(main())
