# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

# Fails when tests/fixtures holds any file git does not track, ignored
# ones included (#111): what a test leaves there when it opens a committed
# fixture in place instead of a copy, such as the -wal and -shm the first
# SQLite connection to a WAL-mode database makes beside it. Such a stray
# is ignored by git, so nobody sees it, and a later test can come to
# depend on it (track_field_edit_change_test did, and failed only in a
# fresh checkout).
#
# Registered as the cleanup of a ctest fixture every test requires, so it
# runs after all of them, in a full run and in a -R subset alike.
#
#   -DGIT=<git>
#   -DSOURCE=<the source tree>

foreach(required GIT SOURCE)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "fixtures_left_clean.cmake: ${required} is not set")
    endif()
endforeach()

# -x: untracked files, ignored or not. -d: whole untracked folders.
execute_process(
    COMMAND "${GIT}" -C "${SOURCE}" clean -ndx -- tests/fixtures
    OUTPUT_VARIABLE strays
    ERROR_VARIABLE error
    RESULT_VARIABLE code)
if(NOT code EQUAL 0)
    message(FATAL_ERROR "fixtures_left_clean.cmake: git clean -ndx tests/fixtures failed (${code}): ${error}")
endif()
if(NOT strays STREQUAL "")
    message(FATAL_ERROR
        "tests/fixtures holds files git does not track. A test opened or wrote a committed fixture in place "
        "instead of a copy of it; find which, make it work on a copy, then remove these "
        "(git clean -fdx tests/fixtures):\n${strays}")
endif()
message(STATUS "tests/fixtures holds only what git tracks")
