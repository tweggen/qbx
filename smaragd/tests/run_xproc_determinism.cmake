# Driver for the CROSS-PROCESS determinism gate (proposal 37 P3c, AC1 + AC2c).
#
# The byte-exact render gate this repo rests on is render-vs-render in FRESH
# PROCESSES (design F4). A .qxa case is one process, so no single case can make
# that compare; and the qxa registration in smaragd/CMakeLists.txt gives every
# case a PRIVATE --test-output-dir, so two cases cannot see each other's files
# either. This script is the smallest thing that expresses it: run two cases,
# in order, into ONE output directory, and let the second one do the comparing
# with the ordinary `assert-file-identical` verb (which accepts the shared
# directory's relative spellings, and absolute paths too).
#
# Invoked as:
#   cmake -DRUNNER=<smaragd exe> -DCASE1=<...qxa> -DCASE2=<...qxa>
#         -DOUTDIR=<dir> -DWORKDIR=<tests/cases>
#         -P run_xproc_determinism.cmake
#
# Run from a clean OUTDIR every time: a stale det_a.wav from an older binary
# would otherwise be compared against a fresh det_c.wav and read as a
# determinism failure when it is really a leftover.

foreach(_var RUNNER CASE1 CASE2 OUTDIR WORKDIR)
    if(NOT DEFINED ${_var})
        message(FATAL_ERROR "run_xproc_determinism.cmake: -D${_var}= is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${OUTDIR}")
file(MAKE_DIRECTORY "${OUTDIR}")

foreach(_case "${CASE1}" "${CASE2}")
    message(STATUS "xproc: running ${_case}")
    execute_process(
        COMMAND "${RUNNER}" --test-case "${_case}" --test-output-dir "${OUTDIR}"
        WORKING_DIRECTORY "${WORKDIR}"
        RESULT_VARIABLE _rc
    )
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "xproc: ${_case} exited with ${_rc}")
    endif()
endforeach()

# Belt and braces. The verb inside pass 2 has already compared these; doing it
# again here means a pass-2 script that silently stopped short of its assertions
# (a parse error in a future edit, say) cannot report success.
foreach(_pair "det_c.wav" "det_d.wav")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E compare_files
                "${OUTDIR}/det_a.wav" "${OUTDIR}/${_pair}"
        RESULT_VARIABLE _cmp
    )
    if(NOT _cmp EQUAL 0)
        message(FATAL_ERROR "xproc: det_a.wav and ${_pair} differ")
    endif()
endforeach()

message(STATUS "xproc: det_a.wav == det_c.wav == det_d.wav")
