# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

# Runs seabass-cli once and checks what it did: the exit code, what its
# output must say and must not say, and that the catalogs it was pointed
# at are byte for byte what they were before.
#
# CTest's own PASS_REGULAR_EXPRESSION ignores the exit code, and WILL_FAIL
# ignores the output, so neither alone can say "exits 0 AND reports 1161
# tracks". This does both.
#
#   -DCLI=<seabass-cli>
#   -DARGS=<arguments, separated by |>
#   -DEXIT=<expected exit code>
#   -DEXPECT=<regular expressions the output must match, separated by |>
#   -DFORBID=<regular expressions it must not match, separated by |>
#   -DUNCHANGED=<files whose SHA-256 must be the same afterwards, separated by |>
#   -DCOUNT=<N=regex pairs, separated by |: the output must match regex exactly N times>
#   -DCOPY_FROM=<directory> -DCOPY_TO=<directory>: COPY_TO is deleted and
#       made a fresh copy of COPY_FROM before the run, for a command pointed
#       at a fixture it must not be pointed at in place
#   -DUNCHANGED_TREE=<directory: no file under it may appear, go or change>
#
# stdout and stderr are checked together, as a person reads them. stdin
# is empty, so any confirmation prompt is answered "no".

# if(IN_LIST), below; a -P script starts with no policies set.
cmake_policy(SET CMP0057 NEW)

foreach(required CLI EXIT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "cli_expect.cmake: ${required} is not set")
    endif()
endforeach()

string(REPLACE "|" ";" args "${ARGS}")
# A pattern spells a newline as the two characters \n: a real one does
# not survive the command line.
string(REPLACE "|" ";" expect "${EXPECT}")
string(REPLACE "\\n" "\n" expect "${expect}")
string(REPLACE "|" ";" forbid "${FORBID}")
string(REPLACE "\\n" "\n" forbid "${forbid}")
string(REPLACE "|" ";" unchanged "${UNCHANGED}")
string(REPLACE "|" ";" counts "${COUNT}")
string(REPLACE "\\n" "\n" counts "${counts}")

if(DEFINED COPY_FROM AND NOT COPY_FROM STREQUAL "")
    file(REMOVE_RECURSE "${COPY_TO}")
    file(COPY "${COPY_FROM}/" DESTINATION "${COPY_TO}")
endif()

# Every file under the tree with its SHA-256, one "path=digest" each.
function(tree_digests dir out)
    file(GLOB_RECURSE files LIST_DIRECTORIES false RELATIVE "${dir}" "${dir}/*")
    list(SORT files)
    set(result "")
    foreach(file IN LISTS files)
        file(SHA256 "${dir}/${file}" digest)
        list(APPEND result "${file}=${digest}")
    endforeach()
    set(${out} "${result}" PARENT_SCOPE)
endfunction()
if(DEFINED UNCHANGED_TREE AND NOT UNCHANGED_TREE STREQUAL "")
    tree_digests("${UNCHANGED_TREE}" tree_before)
    list(LENGTH tree_before tree_files)
    if(tree_files EQUAL 0)
        message(FATAL_ERROR "cli_expect.cmake: ${UNCHANGED_TREE} holds no file, so it cannot be watched")
    endif()
endif()

set(before "")
foreach(file IN LISTS unchanged)
    if(NOT EXISTS "${file}")
        message(FATAL_ERROR "cli_expect.cmake: ${file} does not exist, so it cannot be watched")
    endif()
    file(SHA256 "${file}" digest)
    list(APPEND before "${digest}")
endforeach()

if(CMAKE_HOST_WIN32)
    set(null_device NUL)
else()
    set(null_device /dev/null)
endif()
execute_process(
    COMMAND "${CLI}" ${args}
    INPUT_FILE "${null_device}"
    OUTPUT_VARIABLE out
    ERROR_VARIABLE out
    RESULT_VARIABLE code
    TIMEOUT 600)

set(failures "")
if(NOT "${code}" STREQUAL "${EXIT}")
    string(APPEND failures "exit code ${code}, expected ${EXIT}\n")
endif()
foreach(pattern IN LISTS expect)
    if(NOT out MATCHES "${pattern}")
        string(APPEND failures "output does not match: ${pattern}\n")
    endif()
endforeach()
foreach(pattern IN LISTS forbid)
    if(out MATCHES "${pattern}")
        string(APPEND failures "output matches what it must not: ${pattern}\n")
    endif()
endforeach()
foreach(pair IN LISTS counts)
    string(FIND "${pair}" "=" equals)
    string(SUBSTRING "${pair}" 0 ${equals} wanted)
    math(EXPR start "${equals} + 1")
    string(SUBSTRING "${pair}" ${start} -1 pattern)
    string(REGEX MATCHALL "${pattern}" found "${out}")
    list(LENGTH found times)
    if(NOT times EQUAL wanted)
        string(APPEND failures "output matches ${times} times, expected ${wanted}: ${pattern}\n")
    endif()
endforeach()
if(DEFINED UNCHANGED_TREE AND NOT UNCHANGED_TREE STREQUAL "")
    tree_digests("${UNCHANGED_TREE}" tree_after)
    if(NOT tree_after STREQUAL tree_before)
        foreach(entry IN LISTS tree_after)
            if(NOT entry IN_LIST tree_before)
                string(APPEND failures "${UNCHANGED_TREE}: new or changed: ${entry}\n")
            endif()
        endforeach()
        foreach(entry IN LISTS tree_before)
            if(NOT entry IN_LIST tree_after)
                string(APPEND failures "${UNCHANGED_TREE}: gone or changed: ${entry}\n")
            endif()
        endforeach()
    endif()
    message(STATUS "${UNCHANGED_TREE}: ${tree_files} file(s) watched")
endif()
set(index 0)
foreach(file IN LISTS unchanged)
    list(GET before ${index} was)
    if(NOT EXISTS "${file}")
        string(APPEND failures "${file} is gone\n")
    else()
        file(SHA256 "${file}" now)
        if(NOT now STREQUAL was)
            string(APPEND failures "${file} changed\n")
        endif()
    endif()
    math(EXPR index "${index} + 1")
endforeach()

if(failures)
    message(FATAL_ERROR "seabass-cli ${args}\n${failures}output was:\n${out}")
endif()
message(STATUS "seabass-cli ${args}: exit ${code}, output as expected")
