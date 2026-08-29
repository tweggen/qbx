#!/bin/sh
# Builds body_probe, the feel-flow body-physics probe (see body_probe.cc).
#
# There is a CMake target too (`body_probe`), but body_probe links tw_sidecar's
# three groove sources and nothing else -- no Qt, no audio file library, no
# engine -- so it can be built with a bare compiler and no configure step, which
# is what this script is for. Run from anywhere:
#
#   sh smaragd/tw303a/sidecar/tools/build_body_probe.sh [outdir]
#   ./body_probe smaragd/tests/groove/h_fill_break.wav 18.22 22.22
#
set -e
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
sidecar=$(dirname "$here")
tw303a=$(dirname "$sidecar")
out=${1:-.}
${CXX:-g++} -O2 -std=c++17 \
    -I"$sidecar/include" -I"$tw303a/core/include" -I"$tw303a/body/include" \
    -o "$out/body_probe" \
    "$here/body_probe.cc" \
    "$sidecar/src/twgroove.cc" \
    "$sidecar/src/twgroovependulum.cc" \
    "$sidecar/src/twgrooveaspect.cc" \
    "$tw303a/body/src/twbodymeasures.cc"
echo "built $out/body_probe"
