# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

# Tier 2 test fixtures: real analysis data, de-identified, without audio,
# too large for the repository and published as a zip on the website
# instead (see "Test fixtures in three tiers" in docs/testing.md).
#
# Nothing is fetched unless asked for. With SEABASS_FETCH_FIXTURES=ON the
# zip is downloaded at configure time, checked against the SHA-256 pinned
# below, and unpacked into ${CMAKE_BINARY_DIR}/fixtures. A directory given
# as SEABASS_REAL_ANALYSIS_FIXTURE wins over both, for a copy unpacked by
# hand or kept outside the build tree.
#
# The result is SEABASS_REAL_ANALYSIS_FIXTURE_DIR: the unpacked tree, or
# empty when there is none. The tests that need it get it through their
# environment, and are registered only when there is one.

option(SEABASS_FETCH_FIXTURES "Download the tier 2 real-analysis fixture at configure time" OFF)
set(SEABASS_FIXTURE_BASE_URL "https://vizzzion.org/seabass/downloads/fixtures"
    CACHE STRING "Where SEABASS_FETCH_FIXTURES downloads fixture zips from")
set(SEABASS_REAL_ANALYSIS_FIXTURE ""
    CACHE PATH "An unpacked real-analysis fixture to test against; wins over SEABASS_FETCH_FIXTURES")

# The one fixture there is so far, and the SHA-256 of its zip. A new
# fixture is a new name: a published zip is never replaced, so a pinned
# hash keeps meaning the same bytes.
set(SEABASS_REAL_ANALYSIS_FIXTURE_NAME "real-analysis-2026-11-23")
set(SEABASS_REAL_ANALYSIS_FIXTURE_SHA256 "25110f974681b3d3332d5e603c8cf7f73be4a2b6044fc5dab9ffe15cbe6dba9c")

set(SEABASS_REAL_ANALYSIS_FIXTURE_DIR "")
if(SEABASS_REAL_ANALYSIS_FIXTURE)
    # Not checked here: the test checks the tree against its MANIFEST.json
    # and fails on a directory that does not hold one, which is the louder
    # place to find out.
    get_filename_component(SEABASS_REAL_ANALYSIS_FIXTURE_DIR "${SEABASS_REAL_ANALYSIS_FIXTURE}" ABSOLUTE)
    message(STATUS "Real-analysis fixture: ${SEABASS_REAL_ANALYSIS_FIXTURE_DIR} (SEABASS_REAL_ANALYSIS_FIXTURE)")
