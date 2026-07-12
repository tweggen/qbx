#!/usr/bin/env python3
"""Layering checker (proposal 14 §6.2).

Engine side (build-enforced too; this is the greppable review view):
  1. No engine file includes an app (main/) header.
  2. Every cross-module include inside an engine module respects the declared
     module DAG (transitively).

App side (NOT build-enforced — the app is one strongly-connected component
built as a single OBJECT library until the Phase 6 interface work; this
checker is the only guard):
  3. Each app module's engine includes stay within its declared tw/ modules.
  4. App-internal cross-module includes stay within the DECLARED edge set
     (the measured coupling as of 2026-07-12). New edges must be added here
     consciously — shrinking this list is the Phase 6 burn-down.

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
    'sources':  ['core', 'pages', 'graph'],
    'dsp':      ['core', 'graph'],
    'mix':      ['core', 'pages', 'graph'],
    'plugins':  ['core', 'graph'],
    'devices':  ['core'],
    'sinks':    ['core'],
    'playback': ['core', 'pages', 'graph', 'devices', 'sources'],
    'render':   ['core', 'pages', 'graph', 'sinks', 'playback'],
    'record':   ['core', 'devices', 'sinks', 'sources'],
    'schedule': ['core', 'pages', 'graph'],
    'analysis': ['core'],
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
    'objects/cut':    {'actions', 'model', 'objects/mixer', 'objects/track',
                       'objects/wave', 'persistence'},
    'objects/wave':   {'actions', 'model', 'objects/cut', 'objects/mixer',
                       'objects/track', 'persistence'},
    'objects/track':  {'actions', 'model', 'objects/cut', 'objects/mixer',
                       'persistence'},
    'objects/mixer':  {'actions', 'model', 'objects/cut', 'objects/track',
                       'persistence'},
    'actions':        {'model'},
    'persistence':    {'actions', 'model'},
    'selection':      {'actions', 'model'},
    'timeline':       {'actions', 'model', 'objects/cut', 'objects/mixer',
                       'objects/track', 'objects/wave', 'pluginui',
                       'servicesui', 'shell'},
    'pluginui':       {'model', 'objects/mixer', 'objects/track', 'shell'},
    'servicesui':     {'model', 'shell'},
    'shell':          {'actions', 'model', 'objects/cut', 'objects/mixer',
                       'objects/track', 'objects/wave', 'persistence',
                       'selection', 'servicesui', 'testkit', 'timeline'},
    'testkit':        {'actions', 'model', 'objects/mixer', 'objects/track',
                       'shell'},
}

# Which engine modules each app module may include (tw/<mod>/... paths).
# core and graph are foundational and allowed everywhere.
_ENG_BASE = {'core', 'graph'}
APP_ENG = {
    'model':          _ENG_BASE | {'pages', 'schedule', 'sources'},
    'objects/cut':    _ENG_BASE | {'pages', 'schedule', 'sources'},
    'objects/wave':   _ENG_BASE | {'pages', 'schedule', 'sources'},
    'objects/track':  _ENG_BASE | {'mix', 'plugins', 'schedule'},
    'objects/mixer':  _ENG_BASE | {'mix', 'plugins', 'schedule'},
    'actions':        _ENG_BASE | {'render'},
    'persistence':    _ENG_BASE,
    'selection':      _ENG_BASE,
    'timeline':       _ENG_BASE | {'devices', 'playback', 'sources'},
    'pluginui':       _ENG_BASE | {'plugins'},
    'servicesui':     _ENG_BASE | {'devices', 'playback', 'record', 'render'},
    'shell':          _ENG_BASE | {'devices', 'dsp', 'playback', 'record',
                                   'render'},
    'testkit':        _ENG_BASE | {'analysis'},
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
