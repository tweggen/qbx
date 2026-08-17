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
    'devices':  ['core'],
    'sinks':    ['core'],
    # playback → schedule since proposal 19 stage 5: the readahead is a
    # demand consumer of the page scheduler instead of pulling freezes.
    'playback': ['core', 'pages', 'graph', 'devices', 'sources', 'schedule'],
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
    'objects/mixer':  {'actions', 'model', 'objects/cut', 'objects/track',
                       'persistence'},
    'actions':        {'model'},
    'persistence':    {'actions', 'model'},
    'selection':      {'actions', 'model'},
    # timeline + objects/midi since proposal 37 P1: the Clip Properties dock
    # grows an SMidiCut page, and the ruler's Set BPM commits through set-tempo.
    'timeline':       {'actions', 'model', 'objects/cut', 'objects/midi',
                       'objects/mixer', 'objects/track', 'objects/wave',
                       'pluginui', 'servicesui', 'shell'},
    'pluginui':       {'model', 'objects/mixer', 'objects/track', 'shell'},
    # eventui (proposal 37 P4) sits at the RANK of pluginui: a UI slice that
    # edits ONE object kind through the action system and is hosted by the
    # shell. It deliberately does NOT reach `timeline`: the arranger's zoom and
    # scroll arrive through the SHELL, which already depends on both, so the
    # editor stays independent of the 4000-line arranger. objects/mixer +
    # objects/track are the virtual keyboard's "first event clip on the
    # SELECTED track" fallback.
    'eventui':        {'actions', 'model', 'objects/midi', 'objects/mixer',
                       'objects/track', 'shell'},
    'servicesui':     {'model', 'shell'},
    # shell + objects/midi since proposal 37 P1: the transport tempo box
    # commits through the set-tempo verb instead of writing the project.
    # shell + eventui since proposal 37 P4: the event editor and the virtual
    # keyboard are docks, created in SMainWindow's ctor like every other one.
    'shell':          {'actions', 'eventui', 'model', 'objects/cut',
                       'objects/midi', 'objects/mixer', 'objects/track',
                       'objects/wave', 'persistence', 'selection',
                       'servicesui', 'testkit', 'timeline'},
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
    'testkit':        {'actions', 'model', 'objects/cut', 'objects/midi',
                       'objects/mixer', 'objects/track', 'objects/wave',
                       'pluginui', 'servicesui', 'shell'},
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
    'objects/track':  _ENG_BASE | {'events', 'mix', 'plugins', 'schedule'},
    'objects/mixer':  _ENG_BASE | {'mix', 'schedule'},
    'actions':        _ENG_BASE | {'render'},
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
    'shell':          _ENG_BASE | {'devices', 'dsp', 'events', 'metering',
                                   'playback', 'plugins', 'record', 'render',
                                   'schedule', 'sidecar'},
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
    'testkit':        _ENG_BASE | {'analysis', 'devices', 'events', 'metering',
                                   'pages', 'playback', 'schedule', 'sidecar',
                                   'sinks'},
}

def main():
    bad = []
    for mod in DEPS:
        allowed = closure(mod)
        moddir = os.path.join(TW, mod)
        for root, _dirs, files in os.walk(moddir):
            for fn in files:
                if not fn.endswith(('.h', '.cc', '.cpp', '.mm')):
                    continue
                p = os.path.join(root, fn)
                for i, line in enumerate(open(p, encoding='utf-8',
                                              errors='replace'), 1):
                    if APP_HEADERS.search(line):
                        bad.append(f'{p}:{i}: engine file includes app header: '
                                   f'{line.strip()}')
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
