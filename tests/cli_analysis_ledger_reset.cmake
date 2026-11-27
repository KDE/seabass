# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

# seabass-cli analysis-ledger --reset, on a scratch stick:
#
# 1. A ledger that cannot be read (a folder in its place): the listing
#    exits 1 and names --reset; --reset moves it aside, whole, to
#    analysis.tsv.gz.unreadable-<UTC time> in the same folder, and exits 0.
# 2. --reset with no ledger: exits 1, nothing moved.
# 3. A damaged ledger: moved aside the same way.
# 4. A ledger that can be read: --reset refuses, exits 1, the file stays
#    byte for byte.
#
#   -DCLI=<seabass-cli> -DWRITE_LEDGER=<write_empty_analysis_ledger>
#   -DSCRATCH=<a directory of the test's own>

foreach(required CLI WRITE_LEDGER SCRATCH)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "cli_analysis_ledger_reset.cmake: ${required} is not set")
    endif()
endforeach()

set(stick "${SCRATCH}/stick")
set(seabass "${stick}/Seabass")
set(ledger "${seabass}/analysis.tsv.gz")
file(REMOVE_RECURSE "${SCRATCH}")
file(MAKE_DIRECTORY "${stick}/PIONEER/rekordbox")

if(CMAKE_HOST_WIN32)
    set(null_device NUL)
else()
    set(null_device /dev/null)
endif()

set(failures "")
# Runs seabass-cli with `args` (a list); the output in `out`, checked for
# the exit code and each pattern of `expect` (a list).
function(run_cli label exit_code expect)
    execute_process(
        COMMAND "${CLI}" ${ARGN}
        INPUT_FILE "${null_device}"
        OUTPUT_VARIABLE out
        ERROR_VARIABLE out
        RESULT_VARIABLE code
        TIMEOUT 120)
    if(NOT "${code}" STREQUAL "${exit_code}")
        set(failures "${failures}${label}: exit code ${code}, expected ${exit_code}\n${out}\n" PARENT_SCOPE)
        return()
    endif()
    foreach(pattern IN LISTS expect)
        if(NOT out MATCHES "${pattern}")
            set(failures "${failures}${label}: output does not match: ${pattern}\n${out}\n" PARENT_SCOPE)
            return()
        endif()
    endforeach()
endfunction()

function(moved_aside out)
    file(GLOB found LIST_DIRECTORIES true "${seabass}/analysis.tsv.gz.unreadable-*")
    list(SORT found)
    set(${out} "${found}" PARENT_SCOPE)
endfunction()

# 1.
file(MAKE_DIRECTORY "${ledger}/in the way")
run_cli("unreadable, listed" 1 "cannot be used: .*not a regular file;--reset to move it aside"
    analysis-ledger --rekordbox "${stick}/PIONEER")
run_cli("unreadable, reset" 0
    "It cannot be used: .*not a regular file;Moved it aside to .*analysis\\.tsv\\.gz\\.unreadable-[0-9]+T[0-9]+Z\\. Nothing was deleted\\."
    analysis-ledger --rekordbox "${stick}/PIONEER" --reset)
moved_aside(found)
list(LENGTH found count)
if(EXISTS "${ledger}" OR NOT count EQUAL 1)
    string(APPEND failures "unreadable, reset: the ledger is still there, or not one was moved aside: ${found}\n")
elseif(NOT EXISTS "${found}/in the way")
    string(APPEND failures "unreadable, reset: what was moved aside is not the folder whole: ${found}\n")
elseif(NOT found MATCHES "/analysis\\.tsv\\.gz\\.unreadable-[0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9]T[0-9][0-9][0-9][0-9][0-9][0-9]Z$")
    string(APPEND failures "unreadable, reset: not named for the UTC time: ${found}\n")
endif()

# 2.
run_cli("none, reset" 1 "found no record on this stick: nothing was moved"
    analysis-ledger --rekordbox "${stick}/PIONEER" --reset)

# 3.
file(WRITE "${ledger}" "not a ledger at all")
run_cli("damaged, reset" 0 "Moved it aside to"
    analysis-ledger --rekordbox "${stick}/PIONEER" --reset)
moved_aside(found)
list(LENGTH found count)
if(EXISTS "${ledger}" OR NOT count EQUAL 2)
    string(APPEND failures "damaged, reset: not moved aside beside the first: ${found}\n")
endif()

# 4. An empty ledger, written by the ledger's own writer.
execute_process(COMMAND "${WRITE_LEDGER}" "${stick}" RESULT_VARIABLE written OUTPUT_VARIABLE out ERROR_VARIABLE out
    TIMEOUT 120)
if(NOT written EQUAL 0 OR NOT EXISTS "${ledger}")
    message(FATAL_ERROR "cli_analysis_ledger_reset.cmake: no empty ledger written (${written}): ${out}")
endif()
file(SHA256 "${ledger}" before)
run_cli("readable, listed" 0 "It lists no track\\."
    analysis-ledger --rekordbox "${stick}/PIONEER")
run_cli("readable, reset" 1 "this one can be read: nothing was moved"
    analysis-ledger --rekordbox "${stick}/PIONEER" --reset)
set(after "gone")
if(EXISTS "${ledger}")
    file(SHA256 "${ledger}" after)
endif()
moved_aside(found)
list(LENGTH found count)
if(NOT before STREQUAL after OR NOT count EQUAL 2)
    string(APPEND failures "readable, reset: the ledger was changed or moved\n")
endif()

if(failures)
    message(FATAL_ERROR "${failures}")
endif()
message(STATUS "analysis-ledger --reset: moved aside what cannot be used, never what can be read")
