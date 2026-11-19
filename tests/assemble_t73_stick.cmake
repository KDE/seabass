# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

# Puts the TESTSHARK fixture (tests/fixtures/testshark_t73) back together
# as the stick it was: its PIONEER folder and audio heads, with one of the
# Engine databases it holds as "Engine Library/Database2/m.db". OUT is
# deleted first. The fixture keeps the databases apart, one per step, and
# leaves out the overview files (see its README).
#
#   -DFIXTURE=<tests/fixtures/testshark_t73> -DDB=<file name under engine/> -DOUT=<directory>

foreach(required FIXTURE DB OUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "assemble_t73_stick.cmake: ${required} is not set")
    endif()
endforeach()
file(REMOVE_RECURSE "${OUT}")
file(COPY "${FIXTURE}/stick/" DESTINATION "${OUT}")
file(MAKE_DIRECTORY "${OUT}/Engine Library/Database2")
file(COPY_FILE "${FIXTURE}/engine/${DB}" "${OUT}/Engine Library/Database2/m.db")
