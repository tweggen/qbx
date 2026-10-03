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

# The macOS bundle keys DERIVE from project() now, so they cannot drift from it.
set(SMARAGD_BUNDLE_VERSION       "${PROJECT_VERSION}")
set(SMARAGD_BUNDLE_SHORT_VERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smaragd_version.h.in"
    "${CMAKE_BINARY_DIR}/generated/smaragd_version.h"
    @ONLY)

message(STATUS "smaragd: version ${PROJECT_VERSION}, suite ${NASSAU_SUITE_VERSION} (${NASSAU_SUITE_COMMIT})")
