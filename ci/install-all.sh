#!/usr/bin/env bash
#
# This is for building the entire suite during development.
# It builds everything in the respective repos as they are living side by
# side on hard drive.
# For deployment, look into the nassau-suite.
#
# Stops at the first repo whose pull, build or install fails, and names it.
#

set -Eeuo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

VCPKG_ROOT="$ROOT/vcpkg"
export VCPKG_ROOT

export QBX_PARENT="${QBX_PARENT:-$ROOT/..}"

current=""
trap '[ "$BASH_SUBSHELL" -eq 0 ] && { echo "install-all: FAILED in $current" >&2; trap - ERR; }' ERR

# Runs one command in one repo. Not called from a conditional (no `||`,
# `&&` or `if` around it), so set -e still applies inside.
in_repo() {
	current="$1"
	shift
	echo "==> $current: $*"
	( cd "$QBX_PARENT/$current" && "$@" )
}

# nassau-dsp before the plugins: nassau-analogue builds against it.
for i in nassau-plugin-sdk nassau-dsp qbx
do
	in_repo "$i" git pull
	in_repo "$i" ./ci/build.sh
done

for i in nassau-analogue nassau-bloom nassau-eq nassau-mangrove2 nassau-zermatt
do
	in_repo "$i" git pull
	in_repo "$i" ./ci/build.sh
	in_repo "$i" ./ci/install.sh
done

echo "install-all: done"
