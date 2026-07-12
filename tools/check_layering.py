#!/usr/bin/env python3
"""Layering checker (proposal 14 §6.2).

Verifies two things the CMake DAG cannot express as greppable review rules:
  1. No engine file includes an app (main/) header.
  2. Every cross-module include inside an engine module respects the declared
     module DAG (a module may include its own headers and those of modules it
     depends on, transitively).

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
    # compat/ must contain nothing but forwarding headers (single tw/ include)
    compat = os.path.join(TW, 'compat')
    for root, _dirs, files in os.walk(compat):
        for fn in files:
            p = os.path.join(root, fn)
            body = open(p, encoding='utf-8', errors='replace').read()
            if len(TW_INCLUDE.findall(body)) != 1:
                bad.append(f'{p}: compat header must forward to exactly one '
                           f'tw/<module> header')
    if bad:
        print('\n'.join(bad))
        print(f'\n{len(bad)} layering violation(s).')
        return 1
    print('layering clean.')
    return 0

if __name__ == '__main__':
    sys.exit(main())