elseif(SEABASS_FETCH_FIXTURES)
    set(_seabass_fixture_dir "${CMAKE_BINARY_DIR}/fixtures")
    set(_seabass_fixture_zip "${_seabass_fixture_dir}/${SEABASS_REAL_ANALYSIS_FIXTURE_NAME}.zip")
    set(_seabass_fixture_tree "${_seabass_fixture_dir}/${SEABASS_REAL_ANALYSIS_FIXTURE_NAME}")
    # Beside the tree, not in it: the test fails on any file the manifest
    # does not list.
    set(_seabass_fixture_stamp "${_seabass_fixture_dir}/${SEABASS_REAL_ANALYSIS_FIXTURE_NAME}.unpacked")

    # Unpacked from this zip before, and its manifest still the one that
    # came out of it: nothing to do. The stamp records both hashes, so a
    # new pin, or a tree someone edited, unpacks again.
    set(_seabass_fixture_current FALSE)
    if(EXISTS "${_seabass_fixture_stamp}" AND EXISTS "${_seabass_fixture_tree}/MANIFEST.json")
        file(READ "${_seabass_fixture_stamp}" _seabass_fixture_stamped)
        file(SHA256 "${_seabass_fixture_tree}/MANIFEST.json" _seabass_fixture_manifest_hash)
        if(_seabass_fixture_stamped STREQUAL
           "${SEABASS_REAL_ANALYSIS_FIXTURE_SHA256} ${_seabass_fixture_manifest_hash}")
            set(_seabass_fixture_current TRUE)
        endif()
    endif()

    if(NOT _seabass_fixture_current)
        set(_seabass_fixture_have_zip FALSE)
        if(EXISTS "${_seabass_fixture_zip}")
            file(SHA256 "${_seabass_fixture_zip}" _seabass_fixture_zip_hash)
            if(_seabass_fixture_zip_hash STREQUAL SEABASS_REAL_ANALYSIS_FIXTURE_SHA256)
                set(_seabass_fixture_have_zip TRUE)
            endif()
        endif()
        if(NOT _seabass_fixture_have_zip)
            set(_seabass_fixture_url "${SEABASS_FIXTURE_BASE_URL}/${SEABASS_REAL_ANALYSIS_FIXTURE_NAME}.zip")
            message(STATUS "Downloading ${_seabass_fixture_url}")
            file(MAKE_DIRECTORY "${_seabass_fixture_dir}")
            # Into a side name first, so an interrupted or refused download
            # never sits where a good zip is looked for.
            file(REMOVE "${_seabass_fixture_zip}.part")
            file(DOWNLOAD "${_seabass_fixture_url}" "${_seabass_fixture_zip}.part"
                EXPECTED_HASH SHA256=${SEABASS_REAL_ANALYSIS_FIXTURE_SHA256}
                STATUS _seabass_fixture_status
                TLS_VERIFY ON
                INACTIVITY_TIMEOUT 60)
            list(GET _seabass_fixture_status 0 _seabass_fixture_code)
            list(GET _seabass_fixture_status 1 _seabass_fixture_message)
            if(NOT _seabass_fixture_code EQUAL 0)
                file(REMOVE "${_seabass_fixture_zip}.part")
                message(FATAL_ERROR "SEABASS_FETCH_FIXTURES: ${_seabass_fixture_url} could not be downloaded "
                    "as the pinned file: "
                    "${_seabass_fixture_message}. Configure without -DSEABASS_FETCH_FIXTURES=ON to build "
                    "without the fixture, or point -DSEABASS_REAL_ANALYSIS_FIXTURE=<dir> at an unpacked copy.")
            endif()
            # Again by hand: a mismatch under EXPECTED_HASH is an error that
            # lets configure carry on, and what follows would unpack the
            # bytes that did not match. This stops it, and removes them.
            file(SHA256 "${_seabass_fixture_zip}.part" _seabass_fixture_zip_hash)
            if(NOT _seabass_fixture_zip_hash STREQUAL SEABASS_REAL_ANALYSIS_FIXTURE_SHA256)
                file(REMOVE "${_seabass_fixture_zip}.part")
                message(FATAL_ERROR "SEABASS_FETCH_FIXTURES: ${_seabass_fixture_url} has SHA-256 "
                    "${_seabass_fixture_zip_hash}, but cmake/SeabassFixtures.cmake pins "
                    "${SEABASS_REAL_ANALYSIS_FIXTURE_SHA256}. Not using it.")
            endif()
            file(RENAME "${_seabass_fixture_zip}.part" "${_seabass_fixture_zip}")
        endif()

        file(REMOVE_RECURSE "${_seabass_fixture_tree}")
        file(REMOVE "${_seabass_fixture_stamp}")
        if(CMAKE_VERSION VERSION_LESS 3.18)
            execute_process(COMMAND "${CMAKE_COMMAND}" -E tar xf "${_seabass_fixture_zip}"
                WORKING_DIRECTORY "${_seabass_fixture_dir}"
                RESULT_VARIABLE _seabass_fixture_untar)
            if(NOT _seabass_fixture_untar EQUAL 0)
                message(FATAL_ERROR "SEABASS_FETCH_FIXTURES: could not unpack ${_seabass_fixture_zip}")
            endif()
        else()
            file(ARCHIVE_EXTRACT INPUT "${_seabass_fixture_zip}" DESTINATION "${_seabass_fixture_dir}")
        endif()
        if(NOT EXISTS "${_seabass_fixture_tree}/MANIFEST.json")
            message(FATAL_ERROR "SEABASS_FETCH_FIXTURES: ${_seabass_fixture_zip} did not unpack to "
                "${_seabass_fixture_tree}/MANIFEST.json")
        endif()
        file(SHA256 "${_seabass_fixture_tree}/MANIFEST.json" _seabass_fixture_manifest_hash)
        file(WRITE "${_seabass_fixture_stamp}"
            "${SEABASS_REAL_ANALYSIS_FIXTURE_SHA256} ${_seabass_fixture_manifest_hash}")
    endif()
    set(SEABASS_REAL_ANALYSIS_FIXTURE_DIR "${_seabass_fixture_tree}")
    message(STATUS "Real-analysis fixture: ${SEABASS_REAL_ANALYSIS_FIXTURE_DIR}")
else()
    message(STATUS "Real-analysis fixture: none (-DSEABASS_FETCH_FIXTURES=ON or "
        "-DSEABASS_REAL_ANALYSIS_FIXTURE=<dir> to test against one)")
endif()
