# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

# Every Breeze icon the QML names is bundled, and the three places that
# list the bundle agree.
#
# An icon name is a string: a typo, or a name nobody copied in, compiles
# and runs and draws nothing -- an empty square where a button's only
# content was. So this reads the QML for every name passed where an icon
# is expected (Theme.iconUrl(...), iconName:, cardIcon:,
# cardSubtitleIcon:) and checks the file is there, then checks that
# tools/update-breeze-icons.sh (what copies them), the directory (what is
# there) and src/gui/CMakeLists.txt (what is compiled in) name the same
# set.
#
#   cmake -DSOURCE_DIR=<repo> -P tests/breeze_icons_test.cmake

cmake_minimum_required(VERSION 3.20)

if(NOT SOURCE_DIR)
    message(FATAL_ERROR "pass -DSOURCE_DIR=<repository root>")
endif()

set(iconDir "${SOURCE_DIR}/src/gui/qml/icons/breeze")
set(failures "")

# What is in the directory.
file(GLOB bundledFiles RELATIVE "${iconDir}" "${iconDir}/*.svg")
list(TRANSFORM bundledFiles REPLACE "\\.svg$" "")
list(SORT bundledFiles)
list(LENGTH bundledFiles bundledCount)
if(bundledCount EQUAL 0)
    message(FATAL_ERROR "no icons found in ${iconDir}")
endif()

# What the update script copies.
file(STRINGS "${SOURCE_DIR}/tools/update-breeze-icons.sh" scriptLines REGEX "^[a-z]+/22/[a-z0-9-]+\\.svg$")
set(scriptNames "")
foreach(line IN LISTS scriptLines)
    string(REGEX REPLACE "^.*/([a-z0-9-]+)\\.svg$" "\\1" name "${line}")
    list(APPEND scriptNames "${name}")
endforeach()
list(SORT scriptNames)

# What CMake compiles in.
file(STRINGS "${SOURCE_DIR}/src/gui/CMakeLists.txt" cmakeLines REGEX "qml/icons/breeze/[a-z0-9-]+\\.svg")
set(cmakeNames "")
foreach(line IN LISTS cmakeLines)
    string(REGEX REPLACE "^.*qml/icons/breeze/([a-z0-9-]+)\\.svg.*$" "\\1" name "${line}")
    list(APPEND cmakeNames "${name}")
endforeach()
list(SORT cmakeNames)

if(NOT scriptNames STREQUAL bundledFiles)
    string(APPEND failures "tools/update-breeze-icons.sh lists [${scriptNames}]\n  but the directory holds [${bundledFiles}]\n")
endif()
if(NOT cmakeNames STREQUAL bundledFiles)
    string(APPEND failures "src/gui/CMakeLists.txt lists [${cmakeNames}]\n  but the directory holds [${bundledFiles}]\n")
endif()

# What the QML asks for. Every quoted name on a line that passes an icon,
# so both halves of `expanded ? "arrow-down" : "arrow-right"` are checked.
file(GLOB_RECURSE qmlFiles "${SOURCE_DIR}/src/gui/qml/*.qml" "${SOURCE_DIR}/tests/qml/*.qml")
set(usedCount 0)
foreach(qml IN LISTS qmlFiles)
    file(STRINGS "${qml}" lines REGEX "(iconUrl\\(|iconName:|cardIcon:|cardSubtitleIcon:)")
    foreach(line IN LISTS lines)
        if(line MATCHES "^[ \t]*//")
            continue()
        endif()
        string(REGEX MATCHALL "\"[a-z0-9-]+\"" quoted "${line}")
        foreach(q IN LISTS quoted)
            string(REPLACE "\"" "" name "${q}")
            math(EXPR usedCount "${usedCount} + 1")
            if(NOT name IN_LIST bundledFiles)
                file(RELATIVE_PATH rel "${SOURCE_DIR}" "${qml}")
                string(APPEND failures "${rel} asks for icon \"${name}\", which is not bundled\n")
            endif()
        endforeach()
    endforeach()
endforeach()
# A guard on the guard: the patterns above silently matching nothing would
# pass every check.
if(usedCount LESS 40)
    string(APPEND failures "found only ${usedCount} icon names in the QML; the patterns above no longer match how icons are named\n")
endif()

if(failures)
    message(FATAL_ERROR "Bundled Breeze icons are out of step:\n${failures}")
endif()
message(STATUS "${bundledCount} bundled icons, ${usedCount} uses in QML, all present")
