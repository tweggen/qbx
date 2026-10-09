# SmaragdVersion.cmake — one source of truth for what version this build is.
# =============================================================================
# Plan 50 M5, and D5's rule that the SUITE version is the only user-facing one
# while a component's own version is a developer/forensics number.
#
# Before this there were two hard-coded "1.0.0"s -- project(smaragd VERSION
# 1.0.0) and MACOSX_BUNDLE_BUNDLE_VERSION "1.0.0" -- plus a separate "1.0"
# short string. Two literals for one number is one too many, and the bundle
# pair was the half a user actually sees in Finder.
#
# nassau-suite passes NASSAU_SUITE_VERSION and NASSAU_SUITE_COMMIT into every
# component; a standalone build gets "dev"/"unknown", which is honest rather
# than a lie about being a release.
# =============================================================================

set(NASSAU_SUITE_VERSION "dev" CACHE STRING
    "Product version, supplied by the nassau-suite superbuild")
set(NASSAU_SUITE_COMMIT "unknown" CACHE STRING
    "nassau-suite commit the pin set came from")

# QBX-137 / plan 51: the user-visible product name, also supplied by the suite
# (from its PRODUCT file, the way the version comes from VERSION). It rides
# this pipeline rather than getting one of its own BECAUSE this pipeline exists
# as the answer to two hard-coded "1.0.0"s that drifted apart -- a second
# mechanism for a second name would be the same mistake in a new currency.
#
# The default is "Smaragd", not the shipped name: a standalone qbx build is not
# the product, and saying so is the same honesty as NASSAU_SUITE_VERSION
# defaulting to "dev" instead of claiming to be a release. Every qbx test run
# therefore executes under this default (plan 51 §8), which is why no test may
# assert a product-name literal.
#
# This is the DISPLAY name only. The identity keys that address the user's data
# -- the QSettings quadruple, com.smaragd.*, dev.tweggen.smaragd, the MIDI port
# names -- are frozen forever and are NOT derived from this (plan 51 §3,
# main/shell/CONTRACT.md invariant 41, tools/check_product_name.py).
set(NASSAU_PRODUCT_NAME "Smaragd" CACHE STRING
    "User-visible product name, supplied by the nassau-suite superbuild")

# The macOS bundle keys DERIVE from project() now, so they cannot drift from it.
set(SMARAGD_BUNDLE_VERSION       "${PROJECT_VERSION}")
set(SMARAGD_BUNDLE_SHORT_VERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smaragd_version.h.in"
    "${CMAKE_BINARY_DIR}/generated/smaragd_version.h"
    @ONLY)

message(STATUS "smaragd: version ${PROJECT_VERSION}, suite ${NASSAU_SUITE_VERSION} (${NASSAU_SUITE_COMMIT}), product \"${NASSAU_PRODUCT_NAME}\"")
