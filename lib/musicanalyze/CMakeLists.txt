# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: BSD-2-Clause

# musicanalyze: the beat grid, key and waveform of a track from its audio.
# Builds on its own (cmake -S lib/musicanalyze) and as a subdirectory of
# whatever carries it; needs a C++20 compiler and nothing else.
cmake_minimum_required(VERSION 3.16)

# The version is written down once, in version.hpp.
set(_musicanalyze_version "")
foreach(_musicanalyze_part MAJOR MINOR PATCH)
    file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/include/musicanalyze/version.hpp" _musicanalyze_line
         REGEX "^#define MUSICANALYZE_VERSION_${_musicanalyze_part} [0-9]+$")
    if(NOT _musicanalyze_line MATCHES "^#define MUSICANALYZE_VERSION_${_musicanalyze_part} ([0-9]+)$")
        message(FATAL_ERROR "musicanalyze: version.hpp does not say MUSICANALYZE_VERSION_${_musicanalyze_part} in one line")
    endif()
    list(APPEND _musicanalyze_version "${CMAKE_MATCH_1}")
endforeach()
string(REPLACE ";" "." _musicanalyze_version "${_musicanalyze_version}")

project(musicanalyze VERSION ${_musicanalyze_version} LANGUAGES CXX)

# Static only for now: nothing is marked for export from a shared library.
add_library(musicanalyze STATIC
    src/analysis.cpp
    src/grid.cpp
    src/key.cpp
    src/version.cpp
    src/waveform.cpp
)
add_library(musicanalyze::musicanalyze ALIAS musicanalyze)
target_include_directories(musicanalyze PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
# C++20 and no more, whatever the project around it builds with: the
# library is compiled as strict C++20 everywhere, so it stays usable from
# one that is at C++20. Its headers ask a caller for C++20 at the least.
set_target_properties(musicanalyze PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)
target_compile_features(musicanalyze PUBLIC cxx_std_20)

# No multiply and add fused into one instruction (which rounds once where
# the two round twice): Clang does that by default where the processor
# has the instruction, as on Apple's ARM, and a grid decided by the last
# digit would come out differently there under the same revision.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(musicanalyze PRIVATE -ffp-contract=off)
endif()

# The grid analyzer filters a whole track ten times over: built optimized
# even in a Debug build (GCC and Clang), so a test suite that analyzes
# audio is not several times slower for it.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(src/grid.cpp PROPERTIES COMPILE_OPTIONS "$<$<CONFIG:Debug>:-O2>")
endif()

get_directory_property(_musicanalyze_has_parent PARENT_DIRECTORY)

# The library's own test: built where this is the project being built,
# and where a project that carries the library asks for it. Such a project
# has to have called enable_testing() before it adds this directory, or
# the test is built and not registered.
if(_musicanalyze_has_parent)
    option(MUSICANALYZE_TESTS "Build musicanalyze's own test" OFF)
else()
    option(MUSICANALYZE_TESTS "Build musicanalyze's own test" ON)
endif()
if(MUSICANALYZE_TESTS)
    if(NOT _musicanalyze_has_parent)
        enable_testing()
    endif()
    add_executable(musicanalyze_test tests/musicanalyze_test.cpp)
    target_link_libraries(musicanalyze_test PRIVATE musicanalyze)
    set_target_properties(musicanalyze_test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    add_test(NAME musicanalyze_test COMMAND musicanalyze_test "${CMAKE_CURRENT_SOURCE_DIR}/tests/results.txt")
endif()

# Installed only where it is the project being built: carried inside
# another, it is that project's own static library.
if(NOT _musicanalyze_has_parent)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)
    install(TARGETS musicanalyze EXPORT musicanalyzeTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(DIRECTORY include/musicanalyze DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(EXPORT musicanalyzeTargets NAMESPACE musicanalyze:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/musicanalyze)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/musicanalyzeConfig.cmake"
         "include(\"\${CMAKE_CURRENT_LIST_DIR}/musicanalyzeTargets.cmake\")\n")
    # Before 1.0 a minor version may change the interface.
    write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/musicanalyzeConfigVersion.cmake"
                                     VERSION ${PROJECT_VERSION} COMPATIBILITY SameMinorVersion)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/musicanalyzeConfig.cmake" "${CMAKE_CURRENT_BINARY_DIR}/musicanalyzeConfigVersion.cmake"
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/musicanalyze)
endif()
