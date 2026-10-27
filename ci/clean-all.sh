#!/usr/bin/env bash
#
# Cleans every repo of the suite that lives beside this one on the hard drive,
# the counterpart of ci/install-all.sh. Stops at the first repo that fails.
#

set -Eeuo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

VCPKG_ROOT="$ROOT/vcpkg"
export VCPKG_ROOT

export QBX_PARENT="${QBX_PARENT:-$ROOT/..}"

current=""
trap '[ "$BASH_SUBSHELL" -eq 0 ] && { echo "clean-all: FAILED in $current" >&2; trap - ERR; }' ERR

# Runs one command in one repo. Not called from a conditional (no `||`,
# `&&` or `if` around it), so set -e still applies inside.
in_repo() {
	current="$1"
	shift
	echo "==> $current: $*"
	( cd "$QBX_PARENT/$current" && "$@" )
}

for i in nassau-plugin-sdk nassau-dsp nassau-digital qbx
do
	in_repo "$i" ./ci/clean.sh
done

for i in nassau-analogue nassau-bloom nassau-eq nassau-mangrove2 nassau-zermatt
do
	in_repo "$i" ./ci/clean.sh
done

echo "clean-all: done"
