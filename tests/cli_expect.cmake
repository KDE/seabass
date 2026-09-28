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
#
# stdout and stderr are checked together, as a person reads them. stdin
# is empty, so any confirmation prompt is answered "no".

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
