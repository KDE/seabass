# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

# Prints every backup record's manifest under DIR, for a test that matches
# what a backup holds with PASS_REGULAR_EXPRESSION.
#
#   -DDIR=<a stick's Seabass/backups folder>

file(GLOB manifests "${DIR}/*/.manifest")
list(SORT manifests)
foreach(manifest IN LISTS manifests)
    file(READ "${manifest}" text)
    execute_process(COMMAND ${CMAKE_COMMAND} -E echo_append "${text}")
endforeach()
