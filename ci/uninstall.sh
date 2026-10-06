#!/usr/bin/env bash
#
# ci/uninstall.sh — remove the locally installed development build of Smaragd.
#
#     ./ci/uninstall.sh            # from the per-user location (the default install)
#     ./ci/uninstall.sh --system   # from the shared location
#     ./ci/uninstall.sh -n         # print what would go, remove nothing
#
# A FORWARDER, NOT A SECOND IMPLEMENTATION. An uninstaller that removes something
# other than exactly what the installer wrote is worse than no uninstaller at
# all, and the destination table is the thing that would drift: two files, two
# chances to disagree about where ~/Applications stops and /usr/local starts.
# So the table lives once, in ci/install.sh, and this is the door to it.
#
# Removing nothing is SUCCESS here, not failure: "already absent" is the state
# this script exists to reach, and a non-zero exit would break the obvious loop
# `for r in */; do "$r/ci/uninstall.sh"; done`.
set -euo pipefail

exec "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/install.sh" --uninstall "$@"
