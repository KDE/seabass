# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: BSD-2-Clause

cmake_minimum_required(VERSION 3.16)
# C, not just CXX - libdjinterop's bundled SQLite/date fallback (used
# when cross-compiling, see the WIN32 branch below) is plain C
# (sqlite3.c). Declaring it here at the top level, rather than relying on
# a subdirectory's own project() call to add it implicitly, avoids a real
# cross-compile gotcha hit while adding Windows support: enabling C only
# from within third_party/libdjinterop's own project() call left
# CMAKE_C_COMPILE_OBJECT unset for the Unix Makefiles generator when
# cross-compiling, breaking the Generate step.
# One version for the whole project, and the only place it is written
# down. A release tag (releases/<channel>/X.Y.Z) has to agree with it --
# tools/release.sh refuses to tag a tree that says something else -- so
# the tag stays a statement about this file rather than a second opinion.
project(seabass VERSION 0.7.9 LANGUAGES CXX C)

# Which shelf this build came off. Set by CI from the release tag; a
# build from a working tree is "dev", and says so in Settings and in the
# version check, because it is not any published release.
set(SEABASS_RELEASE_CHANNEL "dev" CACHE STRING "alpha, beta, stable or dev")
set_property(CACHE SEABASS_RELEASE_CHANNEL PROPERTY STRINGS dev alpha beta stable)

# The commit, for a bug report to be actionable. Not fatal when git is
# absent (a source tarball, a Craft build from an export): the version
# above is what identifies the release, this only sharpens it.
set(SEABASS_GIT_DESCRIBE "")
find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" describe --always --dirty --abbrev=8
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE SEABASS_GIT_DESCRIBE
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
endif()

configure_file("${CMAKE_SOURCE_DIR}/src/seabass_version.hpp.in"
               "${CMAKE_BINARY_DIR}/generated/seabass_version.hpp" @ONLY)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    # MSVC leaves the __cplusplus macro at 199711L regardless of
    # /std:c++<N> unless told otherwise -- a legacy-compatibility default,
    # not a bug -- so every __cplusplus version check anywhere (this
    # project's own code never does one, but vendored third_party/
    # dependencies do, e.g. libdjinterop's config.hpp) sees a C++98
    # compiler even while genuinely compiling as C++23. GCC and Clang
    # report the real value unconditionally, so this has no MINGW
    # counterpart.
    add_compile_options(/Zc:__cplusplus)
    # MSVC's default source-file encoding is the system's own ANSI code
    # page, not UTF-8 (GCC/Clang assume UTF-8 unconditionally, which is
    # why this has no MINGW counterpart either). Every source file in this
    # project is UTF-8, so a non-ASCII byte in a string literal -- an
    # accented name in an SPDX header is a comment and harmless, but
    # path_key_test.cpp's own café/decomposed-form literals are not --
    # gets silently re-decoded through the wrong code page and mangled
    # into a different string before the program ever runs. Confirmed
    # directly: path_key_test failed comparing "café.mp3" against its own
    # decomposed spelling because the *composed* one had already been
    # corrupted into a replacement character at compile time, on a target
    # that (unlike the Qt-linked ones, which get /utf-8 for free from Qt's
    # own CMake integration) had nothing setting the source charset at all.
    add_compile_options(/utf-8)
endif()

# --- vendored dependencies (git submodules under third_party/) ---
#
# .gitmodules itself is intentionally not a tracked file in this repo
# (see .gitignore and scripts/init-submodules.sh for why: KDE Invent,
# our canonical host, rejects any pushed commit containing a file
# literally named .gitmodules). The real submodule config lives in
# cmake/dependency-submodules.txt instead; this step regenerates the
# real .gitmodules from it and initializes the submodules below on
# every configure, so a fresh checkout just works with plain `cmake`.
find_package(Git QUIET)
if(GIT_EXECUTABLE)
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E copy
            ${CMAKE_SOURCE_DIR}/cmake/dependency-submodules.txt
            ${CMAKE_SOURCE_DIR}/.gitmodules
    )
    execute_process(
        COMMAND ${GIT_EXECUTABLE} submodule sync --recursive
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    execute_process(
        COMMAND ${GIT_EXECUTABLE} submodule update --init --recursive
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        RESULT_VARIABLE _seabass_submodule_result
    )
    if(NOT _seabass_submodule_result EQUAL 0)
        message(FATAL_ERROR "Failed to initialize git submodules -- see scripts/init-submodules.sh")
    endif()
else()
    message(WARNING "git not found -- skipping automatic submodule init; run scripts/init-submodules.sh manually, or ensure third_party/libdjinterop and third_party/kaitai_struct_cpp_stl_runtime are already populated")
endif()

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_DOC OFF CACHE BOOL "" FORCE)

# libdjinterop ships its own unit tests. We build that library from a
# pinned checkout of its source (third_party/libdjinterop, a submodule)
# rather than linking a system package, and every Engine write in Seabass
# goes through it -- so its suite is coverage of code we ship, and it runs
# with ours. Measured at the pinned 17ea4f70: 14/14.
#
# That is what used to happen, and it was the worst of both: upstream
# gates the test targets on Boost_FOUND alone, never on BUILD_TESTING, so
# with BUILD_TESTING off they were still compiled into `all` while no
# CTestTestfile.cmake was generated for them. Built, so a Boost that is
# headers-only broke `cmake --build` (and on a clean tree took 14 of our
# own tests down with it, unbuilt and reported Not Run); unregistered, so
# they could never pass or fail. See sebasje/seabass#9.
#
# CMAKE_DISABLE_FIND_PACKAGE_Boost is what actually stops them being
# built: upstream's `if (Boost_FOUND)` is the only gate on those targets.
option(SEABASS_LIBDJINTEROP_TESTS "Also build and run libdjinterop's own test suite (needs Boost.Filesystem)" ON)
if(SEABASS_LIBDJINTEROP_TESTS)
    find_package(Boost 1.71.0 QUIET COMPONENTS filesystem system)
    # Boost_FOUND alone is true for a headers-only install, which is the
    # trap upstream falls into; check the components we actually link.
    if(NOT Boost_filesystem_FOUND OR NOT Boost_system_FOUND)
        message(FATAL_ERROR
            "libdjinterop's test suite needs Boost.Filesystem, and only the Boost "
            "headers were found.\n"
            "  Debian/Ubuntu: apt install libboost-filesystem-dev\n"
            "Configure with -DSEABASS_LIBDJINTEROP_TESTS=OFF to build without "
            "libdjinterop's own tests.")
    endif()
    set(BUILD_TESTING ON CACHE BOOL "" FORCE)
else()
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(CMAKE_DISABLE_FIND_PACKAGE_Boost ON)
endif()
if(MSVC)
    # DjInterop keeps its private bundled amalgamation under MSVC, for the
    # vcpkg dllimport reason explained at SQLite::SQLite3 below. Everywhere
    # else, MSYS2 included, it uses the same system SQLite3 as the rest of
    # the build: a second copy of every sqlite3_* symbol in one executable
    # makes the link order-dependent, and some targets then fail with
    # "multiple definition of `sqlite3_finalize'".
    set(SYSTEM_SQLITE OFF CACHE BOOL "" FORCE)
endif()
add_subdirectory(third_party/libdjinterop)

# System SQLite3, already required to build libdjinterop (Engine's
# library format is itself SQLite), so LocalCueStore reuses it rather
# than vendoring a JSON library for the same job. seabass_core itself
# doesn't use this on MSVC (it compiles libdjinterop's bundled
# amalgamation directly instead, see the MSVC branch below). This
# find_package call runs so local_cue_store_test (which links
# SQLite::SQLite3 directly, see tests/ below) has something to link
# against on Windows too. MSYS2's UCRT64 environment ships a real
# sqlite3 package with proper find_package support, unlike the old
# Ubuntu-hosted mingw cross-compile setup this project used before.
#
# MSVC does not take this path at all. vcpkg's sqlite3 port builds a
# DLL by default, so its headers declare every sqlite3_* symbol
# __declspec(dllimport) -- and vcpkg's toolchain file adds its
# installed/include directory to every target's search path globally,
# ahead of libdjinterop's own PRIVATE include of its bundled amalgamation
# for angle-bracket includes. libdjinterop's own .cpp files (via
# sqlite_modern_cpp.h) see vcpkg's dllimport declarations while the
# amalgamation's sqlite3.c -- compiled straight into DjInterop's own
# target because SYSTEM_SQLITE is OFF above -- defines those same
# symbols as plain (non-dllimport) exports, so the two disagree and
# djinterop.lib's own internal calls fail to link with "unresolved
# external symbol __imp_sqlite3_*" in any executable that links it.
# Compiling the same amalgamation source again here, into a target of
# our own aliased as SQLite::SQLite3, sidesteps vcpkg's sqlite3 (and
# its conflicting header) for every consumer without touching the
# pinned third_party/libdjinterop submodule.
if(MSVC)
    add_library(seabass_sqlite3_amalgamation STATIC
        third_party/libdjinterop/ext/sqlite-amalgamation/sqlite3.c)
    target_include_directories(seabass_sqlite3_amalgamation SYSTEM PUBLIC
        third_party/libdjinterop/ext/sqlite-amalgamation)
    target_compile_definitions(seabass_sqlite3_amalgamation PUBLIC SQLITE_OMIT_LOAD_EXTENSION)
    add_library(SQLite::SQLite3 ALIAS seabass_sqlite3_amalgamation)
else()
    find_package(SQLite3 REQUIRED)
endif()

# A test that does not run is a failure, so a missing test dependency
# stops the configure rather than quietly subtracting a target or turning
# a test into a skip. SEABASS_TESTS=OFF is the one way to build without
# the suite, and it is recorded in CMakeCache.txt -- the exception is
# always somebody's stated choice, never an accident of what happened to
# be installed on the machine.
option(SEABASS_GUI "Build the Qt GUI (requires Qt6)" ON)
option(SEABASS_TESTS "Build and run the test suite" ON)

# System zlib, used to compress local cue-backup snapshots and full
# metadata backups before they hit disk (see infrastructure/compression/).
find_package(ZLIB REQUIRED)

# Independent ZIP implementations for the stick-backup cross-validation
# test (tests/backup_archive_crossvalidation_test.cpp). Required, not
# optional: that test is the only thing stopping a writer and a reader
# with the same author agreeing with each other and both disagreeing with
# the ZIP specification, and it can only do that against implementations
# that share no code with ours. Absent, it used to exit 77 and count as a
# pass -- a green line that had validated nothing.
find_package(Python3 COMPONENTS Interpreter QUIET)
find_program(UNZIP_EXECUTABLE unzip)
find_program(SEVENZIP_EXECUTABLE NAMES 7z 7zz 7za)
if(SEABASS_TESTS)
    set(_seabass_missing_zip_tools "")
    if(NOT Python3_Interpreter_FOUND)
        list(APPEND _seabass_missing_zip_tools "python3 (Debian/Ubuntu: python3)")
    endif()
    if(NOT UNZIP_EXECUTABLE)
        list(APPEND _seabass_missing_zip_tools "unzip (Debian/Ubuntu: unzip)")
    endif()
    if(NOT SEVENZIP_EXECUTABLE)
        list(APPEND _seabass_missing_zip_tools "7z (Debian/Ubuntu: p7zip-full)")
    endif()
    if(_seabass_missing_zip_tools)
        list(JOIN _seabass_missing_zip_tools "\n    " _seabass_missing_zip_tools_text)
        message(FATAL_ERROR
            "backup_archive_crossvalidation_test needs independent ZIP "
            "implementations to check our archives against, and these are "
            "missing:\n    ${_seabass_missing_zip_tools_text}\n"
            "To build without any tests at all, configure with -DSEABASS_TESTS=OFF.")
    endif()
endif()

# Only needed by stick_write_lock_test, which spawns a real std::thread to
# prove StickWriteLock actually excludes a concurrent locker.
find_package(Threads REQUIRED)

# TagLib: reads artist/title/duration/bitrate straight out of an audio
# file, for files no catalog references and for catalog rows that left
# fields empty. Required: every package build ships it (Craft's
# libs/taglib, MSYS2, Linux), and a build without it tests an app nobody
# ships -- every caller quietly falls back to NullTrackMetadataProbe,
# and code only a TagLib-less build compiles goes unbuilt everywhere.
#
# find_package(TagLib) only works with TagLib 2.x, which ships a CMake
# config; 1.x (what Debian/Ubuntu's libtag1-dev and MSYS2's
# mingw-w64-ucrt-x86_64-taglib still provide) ships only taglib.pc. The
# CMake config is tried first: TagLib 2.x's own taglib.pc hardcodes
# "-lz" in its Libs line regardless of platform, which pkg-config turns
# into a bare "z" library search -- fine on MSYS2/Linux where a libz.a
# or -lz-compatible zlib exists, but a link error on MSVC/Craft, whose
# zlib import library is named zlib.lib. find_package(TagLib) uses the
# CMake-exported TagLib::tag target instead, which has no such bug.
find_package(TagLib QUIET)
if(NOT TARGET TagLib::tag)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(TAGLIB QUIET IMPORTED_TARGET taglib)
    endif()
endif()
if(NOT TARGET TagLib::tag AND NOT TAGLIB_FOUND)
    message(FATAL_ERROR
        "TagLib not found (neither TagLib 2.x's CMake config nor taglib.pc).\n"
        "  Debian/Ubuntu: apt install libtag1-dev pkg-config\n"
        "  Fedora:        dnf install taglib-devel\n"
        "  MSYS2/UCRT64:  pacman -S mingw-w64-ucrt-x86_64-taglib\n"
        "  vcpkg (MSVC):  vcpkg install taglib:x64-windows\n"
        "  macOS:         brew install taglib")
endif()

add_library(kaitai_cpp_stl_runtime STATIC
    third_party/kaitai_struct_cpp_stl_runtime/kaitai/kaitaistream.cpp
)
target_include_directories(kaitai_cpp_stl_runtime PUBLIC
    third_party/kaitai_struct_cpp_stl_runtime
)
target_compile_definitions(kaitai_cpp_stl_runtime PUBLIC KS_STR_ENCODING_ICONV)
if(WIN32)
    # Neither mingw-w64 nor MSVC ship <endian.h> or <byteswap.h>
    # (Linux/BSD-only) -- see third_party/mingw-compat/endian.h for the
    # full explanation (the name predates MSVC support; the shim itself
    # is not mingw-specific). Only this one target needs it (the only
    # place that source file's platform-detection preamble is compiled),
    # and PRIVATE so it doesn't leak onto anything else's include path.
    target_include_directories(kaitai_cpp_stl_runtime PRIVATE third_party/mingw-compat)
endif()
if(MINGW)
    # win-iconv-mingw-w64-dev's iconv() declares its second parameter
    # `const char **` by default (POSIX-2008 style); kaitaistream.cpp
    # (written against glibc's non-const `char **`) passes a plain
    # `char**`, which C++ refuses to implicitly convert. This empties
    # win-iconv's own WINICONV_CONST macro (its documented escape hatch
    # for exactly this) to match glibc's signature instead.
    target_compile_definitions(kaitai_cpp_stl_runtime PRIVATE WINICONV_CONST=)
    # win-iconv-mingw-w64-dev ships iconv.h but (unlike glibc) needs
    # explicitly linking - PUBLIC so it also reaches whatever links this
    # static library's .a (seabass-cli.exe's final link), not just this
    # target's own compilation. Bare "iconv" resolves to libiconv.dll.a
    # (the *import* library for iconv.dll) by mingw ld's default
    # preference, silently making the exe depend on iconv.dll at runtime
    # instead of bundling it - find_library forces the real static
    # libiconv.a instead, for a self-contained portable exe.
    find_library(WIN_ICONV_STATIC_LIB NAMES libiconv.a PATHS /usr/x86_64-w64-mingw32/lib REQUIRED)
    target_link_libraries(kaitai_cpp_stl_runtime PUBLIC ${WIN_ICONV_STATIC_LIB})
elseif(APPLE)
    # macOS ships iconv as its own libiconv rather than inside libc as
    # glibc does, so it has to be linked explicitly.
    find_package(Iconv REQUIRED)
    target_link_libraries(kaitai_cpp_stl_runtime PUBLIC Iconv::Iconv)
elseif(MSVC)
    # MSVC has no iconv() at all (no POSIX C runtime layer providing
    # one), unlike mingw-w64's win-iconv above. libiconv itself (e.g.
    # vcpkg's `libiconv` port) is the direct substitute and needs no
    # source change -- kaitaistream.cpp's `char**` call matches libiconv's
    # own (non-const) signature already, so WINICONV_CONST is a
    # win-iconv-specific escape hatch this branch never needs.
    find_package(Iconv REQUIRED)
    target_link_libraries(kaitai_cpp_stl_runtime PUBLIC Iconv::Iconv)
endif()

# --- rekordbox format parser, generated from specs/*.ksy (see specs/README.md) ---

add_library(rekordbox_format STATIC
    src/infrastructure/rekordbox/generated/rekordbox_pdb.cpp
    src/infrastructure/rekordbox/generated/rekordbox_anlz.cpp
)
target_include_directories(rekordbox_format PUBLIC
    src/infrastructure/rekordbox/generated
)
target_link_libraries(rekordbox_format PUBLIC kaitai_cpp_stl_runtime)

# --- seabass_core: domain -> application -> infrastructure, shared by ---
# --- both the CLI (src/cli/) and the Qt GUI (src/gui/) composition roots ---

# Standalone drive-measurement library, no Qt and nothing else from this
# tree: src/storageprobe/README.md says how to lift it into another
# project. seabass_core links it; the DJ workload lives in domain/.
add_library(storageprobe STATIC
    src/storageprobe/read_probe.cpp
    src/storageprobe/surface_check.cpp
    src/storageprobe/write_probe.cpp
    src/storageprobe/workload.cpp
    src/storageprobe/walk_tree.cpp
)
target_include_directories(storageprobe PUBLIC src)
target_compile_features(storageprobe PUBLIC cxx_std_17)

add_library(seabass_core STATIC
    src/domain/audio_levels.cpp
    src/domain/beat_grid.cpp
    src/domain/camelot_key.cpp
    src/domain/cleanup_leftovers.cpp
    src/domain/cross_source_sync_conflict.cpp
    src/domain/disk_usage.cpp
    src/domain/duplicate_cleanup.cpp
    src/domain/library_fingerprint.cpp
    src/application/catalog_digest.cpp
    src/application/use_cases/advise_stick_backup.cpp
    src/domain/duplicate_cue_consolidation.cpp
    src/domain/filesystem_compatibility.cpp
    src/domain/fuzzy_matcher.cpp
    src/domain/clustered_cue.cpp
    src/domain/junk_cue.cpp
    src/domain/matching_policy.cpp
    src/domain/rekordbox_xml.cpp
    src/domain/library_consistency.cpp
    src/domain/library_statistics.cpp
    src/domain/stick_performance.cpp
    src/domain/local_restore.cpp
    src/domain/metadata_backup_plan.cpp
    src/domain/metadata_merge.cpp
    src/domain/metadata_restore.cpp
    src/domain/sync_planning.cpp
    src/domain/track_matching.cpp
    src/domain/track_queries.cpp
    src/domain/track_scope.cpp
    src/domain/usb_filesystem.cpp
    src/application/ports/progress_reporter.cpp
    src/application/use_cases/anonymize_library.cpp
    src/infrastructure/benchmark/stick_performance_probe.cpp
    src/infrastructure/benchmark/stick_surface_check.cpp
    src/infrastructure/benchmark/stick_write_probe.cpp
    src/infrastructure/local/stick_performance_history.cpp
    src/infrastructure/bulk_write_strategy.cpp
    src/infrastructure/durable_file_write.cpp
    src/infrastructure/rekordbox/anlz_byte_source.cpp
    src/infrastructure/rekordbox/anlz_source_for_root.cpp
    src/infrastructure/local/browsed_backup_root.cpp
    src/application/use_cases/open_stick_backup.cpp
    src/application/use_cases/manage_stick_backups.cpp
    src/infrastructure/stick_backup/archive_anlz_source.cpp
    src/infrastructure/rekordbox/anlz_cue_codec.cpp
    src/infrastructure/rekordbox/anlz_legacy_cue_codec.cpp
    src/infrastructure/rekordbox/anlz_file.cpp
    src/infrastructure/rekordbox/pdb_lookup.cpp
    src/infrastructure/rekordbox/anlz_path_index.cpp
    src/infrastructure/rekordbox/pdb_row_writer.cpp
    src/infrastructure/rekordbox/rekordbox_cleanup_writer.cpp
    src/infrastructure/rekordbox/rekordbox_cue_writer.cpp
    src/infrastructure/rekordbox/rekordbox_xml_writer.cpp
    src/infrastructure/rekordbox/kaitai_rekordbox_reader.cpp
    src/infrastructure/rekordbox/rekordbox_beat_grid_reader.cpp
    src/infrastructure/rekordbox/rekordbox_waveform_reader.cpp
    src/infrastructure/rekordbox/rekordbox_settings_fields.cpp
    src/infrastructure/rekordbox/rekordbox_settings_reader.cpp
    src/infrastructure/rekordbox/rekordbox_settings_writer.cpp
    src/infrastructure/rekordbox/rekordbox_library_anonymizer.cpp
    src/infrastructure/engine/libdjinterop_engine_reader.cpp
    src/infrastructure/engine/libdjinterop_engine_library_creator.cpp
    src/infrastructure/engine/engine_artwork.cpp
    src/infrastructure/engine/engine_sample_rates.cpp
    src/infrastructure/engine/engine_analysis_state.cpp
    src/infrastructure/engine/engine_import_state.cpp
    src/infrastructure/engine/rekordbox_key_parser.cpp
    src/infrastructure/engine/libdjinterop_beat_grid_reader.cpp
    src/infrastructure/engine/libdjinterop_waveform_reader.cpp
    src/infrastructure/engine/libdjinterop_engine_cue_writer.cpp
    src/infrastructure/engine/libdjinterop_engine_cleanup_writer.cpp
    src/infrastructure/engine/libdjinterop_engine_anonymizer.cpp
    src/infrastructure/media/media_factory.cpp
    src/infrastructure/media/stick_root_scan.cpp
    src/infrastructure/backup/filesystem_backup_store.cpp
    src/infrastructure/backup/stick_space.cpp
    src/infrastructure/backup/stick_write_lock.cpp
    src/infrastructure/cleanup/pending_deletion_manifest.cpp
    src/infrastructure/cleanup/pending_deletion_resolver.cpp
    src/infrastructure/cleanup/stick_containment.cpp
    src/infrastructure/cleanup/pending_deletion_applier.cpp
    src/infrastructure/compression/zlib_compressor.cpp
    src/infrastructure/logging/file_operation_log.cpp
    src/infrastructure/local/local_cue_store.cpp
    src/infrastructure/local/app_data_directory.cpp
    src/infrastructure/paths/seabass_paths.cpp
    src/infrastructure/work_counters.cpp
    src/infrastructure/anonymization_byte_sweep.cpp
    src/infrastructure/anonymization_verifier.cpp
    src/infrastructure/zip_archive_reader.cpp
    src/infrastructure/onelibrary/onelibrary_anonymizer.cpp
    src/infrastructure/local/file_library_edit_lock_store.cpp
    src/infrastructure/system/rekordbox_process_detector.cpp
    src/infrastructure/system/process_liveness.cpp
    src/infrastructure/system/stick_hardware_info.cpp
    src/infrastructure/zip_archive_writer.cpp
    src/infrastructure/hashing/sha256.cpp
    src/infrastructure/stick_backup/posix_archive_file.cpp
    src/infrastructure/stick_backup/zip_format.cpp
    src/infrastructure/stick_backup/zip64_writer.cpp
    src/infrastructure/stick_backup/zip64_reader.cpp
    src/infrastructure/stick_backup/backup_manifest.cpp
    src/infrastructure/stick_backup/backup_artwork_lookup.cpp
    src/infrastructure/stick_backup/archive_journal.cpp
    src/infrastructure/stick_backup/archive_updater.cpp
    src/infrastructure/stick_backup/archive_recovery.cpp
    src/infrastructure/long_paths.cpp
    src/infrastructure/fs_remove.cpp
    src/infrastructure/stick_backup/stick_tree_walker.cpp
    src/infrastructure/stick_backup/stat_diff.cpp
    src/infrastructure/stick_backup/sqlite_db_set.cpp
    src/infrastructure/stick_backup/library_catalog_mtime.cpp
    src/infrastructure/stick_backup/archive_compactor.cpp
    src/infrastructure/stick_backup/restore_path_sanitizer.cpp
    src/infrastructure/engine/engine_restore_check.cpp
    src/application/use_cases/backup_stick.cpp
    src/application/use_cases/compact_stick_backup.cpp
    src/application/use_cases/restore_stick_backup.cpp
    src/application/use_cases/clone_stick.cpp
    src/application/use_cases/fill_missing_durations.cpp
    src/application/path_key.cpp
    src/application/use_cases/collapse_catalog_rows.cpp
    src/application/use_cases/export_rekordbox_xml.cpp
    src/application/use_cases/real_file_sizes.cpp
    src/application/use_cases/find_unreferenced_files.cpp
    src/infrastructure/stick_layout.cpp
    src/application/use_cases/unreferenced_tracks.cpp
    src/infrastructure/cleanup/audio_file_walk.cpp
    src/infrastructure/local/duration_cache.cpp
    src/infrastructure/local/silence_cache.cpp
    src/infrastructure/local/metadata_cache.cpp
    src/infrastructure/local/metadata_store.cpp
)
# generated/ alongside src/: seabass_version.hpp is written there at
# configure time, and everything that links the core can ask what it is.
target_include_directories(seabass_core PUBLIC src "${CMAKE_BINARY_DIR}/generated")
target_link_libraries(seabass_core PUBLIC rekordbox_format DjInterop ZLIB::ZLIB storageprobe)

# The production sources the tests kept re-compiling, compiled once.
#
# Measured first, from a full Ninja build's own .ninja_log: 532 s of
# 2340 s of compile CPU went on re-compiling src/ files another test
# target had already compiled -- 466 redundant objects, 22.7% of compile
# time and 20.5% of all build work. Worst offenders zip64_reader.cpp (18
# targets), durable_file_write.cpp (23), work_counters.cpp (31). Linking
# is 11% of the work, so this is a compile-side fix, not a linker one.
#
# Why not just link seabass_core: it is built for the application and
# keeps NDEBUG. Tests need assert() live, hence -UNDEBUG, and that is
# precisely why they were listing these sources by hand.
#
# Only sources seabass_core ALREADY builds are in here, so this target
# needs no include dirs or dependencies beyond the ones mirrored below.
# Three others are duplicated across tests and deliberately left alone:
# src/gui/media_controller.cpp needs Qt, and the onelibrary readers need
# SQLCipher. Putting those here would hand Qt and SQLCipher to every
# Qt-free test, and staying Qt-free is a property this project keeps on
# purpose (see seabass_core's own comment and corpus_test).
#
# STATIC rather than OBJECT: an OBJECT library puts every object into
# every consumer, while a static archive lets the linker take only what
# a test references, so a small domain test does not grow to carry the
# zip writer.
#
# -UNDEBUG explicitly, because the loop near the end of this file that
# applies it automatically matches target NAMES ending in _test/_tests,
# and this is a library. Do not rely on a name to do it.
add_library(seabass_core_testing STATIC
    src/application/path_key.cpp
    src/application/ports/progress_reporter.cpp
    src/application/use_cases/backup_stick.cpp
    src/application/use_cases/compact_stick_backup.cpp
    src/application/use_cases/restore_stick_backup.cpp
    src/domain/duplicate_cue_consolidation.cpp
    src/domain/clustered_cue.cpp
    src/domain/junk_cue.cpp
    src/domain/local_restore.cpp
    src/domain/matching_policy.cpp
    src/domain/metadata_merge.cpp
    src/domain/stick_performance.cpp
    src/domain/sync_planning.cpp
    src/domain/track_matching.cpp
    src/domain/usb_filesystem.cpp
    src/infrastructure/backup/filesystem_backup_store.cpp
    src/infrastructure/backup/stick_write_lock.cpp
    src/infrastructure/benchmark/stick_performance_probe.cpp
    src/infrastructure/benchmark/stick_surface_check.cpp
    src/infrastructure/benchmark/stick_write_probe.cpp
    src/infrastructure/cleanup/pending_deletion_manifest.cpp
    src/infrastructure/cleanup/stick_containment.cpp
    src/infrastructure/durable_file_write.cpp
    src/infrastructure/engine/libdjinterop_engine_cleanup_writer.cpp
    src/infrastructure/engine/libdjinterop_engine_cue_writer.cpp
    src/infrastructure/engine/engine_import_state.cpp
    src/infrastructure/engine/libdjinterop_engine_library_creator.cpp
    src/infrastructure/engine/libdjinterop_engine_reader.cpp
    src/infrastructure/engine/rekordbox_key_parser.cpp
    src/infrastructure/fs_remove.cpp
    src/infrastructure/hashing/sha256.cpp
    src/infrastructure/local/app_data_directory.cpp
    src/infrastructure/local/browsed_backup_root.cpp
    src/infrastructure/long_paths.cpp
    src/infrastructure/paths/seabass_paths.cpp
    src/infrastructure/rekordbox/anlz_cue_codec.cpp
    src/infrastructure/rekordbox/anlz_file.cpp
    src/infrastructure/rekordbox/anlz_legacy_cue_codec.cpp
    src/infrastructure/rekordbox/anlz_path_index.cpp
    src/infrastructure/rekordbox/pdb_lookup.cpp
    src/infrastructure/rekordbox/pdb_row_writer.cpp
    src/infrastructure/stick_backup/archive_compactor.cpp
    src/infrastructure/stick_backup/backup_manifest.cpp
    src/infrastructure/stick_backup/posix_archive_file.cpp
    src/infrastructure/stick_backup/restore_path_sanitizer.cpp
    src/infrastructure/stick_backup/stick_tree_walker.cpp
    src/infrastructure/work_counters.cpp
)
target_include_directories(seabass_core_testing PUBLIC src)
target_link_libraries(seabass_core_testing PUBLIC rekordbox_format DjInterop ZLIB::ZLIB storageprobe)

# COMPILE_LANGUAGE:CXX-gated: this target also compiles sqlite3.c (below,
# MSVC only) as a plain C file, and both of these are C++-only. The
# forced include in particular does not merely not-apply to a C file --
# msvc_assert_no_dialog.hpp pulls in <yvals_core.h>, and cl.exe's C
# frontend refuses that STL header outright ("STL1003: Unexpected
# compiler, expected C++ compiler"), which looks nothing like what
# these two options are for and cost a rebuild to trace.
target_compile_options(seabass_core_testing PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:$<IF:$<CXX_COMPILER_ID:MSVC>,/UNDEBUG,-UNDEBUG>>"
    "$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<COMPILE_LANGUAGE:CXX>>:/FI${CMAKE_SOURCE_DIR}/tests/msvc_assert_no_dialog.hpp>")

if(MSVC)
    # See the SYSTEM_SQLITE note above - same bundled amalgamation
    # libdjinterop uses privately, compiled straight into seabass_core
    # instead of going through find_package(SQLite3). seabass_core_testing
    # compiles several of the same engine/.cpp files (its own comment
    # above promises its needs are "mirrored below"), so it needs the
    # same amalgamation or sqlite3.h is simply not on its include path --
    # confirmed directly: without this, seabass_core_testing fails to
    # build at all on Windows with "Cannot open include file: 'sqlite3.h'".
    target_sources(seabass_core PRIVATE
        third_party/libdjinterop/ext/sqlite-amalgamation/sqlite3.c)
    target_include_directories(seabass_core PRIVATE
        third_party/libdjinterop/ext/sqlite-amalgamation)
    target_compile_definitions(seabass_core PRIVATE SQLITE_OMIT_LOAD_EXTENSION)

    target_sources(seabass_core_testing PRIVATE
        third_party/libdjinterop/ext/sqlite-amalgamation/sqlite3.c)
    target_include_directories(seabass_core_testing PRIVATE
        third_party/libdjinterop/ext/sqlite-amalgamation)
    target_compile_definitions(seabass_core_testing PRIVATE SQLITE_OMIT_LOAD_EXTENSION)
else()
    # No copy of our own here, MinGW included: tests that #include
    # <sqlite3.h> also link SQLite::SQLite3, and a second copy collides
    # at link time (see SYSTEM_SQLITE above).
    target_link_libraries(seabass_core PUBLIC SQLite::SQLite3)
    target_link_libraries(seabass_core_testing PUBLIC SQLite::SQLite3)
endif()

# --- Read-only detection and filesystem repair: one file with a branch ---
# --- per platform (statvfs/udisks2, statfs/diskutil, GetVolumeInformation ---
# --- /Repair-Volume), so it builds everywhere rather than living in the ---
# --- platform lists below. ---

target_sources(seabass_core PRIVATE
    src/infrastructure/media/filesystem_health.cpp
)

# --- platform-specific removable-media adapters (see media_factory.cpp) ---
# --- -- the only part of seabass_core with no single cross-platform ---
# --- implementation, since "how do I see/mount/eject a USB stick" is ---
# --- genuinely OS-specific (udev+udisksctl on Linux, drive letters + ---
# --- DeviceIoControl on Windows, DiskArbitration+diskutil on macOS). ---

if(WIN32)
    target_sources(seabass_core PRIVATE
        src/infrastructure/media/windows_removable_media_locator.cpp
        src/infrastructure/media/windows_removable_media_monitor.cpp
        src/infrastructure/media/windows_removable_media_mounter.cpp
        src/infrastructure/media/windows_usb_formatter.cpp
        src/infrastructure/process/run_command_windows.cpp
    )
elseif(APPLE)
    target_sources(seabass_core PRIVATE
        src/infrastructure/media/diskutil_media_mounter.cpp
        src/infrastructure/media/macos_disk_arbitration_monitor.cpp
        src/infrastructure/media/macos_disk_description.cpp
        src/infrastructure/media/macos_removable_media_locator.cpp
        src/infrastructure/media/macos_usb_formatter.cpp
        src/infrastructure/process/run_command_posix.cpp
    )
    target_link_libraries(seabass_core PUBLIC
        "-framework CoreFoundation" "-framework DiskArbitration" "-framework IOKit")
else()
    target_sources(seabass_core PRIVATE
        src/infrastructure/media/linux_removable_media_locator.cpp
        src/infrastructure/media/linux_udev_media_monitor.cpp
        src/infrastructure/media/udisksctl_media_mounter.cpp
        src/infrastructure/media/linux_usb_formatter.cpp
        src/infrastructure/process/run_command_posix.cpp
    )
    target_link_libraries(seabass_core PUBLIC udev)
endif()

# --- OneLibrary/Device Library Plus (exportLibrary.db) cue writer -- ---
# --- Cross-platform: talks to libsqlcipher at runtime (LoadLibrary/ ---
# --- GetProcAddress on Windows, dlopen/dlsym elsewhere) rather than ---
# --- linking it, specifically to avoid a real symbol collision (see ---
# --- sqlcipher_dyn.hpp's own doc comment) with the plain, unencrypted ---
# --- SQLite3 seabass_core already links above. See ---
# --- docs/onelibrary-format.md. Needs libsqlcipher's runtime library ---
# --- present on the machine actually running seabass (Windows: the ---
# --- mingw-w64-ucrt-x86_64-sqlcipher package's DLL; Linux: the ---
# --- libsqlcipher1 package or equivalent) -- OneLibraryCueWriter's own ---
# --- callers already treat it as a best-effort secondary write target ---
# --- and degrade gracefully if it's missing. ---

target_sources(seabass_core PRIVATE
    src/infrastructure/onelibrary/onelibrary_key.cpp
    src/infrastructure/onelibrary/sqlcipher_dyn.cpp
    src/infrastructure/onelibrary/onelibrary_cue_writer.cpp
    src/infrastructure/onelibrary/onelibrary_reader.cpp
)
if(NOT WIN32)
    target_link_libraries(seabass_core PUBLIC ${CMAKE_DL_LIBS})
endif()

# --- seabass-cli: the terminal composition root ---

add_executable(seabass-cli
    src/cli/console.cpp
    src/cli/terminal_progress_reporter.cpp
    src/cli/main.cpp
    src/cli/damage_filesystem.cpp
)
target_include_directories(seabass-cli PRIVATE src)
target_link_libraries(seabass-cli PRIVATE seabass_core)
if(MINGW)
    # A portable single-file .exe - without these, mingw links
    # libgcc_s_seh-1.dll/libstdc++-6.dll dynamically, which aren't
    # present next to the exe (or anywhere in a real Windows install)
    # unless separately deployed. -static-libgcc/-static-libstdc++
    # aren't enough on their own with the posix-threading mingw
    # variant (required to match Qt's prebuilt win64_mingw binaries,
    # see cmake/mingw-w64-toolchain.cmake). That variant also pulls
    # in libwinpthread-1.dll dynamically unless -static covers it too.
    # MSVC has no equivalent of this branch: its runtime is the
    # near-universally-preinstalled VC++ Redistributable, not a
    # toolchain-specific DLL pair that needs statically absorbing.
    target_link_options(seabass-cli PRIVATE -static-libgcc -static-libstdc++ -static)
endif()
install(TARGETS seabass-cli RUNTIME DESTINATION bin)

# --- seabass: the Qt Quick composition root (opt-in: only built ---
# --- when Qt6 is available, so machines without Qt still get the CLI) ---

# Controls whether the experimental-features opt-in itself (the Settings
# toggle AppSettingsController::experimentalFeaturesEnabled) exists at
# all, not per-feature inclusion of code -- QML has no conditional-
# compilation mechanism, so a feature's own page/controller can't be
# compiled out this way. See docs/experimental-features.md. ON by
# default: a normal build still defaults every experimental feature to
# hidden until the user opts in at runtime; turning this OFF instead
# produces a real "stable-only build" where that opt-in can't be reached
# at all, regardless of anything already stored in QSettings.
option(SEABASS_EXPERIMENTAL "Compile in the experimental-features opt-in (Settings toggle)" ON)

# A test that does not run is a failure, so a missing test dependency
# stops the configure rather than quietly subtracting a target. These two
# options are the only way to build without the GUI or without the QML
# suite, and both are recorded in CMakeCache.txt -- the exception is
# always somebody's stated choice, never an accident of what happened to
# be installed on the machine.

# Test/QuickTest are OPTIONAL_COMPONENTS here for a discovery reason only:
# they're packaged separately from the core Qt6 modules on several distros
# (e.g. Debian/Ubuntu's libqt6quicktest6-dev vs libqt6qml6-dev), and
# find_package(Qt6 COMPONENTS ...) requires every listed component -- in
# the required list they would take Qt6_FOUND down with them and report
# "no Qt" on an install that is only missing the test package. Discovering
# them separately is what lets the check below name the package that is
# actually absent. It is not a licence to continue without them; that
# check is a FATAL_ERROR.
# QuickEffects/QuickShapes are OPTIONAL_COMPONENTS too, for a different
# reason than Test/QuickTest above: MSYS2's Qt6 packaging ships a public
# Qt6QuickEffects/Qt6QuickShapes CMake config, but the official Qt
# installer (and aqtinstall, which fetches the same kits) only ships
# their *Private counterparts -- confirmed by comparing the two
# installs' lib/cmake directories directly, not assumed. Every use below
# is itself guarded on `TARGET Qt6::QuickEffects`/`Qt6::QuickShapes`, so
# an MSVC build built from an official Qt kit just proceeds without
# them (losing MultiEffect/Shape QML types) rather than failing to
# configure at all.
# 6.8, said here rather than only in qt_standard_project_setup below.
# Without a version this call accepts whatever Qt a distribution has and
# the build finds out later, in the worst way: Ubuntu 24.04 ships 6.4.2,
# configure completed cleanly, and 1156 objects in, main.cpp failed with
# "QQmlApplicationEngine has no member named loadFromModule" -- a member
# that arrived in 6.5, in an error that never mentions a version. The
# floor is 6.8 because that is the oldest Qt this project is built and
# tested against: Windows runs 6.8.3, and the Linux CI image ships 6.10.
find_package(Qt6 6.8 QUIET COMPONENTS Core Gui Qml Quick QuickControls2 QuickLayouts QuickDialogs2 Multimedia Concurrent Svg Network
    OPTIONAL_COMPONENTS Test QuickTest QuickEffects QuickShapes ShaderTools)
# --- QtMultimedia duration probe (see TrackDurationProbe) ---
#
# Deliberately its own tiny target rather than sources in seabass_core:
# the core library, seabass-cli and the Qt-free corpus_test must keep
# linking no Qt at all. Only this target sees Qt6::Multimedia, and both
# seabass-cli and the GUI link it when Qt was found. Where it was not,
# callers fall back to application::NullTrackDurationProbe and the build
# is unchanged.
#
# No AUTOMOC: QtMultimediaDurationProbe has no Q_OBJECT of its own (it
# connects with a context object + lambda), so nothing needs moc'ing.
# TagLib-backed metadata probe. Same optional shape as seabass_audio_qt
# below, and independent of it: a build can have either, both or
# neither.
if(TAGLIB_FOUND OR TARGET TagLib::tag)
    add_library(seabass_taglib STATIC
        src/infrastructure/audio/taglib_metadata_probe.cpp
        src/infrastructure/audio/taglib_duration_probe.cpp
        src/infrastructure/audio/embedded_artwork.cpp)
    target_include_directories(seabass_taglib PUBLIC src)
    if(TARGET PkgConfig::TAGLIB)
        target_link_libraries(seabass_taglib PUBLIC PkgConfig::TAGLIB)
    else()
        target_link_libraries(seabass_taglib PUBLIC TagLib::tag)
    endif()
    target_compile_definitions(seabass_taglib PUBLIC SEABASS_HAVE_TAGLIB)
    # seabass-cli is declared above this point, so it is linked here
    # rather than at its own definition, same as seabass_audio_qt.
    target_link_libraries(seabass-cli PRIVATE seabass_taglib)
endif()

if(TARGET Qt6::Multimedia)
    add_library(seabass_audio_qt STATIC
        src/infrastructure/audio/qt_multimedia_duration_probe.cpp
        src/infrastructure/audio/qt_multimedia_silence_probe.cpp)
    target_include_directories(seabass_audio_qt PUBLIC src)
    target_link_libraries(seabass_audio_qt PUBLIC Qt6::Core Qt6::Multimedia)
    target_compile_definitions(seabass_audio_qt PUBLIC SEABASS_HAVE_QT_AUDIO)
    # seabass-cli is declared above this point, so it is linked here
    # rather than at its own definition.
    target_link_libraries(seabass-cli PRIVATE seabass_audio_qt)
endif()

# The real decoder against generated audio. Only where the probe itself
# was built; it refuses to answer without a QCoreApplication, which this
# test is the only place outside the app to create.
if(TARGET seabass_audio_qt)
    add_executable(silence_probe_test tests/silence_probe_test.cpp)
    target_include_directories(silence_probe_test PRIVATE src)
    target_link_libraries(silence_probe_test PRIVATE seabass_audio_qt seabass_core Qt6::Core)
    add_test(NAME silence_probe_test COMMAND silence_probe_test)
    # No display, no audio device: the probe decodes to buffers and
    # never opens an output.
    set_tests_properties(silence_probe_test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

if(SEABASS_GUI AND Qt6_FOUND)
    qt_standard_project_setup(REQUIRES 6.8)
    if(WIN32)
        # Needed to compile src/gui/win/app_icon.rc (the .exe's icon
        # resource), not enabled by the top-level project(seabass CXX
        # C) call above since only the Windows GUI build needs it.
        enable_language(RC)
    endif()
    # BUILD_SHARED_LIBS was forced OFF above for our own vendored static
    # libs; shadow it back to ON (non-cache, scoped to this subdirectory)
    # so Qt's QML tooling doesn't assume a static Qt build and go looking
    # for static QML plugin targets that don't exist in this (dynamic)
    # system Qt install.
    set(BUILD_SHARED_LIBS ON)

    # --- seabass_edit: the staged-edit machinery, Qt Core only ---
    #
    # One save's worth of shared state (SaveContext), the loop that
    # applies staged changes (runSaveLoop), the scratch-copy sessions
    # (FormatWriteSession), the catalog scan cache, and one class per
    # kind of staged edit under edit/changes/.
    #
    # Separate from the GUI target on purpose: a change class is the unit
    # a page stages and the save loop applies, so it is also the unit a
    # test has to be able to construct. While these lived as file-local
    # classes inside the controllers, five of the nine editing features
    # could not be driven from a test at all (docs/real-data-testing.md).
    # Qt Core is the whole Qt dependency -- no widgets, no QML, no event
    # loop -- so a plain C++ test links this and stages real work.
    # Keeping the system awake while a stick is written (gui/sleep_inhibitor)
    # asks logind over D-Bus on Linux. Windows uses a power request, which
    # needs nothing beyond kernel32.
    if(UNIX AND NOT APPLE)
        find_package(Qt6 REQUIRED COMPONENTS DBus)
    endif()

    add_library(seabass_edit STATIC
        src/gui/library_catalog_cache.hpp
        src/gui/library_catalog_cache.cpp
        src/gui/onelibrary_cue_writer_adapter.hpp
        src/gui/edit/pending_change.hpp
        src/gui/edit/save_context.hpp
        src/gui/edit/save_context.cpp
        src/gui/edit/save_loop.hpp
        src/gui/edit/save_loop.cpp
        src/gui/sleep_inhibitor.hpp
        src/gui/sleep_inhibitor.cpp
        src/gui/edit/format_write_session.hpp
        src/gui/edit/format_write_session.cpp
        src/gui/edit/changes/change_helpers.hpp
        src/gui/edit/changes/change_helpers.cpp
        src/gui/edit/changes/add_cue_change.hpp
        src/gui/edit/changes/add_cue_change.cpp
        src/gui/edit/changes/cleanup_group_change.hpp
        src/gui/edit/changes/cleanup_group_change.cpp
        src/gui/edit/changes/copy_cues_change.hpp
        src/gui/edit/changes/copy_cues_change.cpp
        src/gui/edit/changes/delete_orphan_change.hpp
        src/gui/edit/changes/delete_orphan_change.cpp
        src/gui/edit/changes/finish_cleanup_change.hpp
        src/gui/edit/changes/finish_cleanup_change.cpp
        src/gui/edit/changes/device_setting_change.hpp
        src/gui/edit/changes/device_setting_change.cpp
        src/gui/edit/changes/merge_cues_change.hpp
        src/gui/edit/changes/merge_cues_change.cpp
        src/gui/edit/changes/restore_backups_change.hpp
        src/gui/edit/changes/restore_backups_change.cpp
        src/gui/edit/changes/restore_metadata_change.hpp
        src/gui/edit/changes/restore_metadata_change.cpp
        src/gui/edit/changes/remove_junk_cue_change.hpp
        src/gui/edit/changes/remove_junk_cue_change.cpp
        src/gui/edit/changes/repair_artwork_change.hpp
        src/gui/edit/changes/repair_artwork_change.cpp
        src/gui/artwork_rescue_sources.hpp
        src/gui/artwork_rescue_sources.cpp
        src/gui/edit/changes/fill_sample_rate_change.hpp
        src/gui/edit/changes/fill_sample_rate_change.cpp
        src/gui/edit/changes/mark_rekordbox_imported_change.hpp
        src/gui/edit/changes/mark_rekordbox_imported_change.cpp
        src/gui/edit/changes/repair_issue_change.hpp
        src/gui/edit/changes/repair_issue_change.cpp
        src/gui/edit/changes/sync_plan_change.hpp
        src/gui/edit/changes/sync_plan_change.cpp
    )
    target_include_directories(seabass_edit PUBLIC src)
    target_link_libraries(seabass_edit PUBLIC seabass_core Qt6::Core)
    if(UNIX AND NOT APPLE)
        target_link_libraries(seabass_edit PUBLIC Qt6::DBus)
    endif()
    # RepairArtworkChange asks ArtworkRescueSources for a cover, so the
    # sources live here with it: in the GUI's own list, any other user of
    # this library failed to link. TagLib travels with it, PUBLIC, because
    # the embedded-artwork source is behind SEABASS_HAVE_TAGLIB -- compiled
    # without the definition it would drop out without a word.
    if(TARGET seabass_taglib)
        target_link_libraries(seabass_edit PUBLIC seabass_taglib)
    endif()

    # Times a real staged save against a real stick -- the wall-clock half
    # of the write-path work, which the corpus runner's counts cannot
    # answer. Writes only inside <stick>/.seabass-savebench/.
    add_executable(staged_save_bench tools/staged_save_bench.cpp)
    target_include_directories(staged_save_bench PRIVATE src)
    # SQLite::SQLite3 here for the same reason as the libdjinterop_engine_*
    # test targets above: DjInterop's own object files need sqlite3's
    # symbols resolved from somewhere in the final link, and MSVC's linker,
    # unlike MinGW's, does not reliably pull them from seabass_core's own
    # bundled-amalgamation object file through two levels of static-library
    # indirection (staged_save_bench -> seabass_edit -> seabass_core).
    target_link_libraries(staged_save_bench PRIVATE seabass_edit Qt6::Core SQLite::SQLite3)

    add_subdirectory(src/gui)
    if(TARGET seabass_audio_qt)
        target_link_libraries(seabass PRIVATE seabass_audio_qt)
    endif()
    if(TARGET seabass_taglib)
        target_link_libraries(seabass PRIVATE seabass_taglib)
    endif()
    target_compile_definitions(seabass PRIVATE
        $<$<BOOL:${SEABASS_EXPERIMENTAL}>:SEABASS_EXPERIMENTAL_BUILD>)

    # --- QtQuickTest: QML-level UI tests (component behavior, not the ---
    # --- domain/controller logic the C++ tests above already cover) ---
    #
    # Qt6Test_FOUND/Qt6QuickTest_FOUND are set by the OPTIONAL_COMPONENTS
    # find_package call above whether or not they were found.
    if(SEABASS_TESTS AND NOT (Qt6Test_FOUND AND Qt6QuickTest_FOUND))
        message(FATAL_ERROR
            "Qt6 Test/QuickTest not found, so seabass_qml_tests cannot be built and "
            "its QML tests would not run.\n"
            "  Debian/Ubuntu: apt install qt6-declarative-dev libqt6quicktest6-dev\n"
            "  Fedora:        dnf install qt6-qtdeclarative-devel\n"
            "  MSYS2/UCRT64:  pacman -S mingw-w64-ucrt-x86_64-qt6-declarative\n"
            "To build without any tests at all, configure with "
            "-DSEABASS_TESTS=OFF.")
    endif()
    if(SEABASS_TESTS)
        # A separate, test-only "SeabassGui" module registration (URI, and
        # the exact QML files under test, listed again here) rather than
        # reusing seabass's own qt_add_qml_module() call: that one is
        # scoped to the `seabass` executable target, not a separately
        # linkable library, so there's nothing for a second binary to
        # import. Both targets registering the same URI is fine -- they're
        # two independent, self-contained binaries, no shared linkage,
        # same as how every C++ domain test below lists just the .cpp
        # files it needs instead of linking one shared library.
        #
        # Test *.qml files themselves are NOT compiled in here --
        # QUICK_TEST_MAIN scans a real directory for tst_*.qml at run time
        # (see the -input arg on the add_test() call below), so editing/
        # adding a test doesn't need a rebuild, only the components-under-
        # test do.
        qt_add_executable(seabass_qml_tests tests/qml/qml_test_main.cpp)
        # Must run before qt_add_qml_module() -- it reads this property at
        # call time (see src/gui/CMakeLists.txt's own comment on the exact
        # same ordering requirement for the real seabass target). Set
        # after instead, Theme.qml compiles as a plain (non-singleton)
        # type, and every bare `Theme.xxx` reference elsewhere silently
        # evaluates to undefined rather than erroring -- confusing to
        # track down the first time.
        set_source_files_properties(src/gui/qml/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
        # Same ordering requirement as Theme.qml above -- must run before
        # qt_add_qml_module() or CamelotWheelPosition silently registers as
        # a plain (non-singleton) type and every `CamelotWheelPosition.x`
        # style access in CamelotWheelPopup.qml goes undefined.
        set_source_files_properties(src/gui/qml/CamelotWheelPosition.qml
            PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
        # And FormatLabels, which was missing here while the real target
        # had it -- so `FormatLabels.label(...)` was not undefined-but-
        # harmless in tests, it was a TypeError on a plain type. Nothing
        # caught it because no test had rendered a component that calls
        # it until tst_UnreferencedFilesNotice did.
        set_source_files_properties(src/gui/qml/FormatLabels.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

        # The same module the app registers (see src/gui/CMakeLists.txt's
        # SEABASS_GUI_* lists): every page with its real controller, so
        # tests/qml-live/ can drive the real code against a real stick.
        # The app registers this from src/gui/, so it resolves to
        # qrc:/qt/qml/SeabassGui/qml/icons/... and that is the URL the QML
        # asks for. Registered from the root it would land two directories
        # deeper and every page showing the logo would render a blank box
        # under test while being perfectly fine in the app -- a difference
        # between test and app that is invisible until someone looks at a
        # screenshot and mistrusts it.
        set_source_files_properties(src/gui/qml/icons/seabass_soundbass.svg
            PROPERTIES QT_RESOURCE_ALIAS qml/icons/seabass_soundbass.svg)
        # Same aliasing, same reason: DonationPage asks for
        # qrc:/qt/qml/SeabassGui/qml/images/sebas.png, which is where the
        # app's own module puts it. Without the alias the portrait is a
        # blank circle under test and perfect in the app, which is the
        # exact failure the comment above describes.
        set_source_files_properties(src/gui/qml/images/sebas.png
            PROPERTIES QT_RESOURCE_ALIAS qml/images/sebas.png)
        foreach(icon IN LISTS SEABASS_GUI_ICONS_FROM_ROOT SEABASS_GUI_FONTS_FROM_ROOT)
            string(REPLACE "src/gui/" "" iconAlias "${icon}")
            set_source_files_properties(${icon} PROPERTIES QT_RESOURCE_ALIAS ${iconAlias})
        endforeach()
        qt_add_qml_module(seabass_qml_tests
            URI SeabassGui
            VERSION 1.0
            QML_FILES
                ${SEABASS_GUI_QML_FILES_FROM_ROOT}
            RESOURCES
                src/gui/qml/icons/seabass_soundbass.svg
                src/gui/qml/images/sebas.png
                ${SEABASS_GUI_ICONS_FROM_ROOT}
                ${SEABASS_GUI_FONTS_FROM_ROOT}
            SOURCES
                ${SEABASS_GUI_SOURCES_FROM_ROOT}
        )
        target_link_libraries(seabass_qml_tests PRIVATE
            seabass_core
            seabass_edit
            Qt6::QuickTest
            Qt6::Core
            Qt6::Gui
            Qt6::Qml
            Qt6::Quick
            Qt6::QuickControls2
            Qt6::QuickLayouts
            Qt6::QuickDialogs2
            Qt6::Multimedia
            Qt6::Concurrent
            Qt6::Svg
            # The update check, and nothing else: Seabass makes no other
            # network request. See gui/update_checker.hpp.
            Qt6::Network
            # ArtworkFixture builds a small Engine database for the
            # cover-art page's tests.
            SQLite::SQLite3
        )
        # The audio probes, for the same reason the icons are aliased
        # above: what the tests render has to be what the app renders.
        # seabass_audio_qt carries SEABASS_HAVE_QT_AUDIO as a PUBLIC
        # definition, and Preferences draws a "this build cannot decode
        # audio" line when it is absent -- so without this the tests
        # screenshot a warning the real app never shows, and a screenshot
        # nobody can trust is worse than no screenshot.
        if(TARGET seabass_audio_qt)
            target_link_libraries(seabass_qml_tests PRIVATE seabass_audio_qt)
        endif()
        # See the find_package(Qt6 ...) call above for why these two are
        # conditional rather than in the list above.
        if(TARGET Qt6::QuickEffects)
            target_link_libraries(seabass_qml_tests PRIVATE Qt6::QuickEffects)
        endif()
        if(TARGET Qt6::QuickShapes)
            target_link_libraries(seabass_qml_tests PRIVATE Qt6::QuickShapes)
        endif()
        # The generated qmltyperegistrations.cpp #includes each QML_ELEMENT
        # header via <angle brackets> (see e.g. seabass_qmltyperegistrations.cpp
        # in the build dir), resolved against the include path -- src/gui/
        # itself needs to be on it for <system_font_metrics.hpp> to resolve.
        target_include_directories(seabass_qml_tests PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/src/gui
            ${CMAKE_SOURCE_DIR}/src/gui/edit)
        # Same experimental setting the application target is built with.
        # Without this the QML tests always compiled the stable-only
        # variant of AppSettingsController while the default build of the
        # app ships the experimental one, so no QML test could reach an
        # experimental-gated section of a page -- including the whole
        # "Full stick backups" group on AppSettingsPage.
        target_compile_definitions(seabass_qml_tests PRIVATE
            $<$<BOOL:${SEABASS_EXPERIMENTAL}>:SEABASS_EXPERIMENTAL_BUILD>)

        # The same shader the app carries, at the same URL -- see
        # src/gui/CMakeLists.txt. BASE drops the src/gui/ this target's
        # root-relative path would otherwise put into it.
        if(Qt6ShaderTools_FOUND)
            qt_add_shaders(seabass_qml_tests "seabass_shaders"
                PREFIX "/seabass"
                BASE src/gui
                FILES src/gui/shaders/track_ring.frag)
        endif()
        # Both QML suites want a display; look for the thing that gives
        # them one before either is registered.
        find_program(SEABASS_XVFB_RUN xvfb-run)

        # On Linux these are requirements, not preferences. Without
        # xvfb-run there is no display and every shader-drawn item is
        # simply absent; without Qt6 ShaderTools the shader is not built
        # into the binary at all. Either way the ring, the watermark and
        # the fullscreen player go untested while the suite reports
        # green -- and the tests that would have noticed now ask the
        # platform what it can do, so they take the "nothing is drawn"
        # branch and agree with it. A build that cannot test the shader
        # should say so before the tests run, not after they pass.
        if(UNIX AND NOT APPLE)
            if(NOT SEABASS_XVFB_RUN)
                message(FATAL_ERROR
                    "xvfb-run not found, and SEABASS_TESTS=ON. The QML tests run on a real display so that "
                    "shader-drawn items are actually drawn. Install it (Debian/Ubuntu: xvfb), or configure "
                    "with -DSEABASS_TESTS=OFF if you do not want the tests at all.")
            endif()
            if(NOT Qt6ShaderTools_FOUND)
                message(FATAL_ERROR
                    "Qt6 ShaderTools not found, and SEABASS_TESTS=ON. Without it TrackRing's shader is not "
                    "compiled into the test binary and everything drawn with it silently disappears. "
                    "Install it (Debian/Ubuntu: qt6-shadertools-dev), or configure with -DSEABASS_TESTS=OFF.")
            endif()
        endif()

        # On a real display, not offscreen. The offscreen platform draws
        # no ShaderEffect at all -- silently, the item is simply empty --
        # so every page this project checks by rendering it was checked
        # without the one part of it that needs a GPU, and the tests that
        # noticed had to assert the ABSENCE of the ring rather than the
        # ring. Xvfb with Mesa's software OpenGL costs no GPU, gives the
        # same pixels on every machine, and renders what a user sees.
        #
        # XDG_CONFIG_HOME is redirected because the QML tests build real
        # controllers, and AppSettingsController is backed by QSettings --
        # without this a test could read, or a future one write, the
        # developer's own Seabass settings.
        if(SEABASS_XVFB_RUN)
            add_test(NAME seabass_qml_tests
                COMMAND ${SEABASS_XVFB_RUN} -a -s "-screen 0 1280x1024x24"
                    $<TARGET_FILE:seabass_qml_tests> -input ${CMAKE_CURRENT_SOURCE_DIR}/tests/qml)
            # The style is pinned, which it never had to be offscreen:
            # that platform offered no style hint, so every machine got
            # Basic. On a real display the KDE platform theme answers
            # with Breeze, which has its own metrics -- so the same
            # pixel assertions would measure Breeze here and Basic in a
            # container, and a green run on one would say nothing about
            # the other. Override it deliberately (QT_QUICK_CONTROLS_STYLE
            # in the environment) to look at the app in its desktop
            # style; the suite itself compares like with like.
            set(_seabass_qml_test_env
                "QT_QPA_PLATFORM=xcb;QSG_RHI_BACKEND=opengl;LIBGL_ALWAYS_SOFTWARE=1;QT_QUICK_CONTROLS_STYLE=Basic;SEABASS_SHADER_EXPECTED=1;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/qml-test-config")
        else()
            # No xvfb-run: fall back to offscreen rather than register
            # nothing, and say so loudly. A build that tests every page
            # without a shader is worth having; one that tests no page is
            # not, and a silent gap is how this was missed for months.
            message(WARNING "xvfb-run not found: seabass_qml_tests falls back to the offscreen platform, "
                "where no ShaderEffect draws. The ring, the watermark and anything else shader-drawn "
                "go untested in this build.")
            add_test(NAME seabass_qml_tests
                COMMAND seabass_qml_tests -input ${CMAKE_CURRENT_SOURCE_DIR}/tests/qml)
            # No style named here, on purpose. The harness calls
            # gui::applyDefaultControlsStyle(), which is the same
            # function main.cpp calls, so the suite runs under whatever
            # the app would run under on this platform and the two
            # cannot drift apart.
            #
            # Naming one here would defeat that: the shared call returns
            # early when QT_QUICK_CONTROLS_STYLE is already set, so
            # pinning Material for macOS -- which this did for a few
            # hours -- would leave the one platform that most needs the
            # shared code path as the one platform that never runs it.
            #
            # The xcb branch above still pins Basic. That is deliberate
            # and different: Linux leaves the style to KDE's platform
            # integration, so there is no single answer to share, and the
            # suite pins one to compare like with like.
            set(_seabass_qml_test_env "QT_QPA_PLATFORM=offscreen;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/qml-test-config")
        endif()
        # MSVC only: the offscreen platform's font backend needs pointing
        # at real font files explicitly on Windows, unlike the "windows"
        # platform (which uses DirectWrite directly, no directory needed)
        # or MSYS2's Qt (whose offscreen plugin resolves fonts through the
        # fontconfig it's built against). Without this, EVERY glyph in
        # every QML test renders as a same-width tofu box -- confirmed
        # directly with a screenshot -- which is indistinguishable from
        # the app itself at a glance but silently wrecks every pixel
        # measurement downstream (text width, centering, elision): seven
        # pixel-measurement cases across five pages failed on Windows
        # this way, some by over a hundred pixels, none of them a real
        # layout bug. Confirmed fixed with the same screenshot once this
        # was set. $ENV{SystemRoot} rather than a literal C:/Windows,
        # since that's not guaranteed on every Windows install.
        # Every screenshot case skipped without this, and a skip proves
        # nothing: fourteen of them, one per page, sitting at "SKIP:
        # SEABASS_SCREENSHOT_DIR not set" in every run this project has
        # ever done. They render a page and grab it, so running them
        # catches a page that throws, fails to render or produces an
        # empty grab -- none of which the assertions elsewhere look for.
        # The PNGs are a by-product worth having too: this project's
        # standard for a UI claim is a real offscreen screenshot, and
        # they now exist after every test run rather than only when
        # somebody remembers the variable.
        file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/screenshots")
        list(APPEND _seabass_qml_test_env "SEABASS_SCREENSHOT_DIR=${CMAKE_CURRENT_BINARY_DIR}/screenshots")
        if(MSVC)
            list(APPEND _seabass_qml_test_env "QT_QPA_FONTDIR=$ENV{SystemRoot}/Fonts")
        endif()
        if(WIN32)
            # With no console attached, Qt sends log output and QtQuickTest's
            # PASS/FAIL report to OutputDebugString, where ctest never sees
            # it: a failing run looks like an empty log and exit code 1.
            list(APPEND _seabass_qml_test_env "QT_FORCE_STDERR_LOGGING=1")
        endif()
        set_tests_properties(seabass_qml_tests PROPERTIES ENVIRONMENT "${_seabass_qml_test_env}")

        # The same suite again, under the style the Linux build actually
        # starts with. The pinned lane above compares pixels like with
        # like across machines; this one answers a different question --
        # does the program a Linux user runs behave -- and the two
        # answers have already differed. Under org.kde.desktop the
        # playlist picker's rows came out zero wide and could not be
        # clicked at all, three pages failed to instantiate on a
        # TypeError thrown inside the style's own ComboBox, and none of
        # it was visible from the pinned lane. That is what seabass#18
        # asked for and it stayed open for months because nobody had a
        # lane to run it in.
        #
        # Registered only where the style is installed, so a container
        # without kf6-qqc2-desktop-style skips the lane rather than
        # failing it, and it takes the same xvfb lock as the others.
        if(SEABASS_XVFB_RUN)
            find_path(SEABASS_KDE_STYLE_QMLDIR "org/kde/desktop/qmldir"
                HINTS ${QT6_INSTALL_PREFIX}/${QT6_INSTALL_QML} /usr/lib/${CMAKE_LIBRARY_ARCHITECTURE}/qt6/qml
                      /usr/lib/qt6/qml
                NO_CACHE)
            if(SEABASS_KDE_STYLE_QMLDIR)
                add_test(NAME seabass_qml_desktop_style_tests
                    COMMAND ${SEABASS_XVFB_RUN} -a -s "-screen 0 1280x1024x24"
                        $<TARGET_FILE:seabass_qml_tests> -input ${CMAKE_CURRENT_SOURCE_DIR}/tests/qml)
                # Its own screenshot directory, not the pinned lane's.
                # The two lanes render the same pages under different
                # styles and save under the same names, so sharing one
                # would leave whichever ran last, and a screenshot whose
                # style you cannot tell is not evidence of anything.
                #
                # This lane had none at all, which is the gap that left
                # seabass#45 undecidable: the one lane where a style
                # question can be answered was the one lane that saved
                # no pixels.
                file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/screenshots-desktop-style")
                set_tests_properties(seabass_qml_desktop_style_tests PROPERTIES
                    ENVIRONMENT "QT_QPA_PLATFORM=xcb;QSG_RHI_BACKEND=opengl;LIBGL_ALWAYS_SOFTWARE=1;QT_QUICK_CONTROLS_STYLE=org.kde.desktop;SEABASS_SHADER_EXPECTED=1;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/qml-desktop-style-config;SEABASS_SCREENSHOT_DIR=${CMAKE_CURRENT_BINARY_DIR}/screenshots-desktop-style"
                    RESOURCE_LOCK xvfb)
            else()
                message(STATUS "org.kde.desktop not found: the desktop-style QML lane is not registered. "
                    "Install kf6-qqc2-desktop-style to run the suite under the style the Linux build ships.")
            endif()
        endif()

        # What a shader draws, pixel by pixel. Same display as the suite
        # above, kept as its own check because these read back real
        # pixels and are the slowest of the lot.
        if(SEABASS_XVFB_RUN AND Qt6ShaderTools_FOUND)
            add_test(NAME seabass_qml_shader_tests
                COMMAND ${SEABASS_XVFB_RUN} -a -s "-screen 0 1280x1024x24"
                    $<TARGET_FILE:seabass_qml_tests> -input ${CMAKE_CURRENT_SOURCE_DIR}/tests/qml-shader)
            set_tests_properties(seabass_qml_shader_tests PROPERTIES ENVIRONMENT
                "QT_QPA_PLATFORM=xcb;QSG_RHI_BACKEND=opengl;LIBGL_ALWAYS_SOFTWARE=1;QT_QUICK_CONTROLS_STYLE=Basic;SEABASS_SHADER_EXPECTED=1;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/qml-test-config")
            # Both suites start their own `xvfb-run -a`, whose search for
            # a free display and the lock it then takes are not one
            # atomic step: run in parallel they can choose the same
            # number and one dies with "Server is already active". Only
            # the shader suite used xvfb before this, so the race is new.
            set_tests_properties(seabass_qml_tests seabass_qml_shader_tests PROPERTIES RESOURCE_LOCK xvfb)
        else()
            # A warning, not a status line: without it nothing at all checks
            # what the shader draws, and that should not scroll past unseen.
            message(WARNING "seabass_qml_shader_tests NOT registered: it needs xvfb-run and Qt6 ShaderTools. "
                "What TrackRing's shader draws is untested in this build.")
        endif()
    else()
        message(STATUS "SEABASS_TESTS=OFF: seabass_qml_tests not built")
    endif()
elseif(SEABASS_GUI)
    message(FATAL_ERROR
        "Qt6 not found, so neither the seabass GUI nor its QML tests can be built.\n"
        "  Debian/Ubuntu: apt install qt6-base-dev qt6-declarative-dev libqt6quicktest6-dev qt6-multimedia-dev qt6-svg-dev\n"
        "  Point at a non-default prefix with -DCMAKE_PREFIX_PATH=<qt6 prefix>\n"
        "To build only the CLI and the C++ tests, configure with -DSEABASS_GUI=OFF.")
else()
    message(WARNING
        "SEABASS_GUI=OFF: the GUI and its QML suite are not built; ctest covers "
        "the CLI and C++ tests only.")
endif()

# --- domain-layer unit tests (no infrastructure deps, no fixtures needed) ---
#
# Everything from here to the end of the file is the test suite, wrapped in
# one SEABASS_TESTS guard rather than indented, so the guard stays a
# two-line diff and every target below reads the same as it did before.
if(SEABASS_TESTS)

enable_testing()
add_executable(duration_cache_test tests/duration_cache_test.cpp)
target_include_directories(duration_cache_test PRIVATE src)
target_link_libraries(duration_cache_test PRIVATE seabass_core)
add_test(NAME duration_cache_test COMMAND duration_cache_test)

add_executable(fill_missing_durations_test tests/fill_missing_durations_test.cpp)
target_include_directories(fill_missing_durations_test PRIVATE src)
target_link_libraries(fill_missing_durations_test PRIVATE seabass_core)
add_test(NAME fill_missing_durations_test COMMAND fill_missing_durations_test)

add_executable(file_operation_log_test tests/file_operation_log_test.cpp)
target_include_directories(file_operation_log_test PRIVATE src tests)
target_link_libraries(file_operation_log_test PRIVATE seabass_core)
add_test(NAME file_operation_log_test COMMAND file_operation_log_test)

add_executable(silence_cache_test tests/silence_cache_test.cpp)
target_include_directories(silence_cache_test PRIVATE src)
target_link_libraries(silence_cache_test PRIVATE seabass_core)
add_test(NAME silence_cache_test COMMAND silence_cache_test)

add_executable(metadata_cache_test tests/metadata_cache_test.cpp)
target_include_directories(metadata_cache_test PRIVATE src)
target_link_libraries(metadata_cache_test PRIVATE seabass_core)
add_test(NAME metadata_cache_test COMMAND metadata_cache_test)

add_executable(metadata_store_test tests/metadata_store_test.cpp)
target_include_directories(metadata_store_test PRIVATE src)
# Links SQLite directly as well: one case writes a database in an
# earlier schema by hand, to prove the store moves it aside.
target_link_libraries(metadata_store_test PRIVATE seabass_core SQLite::SQLite3)
add_test(NAME metadata_store_test COMMAND metadata_store_test)

add_executable(no_em_dashes_test tests/no_em_dashes_test.cpp)
target_compile_definitions(no_em_dashes_test PRIVATE SEABASS_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME no_em_dashes_test COMMAND no_em_dashes_test)

# The vendored crate-digger specs and the parser generated from them have
# to describe the same thing: nothing forces two vendored files to agree,
# and a spec updated without regenerating the parser fails silently.
add_executable(kaitai_spec_matches_parser_test tests/kaitai_spec_matches_parser_test.cpp)
target_compile_definitions(kaitai_spec_matches_parser_test PRIVATE SEABASS_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME kaitai_spec_matches_parser_test COMMAND kaitai_spec_matches_parser_test)

add_executable(no_manual_file_urls_test tests/no_manual_file_urls_test.cpp)
target_compile_definitions(no_manual_file_urls_test PRIVATE SEABASS_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME no_manual_file_urls_test COMMAND no_manual_file_urls_test)

# Source guard, same shape as the one above: no QFutureWatcher result
# read raw in the GUI, because the exception it rethrows lands on the
# GUI thread and terminates the process.
add_executable(no_raw_future_reads_test tests/no_raw_future_reads_test.cpp)
target_compile_definitions(no_raw_future_reads_test PRIVATE SEABASS_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME no_raw_future_reads_test COMMAND no_raw_future_reads_test)

# Source guard: no path crosses between std::string and fs::path through
# the ANSI code page (src/infrastructure/paths/utf8_path.hpp). Silent on
# Linux and macOS, a crash or a missing file on Windows.
add_executable(no_narrow_path_conversions_test tests/no_narrow_path_conversions_test.cpp)
target_compile_definitions(no_narrow_path_conversions_test PRIVATE SEABASS_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME no_narrow_path_conversions_test COMMAND no_narrow_path_conversions_test)

# Runs the real .githooks scripts, so it needs a POSIX shell and a git.
# Both are a given on the platforms that build this; on Windows they come
# with Git for Windows, which is already required to clone the submodules.
add_executable(no_claude_trailers_test tests/no_claude_trailers_test.cpp)
target_compile_definitions(no_claude_trailers_test PRIVATE SEABASS_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME no_claude_trailers_test COMMAND no_claude_trailers_test)

add_executable(real_file_sizes_test tests/real_file_sizes_test.cpp)
target_include_directories(real_file_sizes_test PRIVATE src)
target_link_libraries(real_file_sizes_test PRIVATE seabass_core)
add_test(NAME real_file_sizes_test COMMAND real_file_sizes_test)

# The Linux formatter only builds into seabass_core on Linux, but the value
# this pins is one a second operating system has to agree with -- that is the
# whole point of it -- so the test compiles the source directly and runs
# everywhere, macOS included.
add_executable(walk_tree_test tests/walk_tree_test.cpp)
target_include_directories(walk_tree_test PRIVATE src)
target_link_libraries(walk_tree_test PRIVATE storageprobe)
add_test(NAME walk_tree_test COMMAND walk_tree_test)

add_executable(linux_usb_formatter_test
    tests/linux_usb_formatter_test.cpp
    src/infrastructure/media/linux_usb_formatter.cpp
)
target_link_libraries(linux_usb_formatter_test PRIVATE seabass_core_testing)
target_include_directories(linux_usb_formatter_test PRIVATE src)
target_link_libraries(linux_usb_formatter_test PRIVATE seabass_core)
add_test(NAME linux_usb_formatter_test COMMAND linux_usb_formatter_test)

add_executable(rekordbox_xml_writer_test tests/rekordbox_xml_writer_test.cpp)
target_include_directories(rekordbox_xml_writer_test PRIVATE src)
target_link_libraries(rekordbox_xml_writer_test PRIVATE seabass_core)
add_test(NAME rekordbox_xml_writer_test COMMAND rekordbox_xml_writer_test)

add_executable(export_rekordbox_xml_test tests/export_rekordbox_xml_test.cpp)
target_include_directories(export_rekordbox_xml_test PRIVATE src)
target_link_libraries(export_rekordbox_xml_test PRIVATE seabass_core)
add_test(NAME export_rekordbox_xml_test COMMAND export_rekordbox_xml_test
    ${CMAKE_SOURCE_DIR}/tests/fixtures/anonymized_library)
set_tests_properties(export_rekordbox_xml_test PROPERTIES LABELS "integration")

add_executable(collapse_catalog_rows_test tests/collapse_catalog_rows_test.cpp)
target_include_directories(collapse_catalog_rows_test PRIVATE src)
target_link_libraries(collapse_catalog_rows_test PRIVATE seabass_core)
add_test(NAME collapse_catalog_rows_test COMMAND collapse_catalog_rows_test)

add_executable(unreferenced_tracks_test tests/unreferenced_tracks_test.cpp)
target_include_directories(unreferenced_tracks_test PRIVATE src)
target_link_libraries(unreferenced_tracks_test PRIVATE seabass_core)
add_test(NAME unreferenced_tracks_test COMMAND unreferenced_tracks_test)

add_executable(stick_layout_test tests/stick_layout_test.cpp)
target_include_directories(stick_layout_test PRIVATE src)
target_link_libraries(stick_layout_test PRIVATE seabass_core)
add_test(NAME stick_layout_test COMMAND stick_layout_test)

add_executable(find_unreferenced_files_test tests/find_unreferenced_files_test.cpp)
target_include_directories(find_unreferenced_files_test PRIVATE src)
target_link_libraries(find_unreferenced_files_test PRIVATE seabass_core)
add_test(NAME find_unreferenced_files_test COMMAND find_unreferenced_files_test)

add_executable(audio_file_walk_test tests/audio_file_walk_test.cpp)
target_include_directories(audio_file_walk_test PRIVATE src)
target_link_libraries(audio_file_walk_test PRIVATE seabass_core)
add_test(NAME audio_file_walk_test COMMAND audio_file_walk_test)

# Only built when TagLib was found, same as the probe it covers.
if(TARGET seabass_taglib)
    add_executable(taglib_metadata_probe_test tests/taglib_metadata_probe_test.cpp)
    target_link_libraries(taglib_metadata_probe_test PRIVATE seabass_taglib)
    add_test(NAME taglib_metadata_probe_test COMMAND taglib_metadata_probe_test)

    add_executable(taglib_duration_probe_test tests/taglib_duration_probe_test.cpp)
    target_link_libraries(taglib_duration_probe_test PRIVATE seabass_taglib)
    add_test(NAME taglib_duration_probe_test COMMAND taglib_duration_probe_test)

    # The whole unreferenced-file scan, composed the way the GUI
    # composes it -- which is only meaningful with a real probe behind
    # it, hence inside this same if().
    add_executable(stray_file_scan_test tests/stray_file_scan_test.cpp)
    target_include_directories(stray_file_scan_test PRIVATE src)
    target_link_libraries(stray_file_scan_test PRIVATE seabass_core seabass_taglib)
    add_test(NAME stray_file_scan_test COMMAND stray_file_scan_test)

    # The same scan against a real stick. Needs hardware plugged in, so
    # it is the one test that cannot simply be required -- but it used to
    # be registered unconditionally and `return 0` without a stick, which
    # meant the normal suite carried a green line for a live scan nobody
    # had run. It is now registered only when the build was configured
    # with -DSEABASS_LIVE_STICK=<path>, which is a stated choice recorded
    # in CMakeCache.txt, and that path is handed to the test so it really
    # runs. The binary is still built either way, so it cannot rot
    # unnoticed. See docs/unreferenced-file-cleanup-plan.md, step 7.
    set(SEABASS_LIVE_STICK "" CACHE PATH
        "Mounted USB stick to run the live stray-file scan against (empty: not registered)")
    add_executable(stray_scan_live_test tests/stray_scan_live_test.cpp)
    target_include_directories(stray_scan_live_test PRIVATE src)
    target_link_libraries(stray_scan_live_test PRIVATE seabass_core seabass_taglib)
    if(SEABASS_LIVE_STICK)
        add_test(NAME stray_scan_live_test COMMAND stray_scan_live_test)
        set_tests_properties(stray_scan_live_test PROPERTIES
            LABELS "integration"
            ENVIRONMENT "SEABASS_LIVE_STICK=${SEABASS_LIVE_STICK}")
    else()
        message(STATUS
            "SEABASS_LIVE_STICK empty: stray_scan_live_test is built but not registered; "
            "configure with -DSEABASS_LIVE_STICK=/path/to/stick to run it")
    endif()
endif()

add_executable(duplicate_cue_consolidation_test
    tests/duplicate_cue_consolidation_test.cpp
)
target_link_libraries(duplicate_cue_consolidation_test PRIVATE seabass_core_testing)
target_include_directories(duplicate_cue_consolidation_test PRIVATE src)
add_test(NAME duplicate_cue_consolidation_test COMMAND duplicate_cue_consolidation_test)

add_executable(duplicate_cleanup_test
    src/domain/duplicate_cleanup.cpp
    tests/duplicate_cleanup_test.cpp
)
target_link_libraries(duplicate_cleanup_test PRIVATE seabass_core_testing)
target_include_directories(duplicate_cleanup_test PRIVATE src)
add_test(NAME duplicate_cleanup_test COMMAND duplicate_cleanup_test)

add_executable(cleanup_leftovers_test
    src/domain/cleanup_leftovers.cpp
    tests/cleanup_leftovers_test.cpp
)
target_link_libraries(cleanup_leftovers_test PRIVATE seabass_core_testing)
target_include_directories(cleanup_leftovers_test PRIVATE src)
add_test(NAME cleanup_leftovers_test COMMAND cleanup_leftovers_test)

add_executable(library_consistency_test
    src/domain/library_consistency.cpp
    tests/library_consistency_test.cpp
)
target_link_libraries(library_consistency_test PRIVATE seabass_core_testing)
target_include_directories(library_consistency_test PRIVATE src)
add_test(NAME library_consistency_test COMMAND library_consistency_test)

add_executable(filesystem_health_test
    tests/filesystem_health_test.cpp
    src/infrastructure/media/filesystem_health.cpp
)
# The same platform split seabass_core makes for the command runner: this
# test builds the media file on its own rather than linking the library.
if(WIN32)
    target_sources(filesystem_health_test PRIVATE src/infrastructure/process/run_command_windows.cpp)
else()
    target_sources(filesystem_health_test PRIVATE src/infrastructure/process/run_command_posix.cpp)
endif()
target_include_directories(filesystem_health_test PRIVATE src tests)
add_test(NAME filesystem_health_test COMMAND filesystem_health_test)

# Engine rows that do not say their sample rate, and the files that do.
# The import prompt a Denon player raises, and the counter behind it.
add_executable(engine_import_state_test
    src/infrastructure/engine/engine_analysis_state.cpp
    src/infrastructure/engine/engine_import_state.cpp
    src/infrastructure/paths/seabass_paths.cpp
    tests/engine_import_state_test.cpp
)
target_include_directories(engine_import_state_test PRIVATE src tests)
target_link_libraries(engine_import_state_test PRIVATE DjInterop SQLite::SQLite3)
add_test(NAME engine_import_state_test COMMAND engine_import_state_test)

add_executable(engine_restore_check_test tests/engine_restore_check_test.cpp)
target_include_directories(engine_restore_check_test PRIVATE src tests)
target_link_libraries(engine_restore_check_test PRIVATE seabass_core SQLite::SQLite3)
add_test(NAME engine_restore_check_test COMMAND engine_restore_check_test)

add_executable(waveform_readers_test tests/waveform_readers_test.cpp)
target_include_directories(waveform_readers_test PRIVATE src tests)
target_link_libraries(waveform_readers_test PRIVATE seabass_core DjInterop)
target_compile_definitions(waveform_readers_test PRIVATE SEABASS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME waveform_readers_test COMMAND waveform_readers_test)

# Issue #38: how many tracks an Engine player will have to analyse. Takes
# the fixture path, because its real distribution (1214 of 1564) is what
# makes the count mean anything.
add_executable(engine_analysis_state_test
    src/infrastructure/engine/engine_analysis_state.cpp
    tests/engine_analysis_state_test.cpp
)
target_include_directories(engine_analysis_state_test PRIVATE src tests)
target_link_libraries(engine_analysis_state_test PRIVATE SQLite::SQLite3)
add_test(NAME engine_analysis_state_test COMMAND engine_analysis_state_test
    ${CMAKE_SOURCE_DIR}/tests/fixtures/anonymized_library)

add_executable(engine_sample_rates_test
    src/infrastructure/engine/engine_sample_rates.cpp
    tests/engine_sample_rates_test.cpp
)
target_include_directories(engine_sample_rates_test PRIVATE src tests)
target_link_libraries(engine_sample_rates_test PRIVATE DjInterop SQLite::SQLite3 ZLIB::ZLIB)
add_test(NAME engine_sample_rates_test COMMAND engine_sample_rates_test)

add_executable(engine_artwork_test
    tests/engine_artwork_test.cpp
    src/infrastructure/engine/engine_artwork.cpp
    # The repaired image is written durably, like every other write onto a
    # stick, so this test needs the writer and its long-path helper.
)
target_link_libraries(engine_artwork_test PRIVATE seabass_core_testing)
target_include_directories(engine_artwork_test PRIVATE src tests)
target_link_libraries(engine_artwork_test PRIVATE SQLite::SQLite3)
add_test(NAME engine_artwork_test COMMAND engine_artwork_test)

add_executable(junk_cue_test
    tests/junk_cue_test.cpp
)
target_link_libraries(junk_cue_test PRIVATE seabass_core_testing)
target_include_directories(junk_cue_test PRIVATE src)
add_test(NAME junk_cue_test COMMAND junk_cue_test)

add_executable(clustered_cue_test tests/clustered_cue_test.cpp)
target_link_libraries(clustered_cue_test PRIVATE seabass_core)
target_include_directories(clustered_cue_test PRIVATE src)
target_compile_definitions(clustered_cue_test PRIVATE SEABASS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME clustered_cue_test COMMAND clustered_cue_test)

add_executable(matching_policy_test
    tests/matching_policy_test.cpp
)
target_link_libraries(matching_policy_test PRIVATE seabass_core_testing)
target_include_directories(matching_policy_test PRIVATE src)
add_test(NAME matching_policy_test COMMAND matching_policy_test)

add_executable(library_statistics_test
    src/domain/library_statistics.cpp
    tests/library_statistics_test.cpp
)
target_include_directories(library_statistics_test PRIVATE src)
add_test(NAME library_statistics_test COMMAND library_statistics_test)

add_executable(disk_usage_test
    src/domain/disk_usage.cpp
    tests/disk_usage_test.cpp
)
target_include_directories(disk_usage_test PRIVATE src)
add_test(NAME disk_usage_test COMMAND disk_usage_test)

add_executable(filesystem_compatibility_test
    src/domain/filesystem_compatibility.cpp
    tests/filesystem_compatibility_test.cpp
)
target_include_directories(filesystem_compatibility_test PRIVATE src)
add_test(NAME filesystem_compatibility_test COMMAND filesystem_compatibility_test)

add_executable(track_matching_test
    tests/track_matching_test.cpp
)
target_link_libraries(track_matching_test PRIVATE seabass_core_testing)
target_include_directories(track_matching_test PRIVATE src)
add_test(NAME track_matching_test COMMAND track_matching_test)

add_executable(camelot_key_test
    src/domain/camelot_key.cpp
    tests/camelot_key_test.cpp
)
target_include_directories(camelot_key_test PRIVATE src)
add_test(NAME camelot_key_test COMMAND camelot_key_test)

# The beat grid a playing track's display keeps time by: the marker
# arithmetic by hand, and both readers against the committed fixture.
add_executable(beat_grid_test tests/beat_grid_test.cpp)
target_include_directories(beat_grid_test PRIVATE src)
target_link_libraries(beat_grid_test PRIVATE seabass_core)
add_test(NAME beat_grid_test COMMAND beat_grid_test ${CMAKE_SOURCE_DIR}/tests/fixtures/anonymized_library)
set_tests_properties(beat_grid_test PROPERTIES LABELS "integration")

add_executable(audio_levels_test
    src/domain/audio_levels.cpp
    tests/audio_levels_test.cpp
)
target_include_directories(audio_levels_test PRIVATE src)
add_test(NAME audio_levels_test COMMAND audio_levels_test)

add_executable(rekordbox_key_parser_test
    tests/rekordbox_key_parser_test.cpp
)
target_link_libraries(rekordbox_key_parser_test PRIVATE seabass_core_testing)
target_include_directories(rekordbox_key_parser_test PRIVATE src)
target_link_libraries(rekordbox_key_parser_test PRIVATE DjInterop)
add_test(NAME rekordbox_key_parser_test COMMAND rekordbox_key_parser_test)

add_executable(sync_planning_test
    tests/sync_planning_test.cpp
)
target_link_libraries(sync_planning_test PRIVATE seabass_core_testing)
target_include_directories(sync_planning_test PRIVATE src)
add_test(NAME sync_planning_test COMMAND sync_planning_test)

add_executable(format_usb_stick_test
    tests/format_usb_stick_test.cpp
)
target_link_libraries(format_usb_stick_test PRIVATE seabass_core_testing)
target_include_directories(format_usb_stick_test PRIVATE src)
add_test(NAME format_usb_stick_test COMMAND format_usb_stick_test)

add_executable(cross_source_sync_conflict_test
    src/domain/cross_source_sync_conflict.cpp
    tests/cross_source_sync_conflict_test.cpp
)
target_link_libraries(cross_source_sync_conflict_test PRIVATE seabass_core_testing)
target_include_directories(cross_source_sync_conflict_test PRIVATE src)
add_test(NAME cross_source_sync_conflict_test COMMAND cross_source_sync_conflict_test)

add_executable(track_scope_test
    src/domain/track_scope.cpp
    tests/track_scope_test.cpp
)
target_include_directories(track_scope_test PRIVATE src)
add_test(NAME track_scope_test COMMAND track_scope_test)

# library_catalog_cache.cpp lives in src/gui/ (it's the format-string ->
# concrete-reader dispatch every controller's own scan-task function
# already did, just consolidated) but needs no Qt itself -- realScan()/
# realMtime() reference the rekordbox/engine/onelibrary readers directly,
# so this links the full seabass_core (which already PUBLIC-links
# rekordbox_format/DjInterop/SQLite3) rather than hand-picking individual
# infrastructure .cpp files. The test itself only exercises the injected-
# fake constructor, never realScan()/realMtime(), but the linker still
# needs those symbols to resolve since they're compiled into the same
# translation unit.
add_executable(library_catalog_cache_test
    src/gui/library_catalog_cache.cpp
    tests/library_catalog_cache_test.cpp
)
target_include_directories(library_catalog_cache_test PRIVATE src)
target_link_libraries(library_catalog_cache_test PRIVATE seabass_core Threads::Threads)
add_test(NAME library_catalog_cache_test COMMAND library_catalog_cache_test)

add_executable(library_fingerprint_test
    tests/library_fingerprint_test.cpp
)
target_include_directories(library_fingerprint_test PRIVATE src)
target_link_libraries(library_fingerprint_test PRIVATE seabass_core)
add_test(NAME library_fingerprint_test COMMAND library_fingerprint_test)

add_executable(advise_stick_backup_test
    tests/advise_stick_backup_test.cpp
)
target_include_directories(advise_stick_backup_test PRIVATE src)
target_link_libraries(advise_stick_backup_test PRIVATE seabass_core)
add_test(NAME advise_stick_backup_test COMMAND advise_stick_backup_test)

add_executable(fuzzy_matcher_test
    src/domain/fuzzy_matcher.cpp
    tests/fuzzy_matcher_test.cpp
)
target_include_directories(fuzzy_matcher_test PRIVATE src)
add_test(NAME fuzzy_matcher_test COMMAND fuzzy_matcher_test)

add_executable(track_queries_test
    src/domain/track_queries.cpp
    tests/track_queries_test.cpp
)
target_include_directories(track_queries_test PRIVATE src)
add_test(NAME track_queries_test COMMAND track_queries_test)

add_executable(cue_color_normalization_test
    src/infrastructure/rekordbox/kaitai_rekordbox_reader.cpp
    src/infrastructure/rekordbox/anlz_byte_source.cpp
    # KaitaiRekordboxReader resolves its analysis-file source from the
    # library root itself, so that a stick backup browsed in place is read
    # correctly by every existing caller without one of them changing (see
    # anlzSourceForPioneerRoot). The cost is here: the pdb reader now pulls
    # in the archive stack, so a test that lists its own sources has to
    # list those too.
    src/infrastructure/rekordbox/anlz_source_for_root.cpp
    src/infrastructure/stick_backup/archive_anlz_source.cpp
    src/infrastructure/stick_backup/zip64_reader.cpp
    src/infrastructure/stick_backup/zip_format.cpp
    tests/cue_color_normalization_test.cpp
)
target_link_libraries(cue_color_normalization_test PRIVATE seabass_core_testing)
target_include_directories(cue_color_normalization_test PRIVATE src)
target_link_libraries(cue_color_normalization_test PRIVATE DjInterop rekordbox_format kaitai_cpp_stl_runtime
    ZLIB::ZLIB SQLite::SQLite3)
add_test(NAME cue_color_normalization_test COMMAND cue_color_normalization_test)

# The analysis-path index decides which file a cue write lands in, so it
# is checked against the single-id lookup it replaces on every id the
# fixture holds, on every id it does not, and on databases it cannot read.
add_executable(anlz_path_index_test
    tests/anlz_path_index_test.cpp
)
target_link_libraries(anlz_path_index_test PRIVATE seabass_core_testing)
target_include_directories(anlz_path_index_test PRIVATE src)
target_link_libraries(anlz_path_index_test PRIVATE rekordbox_format kaitai_cpp_stl_runtime)
add_test(NAME anlz_path_index_test COMMAND anlz_path_index_test
    ${CMAKE_SOURCE_DIR}/tests/fixtures/anonymized_library)
set_tests_properties(anlz_path_index_test PROPERTIES LABELS "integration")

add_executable(anlz_cue_codec_test
    tests/anlz_cue_codec_test.cpp
)
target_link_libraries(anlz_cue_codec_test PRIVATE seabass_core_testing)
target_include_directories(anlz_cue_codec_test PRIVATE src)
add_test(NAME anlz_cue_codec_test COMMAND anlz_cue_codec_test)

add_executable(anlz_legacy_cue_codec_test
    tests/anlz_legacy_cue_codec_test.cpp
)
target_link_libraries(anlz_legacy_cue_codec_test PRIVATE seabass_core_testing)
target_include_directories(anlz_legacy_cue_codec_test PRIVATE src)
add_test(NAME anlz_legacy_cue_codec_test COMMAND anlz_legacy_cue_codec_test
    ${CMAKE_SOURCE_DIR}/tests/fixtures/anonymized_library)
set_tests_properties(anlz_legacy_cue_codec_test PROPERTIES LABELS "integration")

# Reads the raw bytes of all three cue lists after a write, so a writer
# that keeps only one of them up to date fails here. See the file's own
# comment: this is the test issue #33 did not have.
add_executable(rekordbox_cue_lists_agree_test
    src/infrastructure/rekordbox/rekordbox_cue_writer.cpp
    tests/rekordbox_cue_lists_agree_test.cpp
)
target_link_libraries(rekordbox_cue_lists_agree_test PRIVATE seabass_core_testing)
target_include_directories(rekordbox_cue_lists_agree_test PRIVATE src tests)
target_link_libraries(rekordbox_cue_lists_agree_test PRIVATE rekordbox_format kaitai_cpp_stl_runtime)
add_test(NAME rekordbox_cue_lists_agree_test COMMAND rekordbox_cue_lists_agree_test
    ${CMAKE_SOURCE_DIR}/tests/fixtures/anonymized_library)
set_tests_properties(rekordbox_cue_lists_agree_test PROPERTIES LABELS "integration")

add_executable(anlz_file_test
    tests/anlz_file_test.cpp
)
target_link_libraries(anlz_file_test PRIVATE seabass_core_testing)
target_include_directories(anlz_file_test PRIVATE src)
add_test(NAME anlz_file_test COMMAND anlz_file_test)

add_executable(rekordbox_loop_cue_test
    tests/rekordbox_loop_cue_test.cpp
)
target_include_directories(rekordbox_loop_cue_test PRIVATE src)
target_link_libraries(rekordbox_loop_cue_test PRIVATE rekordbox_format)
add_test(NAME rekordbox_loop_cue_test COMMAND rekordbox_loop_cue_test)

add_executable(local_restore_test
    tests/local_restore_test.cpp
)
target_link_libraries(local_restore_test PRIVATE seabass_core_testing)
target_include_directories(local_restore_test PRIVATE src)
add_test(NAME local_restore_test COMMAND local_restore_test)

add_executable(pdb_rating_write_test tests/pdb_rating_write_test.cpp)
target_include_directories(pdb_rating_write_test PRIVATE src)
target_compile_definitions(pdb_rating_write_test PRIVATE SEABASS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(pdb_rating_write_test PRIVATE seabass_core)
add_test(NAME pdb_rating_write_test COMMAND pdb_rating_write_test)

add_executable(metadata_restore_test
    src/domain/metadata_restore.cpp
    tests/metadata_restore_test.cpp
)
target_link_libraries(metadata_restore_test PRIVATE seabass_core_testing)
target_include_directories(metadata_restore_test PRIVATE src)
add_test(NAME metadata_restore_test COMMAND metadata_restore_test)

add_executable(metadata_backup_plan_test
    src/domain/metadata_backup_plan.cpp
    tests/metadata_backup_plan_test.cpp
)
target_link_libraries(metadata_backup_plan_test PRIVATE seabass_core_testing)
target_include_directories(metadata_backup_plan_test PRIVATE src)
add_test(NAME metadata_backup_plan_test COMMAND metadata_backup_plan_test)

add_executable(metadata_merge_test
    tests/metadata_merge_test.cpp
)
target_link_libraries(metadata_merge_test PRIVATE seabass_core_testing)
target_include_directories(metadata_merge_test PRIVATE src)
add_test(NAME metadata_merge_test COMMAND metadata_merge_test)

add_executable(durable_file_write_test
    tests/durable_file_write_test.cpp
)
target_link_libraries(durable_file_write_test PRIVATE seabass_core_testing)
target_include_directories(durable_file_write_test PRIVATE src)
add_test(NAME durable_file_write_test COMMAND durable_file_write_test)

add_executable(scratch_dir_guard_test
    tests/scratch_dir_guard_test.cpp
)
target_include_directories(scratch_dir_guard_test PRIVATE src)
add_test(NAME scratch_dir_guard_test COMMAND scratch_dir_guard_test)

add_executable(bulk_write_strategy_test
    src/infrastructure/bulk_write_strategy.cpp
    tests/bulk_write_strategy_test.cpp
)
target_include_directories(bulk_write_strategy_test PRIVATE src)
add_test(NAME bulk_write_strategy_test COMMAND bulk_write_strategy_test)

# Defined here rather than beside the archive tests further down: the
# backup store itself now writes archives, and its test is the first
# target that needs these.
set(SEABASS_STICK_BACKUP_FORMAT_SOURCES
    src/infrastructure/hashing/sha256.cpp
    src/infrastructure/stick_backup/posix_archive_file.cpp
    src/infrastructure/stick_backup/zip_format.cpp
    src/infrastructure/stick_backup/zip64_writer.cpp
    src/infrastructure/stick_backup/zip64_reader.cpp
    src/infrastructure/stick_backup/backup_manifest.cpp
)

add_executable(stick_space_test
    src/infrastructure/backup/stick_space.cpp
    tests/stick_space_test.cpp
)
target_include_directories(stick_space_test PRIVATE src)
add_test(NAME stick_space_test COMMAND stick_space_test)

add_executable(filesystem_backup_store_test
    ${SEABASS_STICK_BACKUP_FORMAT_SOURCES}
    tests/filesystem_backup_store_test.cpp
)
target_link_libraries(filesystem_backup_store_test PRIVATE seabass_core_testing)
target_include_directories(filesystem_backup_store_test PRIVATE src)
target_link_libraries(filesystem_backup_store_test PRIVATE ZLIB::ZLIB)
add_test(NAME filesystem_backup_store_test COMMAND filesystem_backup_store_test)

add_executable(stick_write_lock_test
    tests/stick_write_lock_test.cpp
)
target_link_libraries(stick_write_lock_test PRIVATE seabass_core_testing)
target_include_directories(stick_write_lock_test PRIVATE src)
target_link_libraries(stick_write_lock_test PRIVATE Threads::Threads)
add_test(NAME stick_write_lock_test COMMAND stick_write_lock_test)

add_executable(stick_identity_test tests/stick_identity_test.cpp)
target_include_directories(stick_identity_test PRIVATE src)
add_test(NAME stick_identity_test COMMAND stick_identity_test)

add_executable(stick_path_match_test tests/stick_path_match_test.cpp)
target_include_directories(stick_path_match_test PRIVATE src)
add_test(NAME stick_path_match_test COMMAND stick_path_match_test)

add_executable(stick_containment_test tests/stick_containment_test.cpp)
target_include_directories(stick_containment_test PRIVATE src)
target_link_libraries(stick_containment_test PRIVATE seabass_core_testing)
add_test(NAME stick_containment_test COMMAND stick_containment_test)

add_executable(stick_presence_diff_test tests/stick_presence_diff_test.cpp)
target_include_directories(stick_presence_diff_test PRIVATE src)
add_test(NAME stick_presence_diff_test COMMAND stick_presence_diff_test)

add_executable(mount_unless_mounted_test tests/mount_unless_mounted_test.cpp)
target_include_directories(mount_unless_mounted_test PRIVATE src)
add_test(NAME mount_unless_mounted_test COMMAND mount_unless_mounted_test)

# A stick pulled mid-save leaves a hot SQLite journal; this makes one and
# rolls it back (#48).
add_executable(sqlite_pending_journal_test tests/sqlite_pending_journal_test.cpp)
target_include_directories(sqlite_pending_journal_test PRIVATE src)
target_link_libraries(sqlite_pending_journal_test PRIVATE SQLite::SQLite3)
add_test(NAME sqlite_pending_journal_test COMMAND sqlite_pending_journal_test)

# Forks a child process on POSIX to prove the pid-liveness staleness rule
# against a real second process, not just an injected fake.
add_executable(library_edit_lock_store_test
    src/infrastructure/local/file_library_edit_lock_store.cpp
    src/infrastructure/system/process_liveness.cpp
    tests/library_edit_lock_store_test.cpp
)
target_link_libraries(library_edit_lock_store_test PRIVATE seabass_core_testing)
target_include_directories(library_edit_lock_store_test PRIVATE src)
add_test(NAME library_edit_lock_store_test COMMAND library_edit_lock_store_test)

# The edit-session save loop and its per-format write target live in
# src/gui/edit but are plain Qt Core code (QString, no widgets), so they
# are testable headless.
# Linked against seabass_edit rather than a list of its sources: the save
# loop now stages a change of its own (the Engine import counter, #42),
# and a hand-kept list is one more place to forget a file.
if(TARGET seabass_edit)
    add_executable(edit_session_save_loop_test tests/edit_session_save_loop_test.cpp tests/engine_information_fixture.cpp)
    target_include_directories(edit_session_save_loop_test PRIVATE src)
    # engine_information_fixture.cpp #includes <sqlite3.h> directly, which
    # needs the PUBLIC SQLite::SQLite3 alias on Windows -- see
    # make_engine_library's comment above for why seabass_edit alone
    # isn't enough there (same shape as library_backup_roundtrip_test).
    target_link_libraries(edit_session_save_loop_test PRIVATE seabass_edit SQLite::SQLite3)
    add_test(NAME edit_session_save_loop_test COMMAND edit_session_save_loop_test)

    # Links media_controller.cpp directly, the way every other GUI-class
    # test here does: the GUI is one executable target, not a library, so
    # there is nothing to link against but the sources actually needed.
    # The stick list's order and its update semantics: a row the user is
    # looking at must not move because its own state changed, and an
    # arriving stick must be an insert the view can animate rather than a
    # reset it can only redraw.
    # Needs Qt6::Gui, not just Core: the font database is a GUI thing, and
    # so is the offscreen platform plugin this runs under deliberately.
    add_executable(interface_font_test
        src/gui/interface_font.cpp
        tests/interface_font_test.cpp
    )
    target_include_directories(interface_font_test PRIVATE src)
    target_link_libraries(interface_font_test PRIVATE Qt6::Core Qt6::Gui Qt6::Test)
    add_test(NAME interface_font_test COMMAND interface_font_test)
    # MSVC only, and the same reason seabass_qml_tests needs it: the
    # offscreen platform's font backend has to be pointed at real font
    # files on Windows, and without it QFontDatabase::families() comes
    # back EMPTY -- "Cannot find font directory .../lib/fonts". A font
    # database with nothing in it is not a thing this test can say
    # anything about, and it is also the state in which the only font a
    # process has is the one this app registers itself, which is the bug
    # this test exists for.
    if(MSVC)
        set_tests_properties(interface_font_test PROPERTIES
            ENVIRONMENT "QT_QPA_FONTDIR=$ENV{SystemRoot}/Fonts")
    endif()

    add_executable(update_decision_test
        src/gui/update_decision.cpp
        tests/update_decision_test.cpp
    )
    target_include_directories(update_decision_test PRIVATE src)
    target_link_libraries(update_decision_test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME update_decision_test COMMAND update_decision_test)

    add_executable(stick_list_order_test
        src/gui/media_controller.cpp
        tests/stick_list_order_test.cpp
    )
    target_include_directories(stick_list_order_test PRIVATE src)
    target_link_libraries(stick_list_order_test PRIVATE seabass_core Qt6::Core Qt6::Concurrent Qt6::Qml Qt6::Test)
    add_test(NAME stick_list_order_test COMMAND stick_list_order_test)

    add_executable(open_folder_test
        src/gui/media_controller.cpp
        tests/open_folder_test.cpp
    )
    target_include_directories(open_folder_test PRIVATE src)
    target_link_libraries(open_folder_test PRIVATE seabass_core Qt6::Core Qt6::Concurrent Qt6::Qml)
    add_test(NAME open_folder_test COMMAND open_folder_test)

    add_executable(stick_backup_paths_test tests/stick_backup_paths_test.cpp)
    target_include_directories(stick_backup_paths_test PRIVATE src)
    target_link_libraries(stick_backup_paths_test PRIVATE Qt6::Core)
    add_test(NAME stick_backup_paths_test COMMAND stick_backup_paths_test)

    add_executable(backup_changelog_text_test
        ${SEABASS_STICK_BACKUP_FORMAT_SOURCES}
        src/gui/backup_changelog_text.cpp
        tests/backup_changelog_text_test.cpp
    )
    target_include_directories(backup_changelog_text_test PRIVATE src)
    target_link_libraries(backup_changelog_text_test PRIVATE Qt6::Core ZLIB::ZLIB)
    add_test(NAME backup_changelog_text_test COMMAND backup_changelog_text_test)

    # The restore page's proposal list, on its own: staging is kept per
    # proposal, so two stick tracks matched to one stored track stage and
    # unstage apart, and each row says what a restore writes and where.
    add_executable(metadata_restore_proposal_model_test
        src/gui/metadata_restore_proposal_model.cpp
        tests/metadata_restore_proposal_model_test.cpp
    )
    target_include_directories(metadata_restore_proposal_model_test PRIVATE src)
    target_link_libraries(metadata_restore_proposal_model_test PRIVATE seabass_core Qt6::Core Qt6::Qml)
    add_test(NAME metadata_restore_proposal_model_test COMMAND metadata_restore_proposal_model_test)

    # Sync Cue Points' list, on its own: decisions before the tracks ready
    # to sync, a search that narrows both without losing a tick, and each
    # row counting its cues the way the page shows them.
    add_executable(sync_plan_list_model_test
        src/gui/sync_plan_list_model.cpp
        tests/sync_plan_list_model_test.cpp
    )
    target_include_directories(sync_plan_list_model_test PRIVATE src)
    target_link_libraries(sync_plan_list_model_test PRIVATE seabass_core Qt6::Core Qt6::Qml)
    add_test(NAME sync_plan_list_model_test COMMAND sync_plan_list_model_test)

    add_executable(format_write_session_test tests/format_write_session_test.cpp)
    target_include_directories(format_write_session_test PRIVATE src)
    target_link_libraries(format_write_session_test PRIVATE seabass_edit Qt6::Core)
    add_test(NAME format_write_session_test COMMAND format_write_session_test)

    add_executable(edit_session_decline_wiring_test tests/edit_session_decline_wiring_test.cpp)
    target_compile_definitions(edit_session_decline_wiring_test PRIVATE
        SEABASS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME edit_session_decline_wiring_test COMMAND edit_session_decline_wiring_test)

    # SleepInhibitor: one system inhibitor however many operations overlap,
    # and, on a machine with logind, the real one listed while it is held.
    add_executable(sleep_inhibitor_test tests/sleep_inhibitor_test.cpp)
    target_include_directories(sleep_inhibitor_test PRIVATE src)
    target_link_libraries(sleep_inhibitor_test PRIVATE seabass_edit Qt6::Core)
    add_test(NAME sleep_inhibitor_test COMMAND sleep_inhibitor_test)

    # Every task that writes a stick or makes a backup keeps the system
    # awake. Read from the controller sources, like the decline test above:
    # the controllers live in the GUI executable, with no library to test.
    add_executable(sleep_inhibit_wiring_test tests/sleep_inhibit_wiring_test.cpp)
    target_compile_definitions(sleep_inhibit_wiring_test PRIVATE
        SEABASS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME sleep_inhibit_wiring_test COMMAND sleep_inhibit_wiring_test)

    # Drives one staged change against a real encrypted OneLibrary
    # database, which is the point of seabass_edit being its own target.
    # ZLIB and dl for the same reason onelibrary_cue_writer_test needs
    # them: SQLCipher is loaded at runtime.
    add_executable(restore_metadata_change_test tests/restore_metadata_change_test.cpp)
    target_include_directories(restore_metadata_change_test PRIVATE src)
    target_compile_definitions(restore_metadata_change_test PRIVATE
        SEABASS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(restore_metadata_change_test PRIVATE seabass_edit Qt6::Core ZLIB::ZLIB)
    if(NOT WIN32)
        target_link_libraries(restore_metadata_change_test PRIVATE ${CMAKE_DL_LIBS})
    endif()
    add_test(NAME restore_metadata_change_test COMMAND restore_metadata_change_test)

    add_executable(remove_junk_cue_change_test tests/remove_junk_cue_change_test.cpp)
    target_include_directories(remove_junk_cue_change_test PRIVATE src)
    target_link_libraries(remove_junk_cue_change_test PRIVATE seabass_edit Qt6::Core ZLIB::ZLIB)
    if(NOT WIN32)
        target_link_libraries(remove_junk_cue_change_test PRIVATE ${CMAKE_DL_LIBS})
    endif()
    add_test(NAME remove_junk_cue_change_test COMMAND remove_junk_cue_change_test)

    add_executable(merge_cues_change_test tests/merge_cues_change_test.cpp)
    target_include_directories(merge_cues_change_test PRIVATE src)
    target_compile_definitions(merge_cues_change_test PRIVATE
        SEABASS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(merge_cues_change_test PRIVATE seabass_edit Qt6::Core ZLIB::ZLIB)
    if(NOT WIN32)
        target_link_libraries(merge_cues_change_test PRIVATE ${CMAKE_DL_LIBS})
    endif()
    add_test(NAME merge_cues_change_test COMMAND merge_cues_change_test)

    add_executable(failed_change_rollback_test tests/failed_change_rollback_test.cpp)
    target_include_directories(failed_change_rollback_test PRIVATE src tests)
    target_link_libraries(failed_change_rollback_test PRIVATE seabass_edit Qt6::Core ZLIB::ZLIB)
    if(NOT WIN32)
        target_link_libraries(failed_change_rollback_test PRIVATE ${CMAKE_DL_LIBS})
    endif()
    add_test(NAME failed_change_rollback_test COMMAND failed_change_rollback_test)

    add_executable(restore_backups_change_test tests/restore_backups_change_test.cpp)
    target_include_directories(restore_backups_change_test PRIVATE src tests)
    target_link_libraries(restore_backups_change_test PRIVATE seabass_edit Qt6::Core ZLIB::ZLIB)
    if(NOT WIN32)
        target_link_libraries(restore_backups_change_test PRIVATE ${CMAKE_DL_LIBS})
    endif()
    add_test(NAME restore_backups_change_test COMMAND restore_backups_change_test)

    add_executable(add_cue_mirror_failure_test tests/add_cue_mirror_failure_test.cpp)
    target_include_directories(add_cue_mirror_failure_test PRIVATE src)
    target_compile_definitions(add_cue_mirror_failure_test PRIVATE
        SEABASS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(add_cue_mirror_failure_test PRIVATE seabass_edit Qt6::Core ZLIB::ZLIB)
    if(NOT WIN32)
        target_link_libraries(add_cue_mirror_failure_test PRIVATE ${CMAKE_DL_LIBS})
    endif()
    add_test(NAME add_cue_mirror_failure_test COMMAND add_cue_mirror_failure_test)

    add_executable(save_backup_discard_under_lock_test tests/save_backup_discard_under_lock_test.cpp)
    target_include_directories(save_backup_discard_under_lock_test PRIVATE src)
    target_compile_definitions(save_backup_discard_under_lock_test PRIVATE
        SEABASS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(save_backup_discard_under_lock_test PRIVATE seabass_edit Qt6::Core ZLIB::ZLIB)
    if(NOT WIN32)
        target_link_libraries(save_backup_discard_under_lock_test PRIVATE ${CMAKE_DL_LIBS})
    endif()
    add_test(NAME save_backup_discard_under_lock_test COMMAND save_backup_discard_under_lock_test)

    add_executable(junk_cue_mirror_failure_test tests/junk_cue_mirror_failure_test.cpp)
    target_include_directories(junk_cue_mirror_failure_test PRIVATE src)
    target_compile_definitions(junk_cue_mirror_failure_test PRIVATE
        SEABASS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(junk_cue_mirror_failure_test PRIVATE seabass_edit Qt6::Core ZLIB::ZLIB)
    if(NOT WIN32)
        target_link_libraries(junk_cue_mirror_failure_test PRIVATE ${CMAKE_DL_LIBS})
    endif()
    add_test(NAME junk_cue_mirror_failure_test COMMAND junk_cue_mirror_failure_test)

    add_executable(finish_cleanup_change_test tests/finish_cleanup_change_test.cpp)
    target_include_directories(finish_cleanup_change_test PRIVATE src)
    target_compile_definitions(finish_cleanup_change_test PRIVATE
        SEABASS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(finish_cleanup_change_test PRIVATE seabass_edit Qt6::Core ZLIB::ZLIB)
    if(NOT WIN32)
        target_link_libraries(finish_cleanup_change_test PRIVATE ${CMAKE_DL_LIBS})
    endif()
    add_test(NAME finish_cleanup_change_test COMMAND finish_cleanup_change_test)

    add_executable(mirror_failure_fails_the_change_test tests/mirror_failure_fails_the_change_test.cpp)
    target_include_directories(mirror_failure_fails_the_change_test PRIVATE src)
    target_compile_definitions(mirror_failure_fails_the_change_test PRIVATE
        SEABASS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(mirror_failure_fails_the_change_test PRIVATE seabass_edit Qt6::Core ZLIB::ZLIB)
    if(NOT WIN32)
        target_link_libraries(mirror_failure_fails_the_change_test PRIVATE ${CMAKE_DL_LIBS})
    endif()
    add_test(NAME mirror_failure_fails_the_change_test COMMAND mirror_failure_fails_the_change_test)

    add_executable(catalog_digest_test tests/catalog_digest_test.cpp)
    target_include_directories(catalog_digest_test PRIVATE src)
    target_compile_definitions(catalog_digest_test PRIVATE
        SEABASS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(catalog_digest_test PRIVATE seabass_core Qt6::Core ZLIB::ZLIB)
    if(NOT WIN32)
        target_link_libraries(catalog_digest_test PRIVATE ${CMAKE_DL_LIBS})
    endif()
    add_test(NAME catalog_digest_test COMMAND catalog_digest_test)

    add_executable(pending_deletion_unreadable_test tests/pending_deletion_unreadable_test.cpp)
    target_include_directories(pending_deletion_unreadable_test PRIVATE src)
    target_link_libraries(pending_deletion_unreadable_test PRIVATE seabass_core Qt6::Core ZLIB::ZLIB)
    if(NOT WIN32)
        target_link_libraries(pending_deletion_unreadable_test PRIVATE ${CMAKE_DL_LIBS})
    endif()
    add_test(NAME pending_deletion_unreadable_test COMMAND pending_deletion_unreadable_test)

    add_executable(damage_filesystem_test tests/damage_filesystem_test.cpp src/cli/damage_filesystem.cpp
        src/cli/console.cpp)
    target_include_directories(damage_filesystem_test PRIVATE src)
    target_link_libraries(damage_filesystem_test PRIVATE seabass_core Qt6::Core ZLIB::ZLIB)
    if(NOT WIN32)
        target_link_libraries(damage_filesystem_test PRIVATE ${CMAKE_DL_LIBS})
    endif()
    add_test(NAME damage_filesystem_test COMMAND damage_filesystem_test)
    # Needs mkfs.fat to build a real FAT32 volume. Without it the cases
    # that matter cannot run, and the test says so rather than passing.
    set_tests_properties(damage_filesystem_test PROPERTIES SKIP_RETURN_CODE 77)
    # Taking search permission off a directory is the only portable way to
    # make exists() fail, and it does nothing as root or on a filesystem
    # that ignores the mode. The test says so and exits 77 rather than
    # reporting a pass it did not earn.
    set_tests_properties(pending_deletion_unreadable_test PROPERTIES SKIP_RETURN_CODE 77)

    # #14: syncing a track Device Library Plus does not list must not fail
    # the save. Sync wrote the mirror inline rather than through
    # mirrorCuesOrExplain(), so OneLibraryRowMissing read as a failure.
    add_executable(sync_mirror_claim_test tests/sync_mirror_claim_test.cpp)
    target_include_directories(sync_mirror_claim_test PRIVATE src)
    target_compile_definitions(sync_mirror_claim_test PRIVATE
        SEABASS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(sync_mirror_claim_test PRIVATE seabass_edit Qt6::Core ZLIB::ZLIB)
    if(NOT WIN32)
        target_link_libraries(sync_mirror_claim_test PRIVATE ${CMAKE_DL_LIBS})
    endif()
    add_test(NAME sync_mirror_claim_test COMMAND sync_mirror_claim_test)

    add_executable(onelibrary_wal_checkpoint_test tests/onelibrary_wal_checkpoint_test.cpp)
    target_include_directories(onelibrary_wal_checkpoint_test PRIVATE src)
    target_compile_definitions(onelibrary_wal_checkpoint_test PRIVATE
        SEABASS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(onelibrary_wal_checkpoint_test PRIVATE seabass_edit Qt6::Core ZLIB::ZLIB)
    if(NOT WIN32)
        target_link_libraries(onelibrary_wal_checkpoint_test PRIVATE ${CMAKE_DL_LIBS})
    endif()
    add_test(NAME onelibrary_wal_checkpoint_test COMMAND onelibrary_wal_checkpoint_test)
endif()

add_executable(rekordbox_process_detector_test
    src/infrastructure/system/rekordbox_process_detector.cpp
    tests/rekordbox_process_detector_test.cpp
)
target_include_directories(rekordbox_process_detector_test PRIVATE src)
add_test(NAME rekordbox_process_detector_test COMMAND rekordbox_process_detector_test)

add_executable(rekordbox_settings_test
    src/infrastructure/rekordbox/rekordbox_settings_fields.cpp
    src/infrastructure/rekordbox/rekordbox_settings_reader.cpp
    src/infrastructure/rekordbox/rekordbox_settings_writer.cpp
    tests/rekordbox_settings_test.cpp
)
target_link_libraries(rekordbox_settings_test PRIVATE seabass_core_testing)
target_include_directories(rekordbox_settings_test PRIVATE src)
add_test(NAME rekordbox_settings_test COMMAND rekordbox_settings_test)

# --- stick performance: the pure scoring model, and the two probes against
# real (scratch) files ---

add_executable(stick_performance_test
    tests/stick_performance_test.cpp
)
target_link_libraries(stick_performance_test PRIVATE seabass_core_testing)
target_include_directories(stick_performance_test PRIVATE src)
target_link_libraries(stick_performance_test PRIVATE storageprobe)
add_test(NAME stick_performance_test COMMAND stick_performance_test)

add_executable(stick_performance_probe_test
    src/infrastructure/local/stick_performance_history.cpp
    tests/stick_performance_probe_test.cpp
)
target_link_libraries(stick_performance_probe_test PRIVATE seabass_core_testing)
target_include_directories(stick_performance_probe_test PRIVATE src)
target_link_libraries(stick_performance_probe_test PRIVATE storageprobe)
add_test(NAME stick_performance_probe_test COMMAND stick_performance_probe_test)

# Both probes against a real stick; skips itself unless SEABASS_LIVE_STICK
# (and, for the write half, SEABASS_LIVE_WRITE=1) is set.
add_executable(stick_performance_live_test
    tests/stick_performance_live_test.cpp
)
target_link_libraries(stick_performance_live_test PRIVATE seabass_core_testing)
target_include_directories(stick_performance_live_test PRIVATE src)
target_link_libraries(stick_performance_live_test PRIVATE storageprobe)
add_test(NAME stick_performance_live_test COMMAND stick_performance_live_test)

add_executable(local_cue_store_test
    src/infrastructure/compression/zlib_compressor.cpp
    src/infrastructure/local/local_cue_store.cpp
    tests/local_cue_store_test.cpp
)
target_link_libraries(local_cue_store_test PRIVATE seabass_core_testing)
target_include_directories(local_cue_store_test PRIVATE src)
target_link_libraries(local_cue_store_test PRIVATE SQLite::SQLite3 ZLIB::ZLIB)
add_test(NAME local_cue_store_test COMMAND local_cue_store_test)

add_executable(pdb_row_writer_test
    tests/pdb_row_writer_test.cpp
)
target_link_libraries(pdb_row_writer_test PRIVATE seabass_core_testing)
target_include_directories(pdb_row_writer_test PRIVATE src)
target_link_libraries(pdb_row_writer_test PRIVATE rekordbox_format ZLIB::ZLIB)
add_test(NAME pdb_row_writer_test COMMAND pdb_row_writer_test)

# Uses the committed real-library fixture rather than a synthetic .pdb:
# the staleness guard compares the whole file's bytes, so exercising it
# against a real 1.5MB export.pdb is closer to what it actually protects
# than a hand-built two-page buffer would be. SEABASS_SOURCE_DIR is how
# it finds that fixture, same as the corpus runner below.
add_executable(pdb_deleted_rows_test
    tests/pdb_deleted_rows_test.cpp
)
target_link_libraries(pdb_deleted_rows_test PRIVATE seabass_core rekordbox_format ZLIB::ZLIB)
target_include_directories(pdb_deleted_rows_test PRIVATE src)
target_compile_definitions(pdb_deleted_rows_test PRIVATE
    SEABASS_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME pdb_deleted_rows_test COMMAND pdb_deleted_rows_test)

add_executable(pdb_row_writer_staleness_test
    tests/pdb_row_writer_staleness_test.cpp
)
target_link_libraries(pdb_row_writer_staleness_test PRIVATE seabass_core_testing)
target_include_directories(pdb_row_writer_staleness_test PRIVATE src)
target_link_libraries(pdb_row_writer_staleness_test PRIVATE rekordbox_format ZLIB::ZLIB)
target_compile_definitions(pdb_row_writer_staleness_test PRIVATE
    SEABASS_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME pdb_row_writer_staleness_test COMMAND pdb_row_writer_staleness_test)

add_executable(pdb_row_writer_string_test
    tests/pdb_row_writer_string_test.cpp
)
target_link_libraries(pdb_row_writer_string_test PRIVATE seabass_core_testing)
target_include_directories(pdb_row_writer_string_test PRIVATE src)
target_link_libraries(pdb_row_writer_string_test PRIVATE rekordbox_format ZLIB::ZLIB)
add_test(NAME pdb_row_writer_string_test COMMAND pdb_row_writer_string_test)

add_executable(rekordbox_library_anonymizer_test tests/rekordbox_library_anonymizer_test.cpp)
target_include_directories(rekordbox_library_anonymizer_test PRIVATE src)
# Reads tests/fixtures/exportExt.pdb: the exportExt cases need a real one,
# because a synthetic pdb cannot show a scrub that works.
target_compile_definitions(rekordbox_library_anonymizer_test PRIVATE
    SEABASS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(rekordbox_library_anonymizer_test PRIVATE seabass_core)
add_test(NAME rekordbox_library_anonymizer_test COMMAND rekordbox_library_anonymizer_test)

add_executable(rekordbox_cleanup_writer_test
    src/infrastructure/rekordbox/rekordbox_cleanup_writer.cpp
    tests/rekordbox_cleanup_writer_test.cpp
)
target_link_libraries(rekordbox_cleanup_writer_test PRIVATE seabass_core_testing)
target_include_directories(rekordbox_cleanup_writer_test PRIVATE src)
target_link_libraries(rekordbox_cleanup_writer_test PRIVATE rekordbox_format ZLIB::ZLIB)
add_test(NAME rekordbox_cleanup_writer_test COMMAND rekordbox_cleanup_writer_test)

# The four targets below build libdjinterop_engine_reader.cpp or
# libdjinterop_engine_anonymizer.cpp directly (not through seabass_core,
# which already gets sqlite3.h from the amalgamation source/include it
# adds for itself in the WIN32 branch above -- PRIVATE, so it does not
# reach these). Both files #include <sqlite3.h> straight, with nothing
# here to satisfy it except SQLite::SQLite3's own include path. That
# worked anyway under MinGW/MSYS2 purely by accident: GCC's ucrt64
# toolchain always searches its own prefix's include/ by default, so
# .../ucrt64/include/sqlite3.h resolved with no -I naming it. MSVC has
# no equivalent implicit search path, and fails outright without this.
add_executable(libdjinterop_engine_library_creator_test
    tests/libdjinterop_engine_library_creator_test.cpp
)
target_link_libraries(libdjinterop_engine_library_creator_test PRIVATE seabass_core_testing)
target_include_directories(libdjinterop_engine_library_creator_test PRIVATE src)
target_link_libraries(libdjinterop_engine_library_creator_test PRIVATE DjInterop SQLite::SQLite3)
add_test(NAME libdjinterop_engine_library_creator_test COMMAND libdjinterop_engine_library_creator_test)

add_executable(libdjinterop_engine_sync_scratch_replace_test
    tests/libdjinterop_engine_sync_scratch_replace_test.cpp
)
target_link_libraries(libdjinterop_engine_sync_scratch_replace_test PRIVATE seabass_core_testing)
target_include_directories(libdjinterop_engine_sync_scratch_replace_test PRIVATE src)
target_link_libraries(libdjinterop_engine_sync_scratch_replace_test PRIVATE DjInterop SQLite::SQLite3)
add_test(NAME libdjinterop_engine_sync_scratch_replace_test COMMAND libdjinterop_engine_sync_scratch_replace_test)

add_executable(libdjinterop_engine_loop_cue_test
    tests/libdjinterop_engine_loop_cue_test.cpp
)
target_link_libraries(libdjinterop_engine_loop_cue_test PRIVATE seabass_core_testing)
target_include_directories(libdjinterop_engine_loop_cue_test PRIVATE src)
target_link_libraries(libdjinterop_engine_loop_cue_test PRIVATE DjInterop SQLite::SQLite3)
add_test(NAME libdjinterop_engine_loop_cue_test COMMAND libdjinterop_engine_loop_cue_test)

add_executable(libdjinterop_engine_cleanup_writer_test
    tests/libdjinterop_engine_cleanup_writer_test.cpp
)
target_link_libraries(libdjinterop_engine_cleanup_writer_test PRIVATE seabass_core_testing)
target_include_directories(libdjinterop_engine_cleanup_writer_test PRIVATE src)
target_link_libraries(libdjinterop_engine_cleanup_writer_test PRIVATE DjInterop SQLite::SQLite3)
add_test(NAME libdjinterop_engine_cleanup_writer_test COMMAND libdjinterop_engine_cleanup_writer_test)

add_executable(duplicate_cleanup_interruption_test
    ${SEABASS_STICK_BACKUP_FORMAT_SOURCES}
    tests/duplicate_cleanup_interruption_test.cpp
)
target_link_libraries(duplicate_cleanup_interruption_test PRIVATE seabass_core_testing)
target_include_directories(duplicate_cleanup_interruption_test PRIVATE src)
target_link_libraries(duplicate_cleanup_interruption_test PRIVATE DjInterop ZLIB::ZLIB SQLite::SQLite3)
add_test(NAME duplicate_cleanup_interruption_test COMMAND duplicate_cleanup_interruption_test)

add_executable(libdjinterop_engine_anonymizer_test
    src/infrastructure/engine/libdjinterop_engine_anonymizer.cpp
    # The anonymizer counts a file as dropped only once removeEntry()
    # says it is gone, so this test links the real one rather than
    # asserting against a remove() that answers a different question.
    tests/libdjinterop_engine_anonymizer_test.cpp
)
target_link_libraries(libdjinterop_engine_anonymizer_test PRIVATE seabass_core_testing)
target_include_directories(libdjinterop_engine_anonymizer_test PRIVATE src)
target_link_libraries(libdjinterop_engine_anonymizer_test PRIVATE DjInterop SQLite::SQLite3)
add_test(NAME libdjinterop_engine_anonymizer_test COMMAND libdjinterop_engine_anonymizer_test)

add_executable(anonymize_library_test tests/anonymize_library_test.cpp)
target_include_directories(anonymize_library_test PRIVATE src)
target_link_libraries(anonymize_library_test PRIVATE seabass_core)
add_test(NAME anonymize_library_test COMMAND anonymize_library_test)

add_executable(libdjinterop_engine_cue_writer_propagate_test
    tests/libdjinterop_engine_cue_writer_propagate_test.cpp
)
target_link_libraries(libdjinterop_engine_cue_writer_propagate_test PRIVATE seabass_core_testing)
target_include_directories(libdjinterop_engine_cue_writer_propagate_test PRIVATE src)
target_link_libraries(libdjinterop_engine_cue_writer_propagate_test PRIVATE DjInterop)
add_test(NAME libdjinterop_engine_cue_writer_propagate_test COMMAND libdjinterop_engine_cue_writer_propagate_test)

add_executable(libdjinterop_engine_cue_writer_main_cue_test
    tests/libdjinterop_engine_cue_writer_main_cue_test.cpp
)
target_link_libraries(libdjinterop_engine_cue_writer_main_cue_test PRIVATE seabass_core_testing)
target_include_directories(libdjinterop_engine_cue_writer_main_cue_test PRIVATE src)
target_link_libraries(libdjinterop_engine_cue_writer_main_cue_test PRIVATE DjInterop SQLite::SQLite3)
add_test(NAME libdjinterop_engine_cue_writer_main_cue_test COMMAND libdjinterop_engine_cue_writer_main_cue_test)

add_executable(pending_deletion_manifest_test
    tests/pending_deletion_manifest_test.cpp
)
target_link_libraries(pending_deletion_manifest_test PRIVATE seabass_core_testing)
target_include_directories(pending_deletion_manifest_test PRIVATE src)
add_test(NAME pending_deletion_manifest_test COMMAND pending_deletion_manifest_test)

add_executable(fs_remove_test
    tests/fs_remove_test.cpp
)
target_link_libraries(fs_remove_test PRIVATE seabass_core_testing)
target_include_directories(fs_remove_test PRIVATE src tests)
add_test(NAME fs_remove_test COMMAND fs_remove_test)

add_executable(pending_deletion_applier_test
    src/infrastructure/cleanup/pending_deletion_applier.cpp
    tests/pending_deletion_applier_test.cpp
)
target_link_libraries(pending_deletion_applier_test PRIVATE seabass_core_testing)
target_include_directories(pending_deletion_applier_test PRIVATE src)
add_test(NAME pending_deletion_applier_test COMMAND pending_deletion_applier_test)

add_executable(zip_archive_writer_test
    src/infrastructure/zip_archive_writer.cpp
    # The reader too: case 5 is about what it does with an archive whose
    # offsets are damaged, which is the other half of this format.
    src/infrastructure/zip_archive_reader.cpp
    tests/zip_archive_writer_test.cpp
)
target_include_directories(zip_archive_writer_test PRIVATE src)
target_link_libraries(zip_archive_writer_test PRIVATE ZLIB::ZLIB)
add_test(NAME zip_archive_writer_test COMMAND zip_archive_writer_test)

# --- stick backup (docs/stick-backup-plan.md) ---

add_executable(sha256_test
    tests/sha256_test.cpp
)
target_link_libraries(sha256_test PRIVATE seabass_core_testing)
target_include_directories(sha256_test PRIVATE src)
add_test(NAME sha256_test COMMAND sha256_test)

# The icons the QML names are all bundled (see the script for why a
# missing one would otherwise go unnoticed). A script, not a target, so
# the -UNDEBUG check at the end of this file has nothing to say about it.
add_test(NAME breeze_icons_test
    COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/breeze_icons_test.cmake)

# Windows-only: WindowsRemovableMediaMonitor has no Linux equivalent
# target (see infrastructure/media/linux_udev_media_monitor.cpp for that
# one), and links against nothing but the Win32 API itself.
if(WIN32)
    add_executable(windows_removable_media_monitor_test
        src/infrastructure/media/windows_removable_media_monitor.cpp
        tests/windows_removable_media_monitor_test.cpp
    )
    target_include_directories(windows_removable_media_monitor_test PRIVATE src)
    add_test(NAME windows_removable_media_monitor_test COMMAND windows_removable_media_monitor_test)
endif()

add_executable(archive_file_test
    tests/archive_file_test.cpp
)
target_link_libraries(archive_file_test PRIVATE seabass_core_testing)
target_include_directories(archive_file_test PRIVATE src)
add_test(NAME archive_file_test COMMAND archive_file_test)


add_executable(backup_manifest_name_test
    ${SEABASS_STICK_BACKUP_FORMAT_SOURCES}
    tests/backup_manifest_name_test.cpp
)
target_include_directories(backup_manifest_name_test PRIVATE src)
target_link_libraries(backup_manifest_name_test PRIVATE ZLIB::ZLIB)
add_test(NAME backup_manifest_name_test COMMAND backup_manifest_name_test)

add_executable(backup_archive_roundtrip_test
    ${SEABASS_STICK_BACKUP_FORMAT_SOURCES}
    tests/backup_archive_roundtrip_test.cpp
)
target_include_directories(backup_archive_roundtrip_test PRIVATE src)
target_link_libraries(backup_archive_roundtrip_test PRIVATE ZLIB::ZLIB)
add_test(NAME backup_archive_roundtrip_test COMMAND backup_archive_roundtrip_test)

add_executable(backup_archive_deflate_test
    ${SEABASS_STICK_BACKUP_FORMAT_SOURCES}
    tests/backup_archive_deflate_test.cpp
)
target_include_directories(backup_archive_deflate_test PRIVATE src)
target_link_libraries(backup_archive_deflate_test PRIVATE ZLIB::ZLIB)
add_test(NAME backup_archive_deflate_test COMMAND backup_archive_deflate_test)

add_executable(backup_archive_crossvalidation_test
    ${SEABASS_STICK_BACKUP_FORMAT_SOURCES}
    tests/backup_archive_crossvalidation_test.cpp
)
target_include_directories(backup_archive_crossvalidation_test PRIVATE src)
target_link_libraries(backup_archive_crossvalidation_test PRIVATE ZLIB::ZLIB)
# Baked in as well as passed through the environment. The environment only
# reaches the test when ctest runs it; the definitions travel with the
# binary, so running it by hand exercises the same tools instead of
# skipping every case. They also let the test tell "no python here" apart
# from "python moved since this build was configured" -- one is a skip and
# the other is a failure.
target_compile_definitions(backup_archive_crossvalidation_test PRIVATE
    SEABASS_CONFIGURED_PYTHON3="${Python3_EXECUTABLE}"
    SEABASS_CONFIGURED_UNZIP="${UNZIP_EXECUTABLE}"
    SEABASS_CONFIGURED_SEVENZIP="${SEVENZIP_EXECUTABLE}"
)
add_executable(assertions_live_test tests/assertions_live_test.cpp)
target_include_directories(assertions_live_test PRIVATE src)
add_test(NAME assertions_live_test COMMAND assertions_live_test)

if(TARGET seabass_edit)
    add_executable(change_summary_wording_test tests/change_summary_wording_test.cpp)
    target_include_directories(change_summary_wording_test PRIVATE src)
    target_link_libraries(change_summary_wording_test PRIVATE seabass_edit seabass_core Qt6::Core)
    add_test(NAME change_summary_wording_test COMMAND change_summary_wording_test)
endif()

add_executable(path_key_test
    tests/path_key_test.cpp
)
target_link_libraries(path_key_test PRIVATE seabass_core_testing)
target_include_directories(path_key_test PRIVATE src)
add_test(NAME path_key_test COMMAND path_key_test)

add_executable(seabass_paths_test
    tests/seabass_paths_test.cpp
)
target_link_libraries(seabass_paths_test PRIVATE seabass_core_testing)
target_include_directories(seabass_paths_test PRIVATE src)
add_test(NAME seabass_paths_test COMMAND seabass_paths_test)

add_test(NAME backup_archive_crossvalidation_test COMMAND backup_archive_crossvalidation_test)
# No SKIP_RETURN_CODE: the three tools are required at configure time
# above, so there is no longer a legitimate reason for this test to do
# anything but run.
set_tests_properties(backup_archive_crossvalidation_test PROPERTIES
    ENVIRONMENT "SEABASS_PYTHON3=${Python3_EXECUTABLE};SEABASS_UNZIP=${UNZIP_EXECUTABLE};SEABASS_SEVENZIP=${SEVENZIP_EXECUTABLE}"
)

set(SEABASS_STICK_BACKUP_UPDATE_SOURCES
    ${SEABASS_STICK_BACKUP_FORMAT_SOURCES}
    src/infrastructure/stick_backup/archive_journal.cpp
    src/infrastructure/stick_backup/archive_updater.cpp
    src/infrastructure/stick_backup/archive_recovery.cpp
)

add_executable(backup_archive_incremental_update_test
    ${SEABASS_STICK_BACKUP_UPDATE_SOURCES}
    tests/backup_archive_incremental_update_test.cpp
)
target_include_directories(backup_archive_incremental_update_test PRIVATE src)
target_link_libraries(backup_archive_incremental_update_test PRIVATE ZLIB::ZLIB)
add_test(NAME backup_archive_incremental_update_test COMMAND backup_archive_incremental_update_test)

add_executable(backup_archive_truncation_fuzz_test
    ${SEABASS_STICK_BACKUP_UPDATE_SOURCES}
    tests/backup_archive_truncation_fuzz_test.cpp
)
target_include_directories(backup_archive_truncation_fuzz_test PRIVATE src)
target_link_libraries(backup_archive_truncation_fuzz_test PRIVATE ZLIB::ZLIB)
add_test(NAME backup_archive_truncation_fuzz_test COMMAND backup_archive_truncation_fuzz_test)

set(SEABASS_STICK_BACKUP_STICK_SOURCES
    ${SEABASS_STICK_BACKUP_UPDATE_SOURCES}
    src/infrastructure/long_paths.cpp
    src/infrastructure/fs_remove.cpp
    src/infrastructure/stick_backup/stick_tree_walker.cpp
    src/infrastructure/stick_backup/stat_diff.cpp
    src/infrastructure/stick_backup/sqlite_db_set.cpp
    src/infrastructure/stick_backup/library_catalog_mtime.cpp
    src/infrastructure/backup/stick_write_lock.cpp
)

add_executable(library_catalog_mtime_test
    src/infrastructure/stick_backup/library_catalog_mtime.cpp
    tests/library_catalog_mtime_test.cpp
)
target_link_libraries(library_catalog_mtime_test PRIVATE seabass_core_testing)
target_include_directories(library_catalog_mtime_test PRIVATE src)
add_test(NAME library_catalog_mtime_test COMMAND library_catalog_mtime_test)

add_executable(long_paths_test
    tests/long_paths_test.cpp
)
target_link_libraries(long_paths_test PRIVATE seabass_core_testing)
target_include_directories(long_paths_test PRIVATE src)
add_test(NAME long_paths_test COMMAND long_paths_test)

add_executable(stat_diff_test
    src/infrastructure/stick_backup/stat_diff.cpp
    tests/stat_diff_test.cpp
)
target_link_libraries(stat_diff_test PRIVATE seabass_core_testing)
target_include_directories(stat_diff_test PRIVATE src)
add_test(NAME stat_diff_test COMMAND stat_diff_test)

# FileEntrySource::readFailed() on every standard library: libc++ reports a
# refused read with the flags of a clean end of file (#36 follow-up).
add_executable(file_entry_source_test tests/file_entry_source_test.cpp)
target_link_libraries(file_entry_source_test PRIVATE seabass_core_testing)
target_include_directories(file_entry_source_test PRIVATE src)
add_test(NAME file_entry_source_test COMMAND file_entry_source_test)

add_executable(backup_database_capture_test
    ${SEABASS_STICK_BACKUP_STICK_SOURCES}
    tests/backup_database_capture_test.cpp
)
target_link_libraries(backup_database_capture_test PRIVATE seabass_core_testing)
target_include_directories(backup_database_capture_test PRIVATE src)
target_link_libraries(backup_database_capture_test PRIVATE ZLIB::ZLIB SQLite::SQLite3)
add_test(NAME backup_database_capture_test COMMAND backup_database_capture_test)

# Manage Backups over real archives: listing (this stick first, counts
# from the fingerprint) and deleting under the archive's own write lock.
add_executable(manage_stick_backups_test tests/manage_stick_backups_test.cpp)
target_include_directories(manage_stick_backups_test PRIVATE src)
target_link_libraries(manage_stick_backups_test PRIVATE seabass_core)
add_test(NAME manage_stick_backups_test COMMAND manage_stick_backups_test)

# The archive-path bug: a named backup has to be findable again.
add_executable(find_stick_archive_test tests/find_stick_archive_test.cpp)
target_include_directories(find_stick_archive_test PRIVATE src)
target_link_libraries(find_stick_archive_test PRIVATE seabass_core)
add_test(NAME find_stick_archive_test COMMAND find_stick_archive_test)

# Issue #1: My Tag names in an exportExt.pdb. Needs the source dir for
# tests/fixtures/exportExt.pdb, which tools/anonymize_export_ext produces
# from a real stick and the anonymizer now keeps instead of deleting.
add_executable(pdb_tag_names_test tests/pdb_tag_names_test.cpp)
target_include_directories(pdb_tag_names_test PRIVATE src)
target_link_libraries(pdb_tag_names_test PRIVATE seabass_core)
target_compile_definitions(pdb_tag_names_test PRIVATE SEABASS_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME pdb_tag_names_test COMMAND pdb_tag_names_test)

# Reads the committed fixture's real catalogs, so it needs the source dir
# and the two catalog readers -- a fingerprint over filler bytes would
# agree with itself whatever the code did.
add_executable(backup_fingerprint_describes_archive_test tests/backup_fingerprint_describes_archive_test.cpp)
target_include_directories(backup_fingerprint_describes_archive_test PRIVATE src)
target_link_libraries(backup_fingerprint_describes_archive_test PRIVATE seabass_core)
target_compile_definitions(backup_fingerprint_describes_archive_test PRIVATE SEABASS_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME backup_fingerprint_describes_archive_test COMMAND backup_fingerprint_describes_archive_test)
# "integration", with library_backup_roundtrip_test (127 s) and the other
# fixture-backed ones: this backs up 2725 real tracks, restores them and
# reads the catalogs on both sides, so it costs minutes rather than
# milliseconds. Deliberately not trimmed to fit the fast lane -- reading
# a SMALL library would not exercise the fingerprint, and reading the
# source stick instead of the restored archive would pass however wrong
# the header was. The label is what that cost is for: skipped on an
# ordinary push, run on a schedule and on every tag, which is where a
# backup's correctness has to hold.
set_tests_properties(backup_fingerprint_describes_archive_test PROPERTIES LABELS "integration")

add_executable(backup_stick_test
    ${SEABASS_STICK_BACKUP_STICK_SOURCES}
    tests/backup_stick_test.cpp
)
target_link_libraries(backup_stick_test PRIVATE seabass_core_testing)
target_include_directories(backup_stick_test PRIVATE src)
target_link_libraries(backup_stick_test PRIVATE ZLIB::ZLIB SQLite::SQLite3)
add_test(NAME backup_stick_test COMMAND backup_stick_test)

add_executable(backup_archive_concurrent_reader_test
    ${SEABASS_STICK_BACKUP_STICK_SOURCES}
    tests/backup_archive_concurrent_reader_test.cpp
)
target_link_libraries(backup_archive_concurrent_reader_test PRIVATE seabass_core_testing)
target_include_directories(backup_archive_concurrent_reader_test PRIVATE src)
target_link_libraries(backup_archive_concurrent_reader_test PRIVATE ZLIB::ZLIB SQLite::SQLite3 Threads::Threads)
add_test(NAME backup_archive_concurrent_reader_test COMMAND backup_archive_concurrent_reader_test)

add_executable(backup_archive_compaction_test
    ${SEABASS_STICK_BACKUP_UPDATE_SOURCES}
    tests/backup_archive_compaction_test.cpp
)
target_link_libraries(backup_archive_compaction_test PRIVATE seabass_core_testing)
target_include_directories(backup_archive_compaction_test PRIVATE src)
target_link_libraries(backup_archive_compaction_test PRIVATE ZLIB::ZLIB)
add_test(NAME backup_archive_compaction_test COMMAND backup_archive_compaction_test)

add_executable(backup_restore_test
    ${SEABASS_STICK_BACKUP_STICK_SOURCES}
    tests/backup_restore_test.cpp
)
target_link_libraries(backup_restore_test PRIVATE seabass_core_testing)
target_include_directories(backup_restore_test PRIVATE src)
target_link_libraries(backup_restore_test PRIVATE ZLIB::ZLIB SQLite::SQLite3)
add_test(NAME backup_restore_test COMMAND backup_restore_test)

add_executable(archive_write_lock_test
    ${SEABASS_STICK_BACKUP_STICK_SOURCES}
    tests/archive_write_lock_test.cpp
)
target_link_libraries(archive_write_lock_test PRIVATE seabass_core_testing)
target_include_directories(archive_write_lock_test PRIVATE src)
target_link_libraries(archive_write_lock_test PRIVATE ZLIB::ZLIB SQLite::SQLite3 Threads::Threads)
add_test(NAME archive_write_lock_test COMMAND archive_write_lock_test)

add_executable(clone_stick_test
    ${SEABASS_STICK_BACKUP_STICK_SOURCES}
    src/application/use_cases/clone_stick.cpp
    tests/clone_stick_test.cpp
)
target_link_libraries(clone_stick_test PRIVATE seabass_core_testing)
target_include_directories(clone_stick_test PRIVATE src)
target_link_libraries(clone_stick_test PRIVATE ZLIB::ZLIB SQLite::SQLite3)
add_test(NAME clone_stick_test COMMAND clone_stick_test)

add_executable(pending_deletion_resolver_test
    src/application/use_cases/find_unreferenced_files.cpp
    src/infrastructure/stick_layout.cpp
    src/infrastructure/cleanup/pending_deletion_resolver.cpp
    tests/pending_deletion_resolver_test.cpp
)
target_link_libraries(pending_deletion_resolver_test PRIVATE seabass_core_testing)
target_include_directories(pending_deletion_resolver_test PRIVATE src)
add_test(NAME pending_deletion_resolver_test COMMAND pending_deletion_resolver_test)

# Cross-platform now, talks to libsqlcipher via runtime LoadLibrary/
# dlopen, same as seabass_core's own onelibrary sources above. Needs
# libsqlcipher's runtime library actually present on whichever machine
# runs this test (see the target_sources block above for package names).
add_executable(onelibrary_cue_writer_test
    src/infrastructure/onelibrary/onelibrary_key.cpp
    src/infrastructure/onelibrary/sqlcipher_dyn.cpp
    src/infrastructure/onelibrary/onelibrary_cue_writer.cpp
    src/infrastructure/onelibrary/onelibrary_reader.cpp
    tests/onelibrary_cue_writer_test.cpp
)
target_link_libraries(onelibrary_cue_writer_test PRIVATE seabass_core_testing)
target_include_directories(onelibrary_cue_writer_test PRIVATE src)
target_link_libraries(onelibrary_cue_writer_test PRIVATE ZLIB::ZLIB)
if(NOT WIN32)
    target_link_libraries(onelibrary_cue_writer_test PRIVATE ${CMAKE_DL_LIBS})
endif()
add_test(NAME onelibrary_cue_writer_test COMMAND onelibrary_cue_writer_test)

add_executable(onelibrary_cue_writer_adapter_test
    src/infrastructure/onelibrary/onelibrary_key.cpp
    src/infrastructure/onelibrary/sqlcipher_dyn.cpp
    src/infrastructure/onelibrary/onelibrary_cue_writer.cpp
    src/infrastructure/onelibrary/onelibrary_reader.cpp
    tests/onelibrary_cue_writer_adapter_test.cpp
)
target_link_libraries(onelibrary_cue_writer_adapter_test PRIVATE seabass_core_testing)
target_include_directories(onelibrary_cue_writer_adapter_test PRIVATE src)
target_link_libraries(onelibrary_cue_writer_adapter_test PRIVATE ZLIB::ZLIB)
if(NOT WIN32)
    target_link_libraries(onelibrary_cue_writer_adapter_test PRIVATE ${CMAKE_DL_LIBS})
endif()
add_test(NAME onelibrary_cue_writer_adapter_test COMMAND onelibrary_cue_writer_adapter_test)

# Covers OneLibraryReader::readAll(), the read side (content/artist/key/
# image/playlist/cue joins), independent of the write-path fixture above.
add_executable(onelibrary_reader_test
    src/infrastructure/onelibrary/onelibrary_key.cpp
    src/infrastructure/onelibrary/sqlcipher_dyn.cpp
    src/infrastructure/onelibrary/onelibrary_cue_writer.cpp
    src/infrastructure/onelibrary/onelibrary_reader.cpp
    tests/onelibrary_reader_test.cpp
)
target_link_libraries(onelibrary_reader_test PRIVATE seabass_core_testing)
target_include_directories(onelibrary_reader_test PRIVATE src)
target_link_libraries(onelibrary_reader_test PRIVATE ZLIB::ZLIB)
if(NOT WIN32)
    target_link_libraries(onelibrary_reader_test PRIVATE ${CMAKE_DL_LIBS})
endif()
add_test(NAME onelibrary_reader_test COMMAND onelibrary_reader_test)

# --- Integration suite: the real use cases against whole real libraries ---
# --- -- slower, and tagged with the CTest LABEL "integration" so they ---
# --- can be selected (`ctest -L integration`) or excluded (`ctest -LE ---
# --- integration`). A label excludes nothing on its own: a bare ---
# --- `ctest` runs these too, and is the run that counts. See ---
# --- docs/testing.md and docs/real-data-testing.md. Links the whole ---
# --- seabass_core (unlike every test above, which links just the ---
# --- handful of .cpp files it needs) since this genuinely exercises ---
# --- most of the real app's read/write/sync/stats/consistency ---
# --- machinery together, the same way seabass-cli itself does. ---

# The corpus runner: the committed anonymized fixture plus every set under
# $SEABASS_CORPUS (real, un-anonymized libraries extracted with
# tools/extract_testdata -- see docs/real-data-testing.md). Qt-free, runs
# from local disk, asserts read-backs and work counts rather than seconds.
# Each set records its own expected counts in SET-EXPECTATIONS.txt beside
# it, so a real library nobody inspects by hand still gets a regression
# guard on what the readers return.
# Checks an anonymized export against the promise its manifest makes.
# The export flow calls the same function before it writes the zip; this
# is for checking an export somebody already produced.
# Copies a real stick's library metadata into the local corpus directory,
# verbatim and NOT anonymized. C++ rather than a shell script because the
# same program has to run on Windows and eventually macOS.
add_executable(extract_testdata tools/extract_testdata.cpp)
target_include_directories(extract_testdata PRIVATE src)
target_link_libraries(extract_testdata PRIVATE seabass_core)

add_executable(verify_anonymized_export tools/verify_anonymized_export.cpp)
target_include_directories(verify_anonymized_export PRIVATE src)
target_link_libraries(verify_anonymized_export PRIVATE seabass_core)

# Release rig: restore a reference full stick backup onto a test stick and
# prove it arrived -- nothing left to restore, and every catalog file matches
# the backup's manifest checksums. Reads only, apart from the restore itself.
add_executable(rig_restore tools/rig_restore.cpp)
target_include_directories(rig_restore PRIVATE src)
target_link_libraries(rig_restore PRIVATE seabass_core)

# Release rig: the read-only paths on a restored test stick -- catalog counts,
# Library Health, stray cues and duplicates, timed -- without writing to it.
add_executable(rig_read tools/rig_read.cpp)
target_include_directories(rig_read PRIVATE src)
target_link_libraries(rig_read PRIVATE seabass_core)

# X3: plants a directory where a track's file should be, and asks whether
# the app tells anyone. Needs the catalog readers, like rig_read.
add_executable(rig_file_failure tools/rig_file_failure.cpp)
target_include_directories(rig_file_failure PRIVATE src)
target_link_libraries(rig_file_failure PRIVATE seabass_core)

# Issue #1: rewrites the My Tag names in an exportExt.pdb in place, so a
# real one can be kept as a fixture instead of deleted. Separate from the
# anonymizer so a fixture can be regenerated from a stick on its own.
add_executable(anonymize_export_ext tools/anonymize_export_ext.cpp)
target_include_directories(anonymize_export_ext PRIVATE src)
target_link_libraries(anonymize_export_ext PRIVATE seabass_core)

# Create Engine Library, headless: build a candidate library from a stick's
# rekordbox export into a folder, at a chosen schema generation, and read it
# back. The feature is otherwise GUI-only and needs real hardware to judge.
add_executable(make_engine_library tools/make_engine_library.cpp)
target_include_directories(make_engine_library PRIVATE src)
# On Windows seabass_core's own sqlite3 include is PRIVATE (see the
# WIN32 branch above), so a direct #include <sqlite3.h> here needs its
# own link to the PUBLIC SQLite::SQLite3 alias; elsewhere seabass_core
# already exposes it PUBLIC, so this is a harmless no-op there.
target_link_libraries(make_engine_library PRIVATE seabass_core SQLite::SQLite3)

# Release rig: a full stick backup as the app takes it, then verified, checked
# for holes, previewed for leftovers and compared with the stick's catalogs.
add_executable(rig_backup tools/rig_backup.cpp)
target_include_directories(rig_backup PRIVATE src)
target_link_libraries(rig_backup PRIVATE seabass_core)

# Release rig: the emergency copy of a damaged, read-only stick and its restore
# (issue #36). Needs a stick that is really damaged; see the tool's header.
add_executable(rig_salvage tools/rig_salvage.cpp)
target_include_directories(rig_salvage PRIVATE src)
target_link_libraries(rig_salvage PRIVATE seabass_core)

# Release rig: compact a full stick backup; it must shrink by exactly the dead
# space it reported and still verify with the same entries.
# Release rig: the filesystem repair, end to end, on a FAT32 loopback image
# this tool damages itself -- no stick, no root. Not built on Windows: see
# the file's own header.
if(NOT WIN32)
    add_executable(rig_fs_repair tools/rig_fs_repair.cpp)
    target_include_directories(rig_fs_repair PRIVATE src)
    target_link_libraries(rig_fs_repair PRIVATE seabass_core)
endif()

# D1: formats a real stick, so it is built everywhere the rig runs but
# only ever erases anything when handed --execute.
# The rig half of issue #42: reads only, so it is built and run
# everywhere, and it is what makes a save that re-arms the player's
# import prompt fail a round instead of surfacing on a player weeks later.
add_executable(rig_import_prompt tools/rig_import_prompt.cpp)
target_include_directories(rig_import_prompt PRIVATE src)
target_link_libraries(rig_import_prompt PRIVATE seabass_core)

add_executable(rig_format tools/rig_format.cpp)
target_include_directories(rig_format PRIVATE src)
target_link_libraries(rig_format PRIVATE seabass_core)

add_executable(rig_compact tools/rig_compact.cpp)
target_include_directories(rig_compact PRIVATE src)
target_link_libraries(rig_compact PRIVATE seabass_core)

# Release rig: move one copy of a duplicate aside so Library Health
# has something to repair, and put it back.
add_executable(rig_plant_repairable tools/rig_plant_repairable.cpp)
target_include_directories(rig_plant_repairable PRIVATE src)
target_link_libraries(rig_plant_repairable PRIVATE seabass_core)

# Release rig: every save left an automatic backup on the stick, and each
# one is restorable.
add_executable(rig_save_backups tools/rig_save_backups.cpp)
target_include_directories(rig_save_backups PRIVATE src)
target_link_libraries(rig_save_backups PRIVATE seabass_core)

# A process the DJ-software guard recognises, for the rig's refusal checks
# (see tools/rig_fake_dj.cpp): named for what the detector matches.
add_executable(rig_fake_dj tools/rig_fake_dj.cpp)
set_target_properties(rig_fake_dj PROPERTIES OUTPUT_NAME rekordbox)

# Release rig: Manage Backups deletes one test archive -- refused while
# another process is writing it, archive and journal gone afterwards.
add_executable(rig_delete_backup tools/rig_delete_backup.cpp)
target_include_directories(rig_delete_backup PRIVATE src)
target_link_libraries(rig_delete_backup PRIVATE seabass_core)

# Release rig: Create / Update Backup USB Stick from one stick onto another,
# and what the stick list advises for each mounted stick.
add_executable(rig_clone tools/rig_clone.cpp)
target_include_directories(rig_clone PRIVATE src)
target_link_libraries(rig_clone PRIVATE seabass_core)
add_executable(rig_advise tools/rig_advise.cpp)
target_include_directories(rig_advise PRIVATE src)
target_link_libraries(rig_advise PRIVATE seabass_core)
# Two different things, and the names now say which is which. The tool
# run proves the committed fixture is clean; it cannot prove the verifier
# would refuse anything, because the fixture never leaks.
#
# WILL_FAIL, and not because the check is wrong. The fixture was exported
# by Seabass 0.6, whose anonymizer overwrote rows without compacting the
# files afterwards, so the pre-scrub values are still legible in the free
# space of engine/Database2/m.db (52 real artist names) and
# rekordbox/export.pdb (403). The byte sweep added to the verifier finds
# them; the reader-based checks never could. Regenerating the fixture from
# the originating USB sticks is the fix. When that lands this test starts
# failing *because it passed*, which is the reminder to delete this
# property and the matching baseline list in anonymization_verifier_test.
add_test(NAME anonymized_fixture_is_clean_test COMMAND verify_anonymized_export
    ${CMAKE_SOURCE_DIR}/tests/fixtures/anonymized_library)
set_tests_properties(anonymized_fixture_is_clean_test PROPERTIES LABELS "integration" WILL_FAIL TRUE)

# Plants a specific leak per case and asserts the verifier catches it.
add_executable(onelibrary_anonymizer_test tests/onelibrary_anonymizer_test.cpp)
target_include_directories(onelibrary_anonymizer_test PRIVATE src)
target_link_libraries(onelibrary_anonymizer_test PRIVATE seabass_core)
add_test(NAME onelibrary_anonymizer_test COMMAND onelibrary_anonymizer_test
    ${CMAKE_SOURCE_DIR}/tests/fixtures/anonymized_library/rekordbox/rekordbox/exportLibrary.db)
set_tests_properties(onelibrary_anonymizer_test PROPERTIES LABELS "integration")

add_executable(anonymization_verifier_test tests/anonymization_verifier_test.cpp)
target_include_directories(anonymization_verifier_test PRIVATE src)
target_link_libraries(anonymization_verifier_test PRIVATE seabass_core)
add_test(NAME anonymization_verifier_test COMMAND anonymization_verifier_test
    ${CMAKE_SOURCE_DIR}/tests/fixtures/anonymized_library)
set_tests_properties(anonymization_verifier_test PROPERTIES LABELS "integration")

# An Engine row with no relative path names no file, not the Engine Library
# folder -- see the test's own header.
# An Engine track with nothing cued must read back with no cues: -1 is
# "not set", not a position. See the test's own header.
add_executable(engine_reader_unset_cue_test tests/engine_reader_unset_cue_test.cpp)
target_include_directories(engine_reader_unset_cue_test PRIVATE src tests)
target_link_libraries(engine_reader_unset_cue_test PRIVATE seabass_core DjInterop SQLite::SQLite3)
add_test(NAME engine_reader_unset_cue_test COMMAND engine_reader_unset_cue_test)

add_executable(engine_reader_paths_test tests/engine_reader_paths_test.cpp)
target_include_directories(engine_reader_paths_test PRIVATE src)
target_compile_definitions(engine_reader_paths_test PRIVATE SEABASS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(engine_reader_paths_test PRIVATE seabass_core SQLite::SQLite3)
add_test(NAME engine_reader_paths_test COMMAND engine_reader_paths_test)

# Browsing a stick backup in place: cues read through the archive must
# match cues read off the folder, byte for byte, with nothing extracted.
# Real fixture data, both compression methods -- see the test's own header.
add_executable(archive_anlz_source_test tests/archive_anlz_source_test.cpp)
target_include_directories(archive_anlz_source_test PRIVATE src)
target_link_libraries(archive_anlz_source_test PRIVATE seabass_core)
add_test(NAME archive_anlz_source_test COMMAND archive_anlz_source_test
    ${CMAKE_SOURCE_DIR}/tests/fixtures/anonymized_library)
set_tests_properties(archive_anlz_source_test PROPERTIES LABELS "integration")

# End to end: back a library up, open the backup, read it. Asserts the
# cues match the original folder even though USBANLZ was never extracted.
add_executable(backup_artwork_lookup_test tests/backup_artwork_lookup_test.cpp)
target_include_directories(backup_artwork_lookup_test PRIVATE src tests)
target_link_libraries(backup_artwork_lookup_test PRIVATE seabass_core SQLite::SQLite3)
add_test(NAME backup_artwork_lookup_test COMMAND backup_artwork_lookup_test)

add_executable(open_stick_backup_test tests/open_stick_backup_test.cpp)
target_include_directories(open_stick_backup_test PRIVATE src)
target_link_libraries(open_stick_backup_test PRIVATE seabass_core)
add_test(NAME open_stick_backup_test COMMAND open_stick_backup_test
    ${CMAKE_SOURCE_DIR}/tests/fixtures/anonymized_library)
set_tests_properties(open_stick_backup_test PROPERTIES LABELS "integration")

add_executable(corpus_test tests/corpus_test.cpp tests/engine_edit_times.cpp)
target_include_directories(corpus_test PRIVATE src)
# engine_edit_times.cpp #includes <sqlite3.h> directly, which needs the
# PUBLIC SQLite::SQLite3 alias on MSVC -- see make_engine_library's
# comment above for why seabass_core alone isn't enough there.
target_link_libraries(corpus_test PRIVATE seabass_core SQLite::SQLite3)
# The matrix cases stage real edits through the real save loop, which
# lives in seabass_edit and so needs Qt Core. The reader, writer and
# work-count cases do not, and still run on a machine without Qt -- the
# same reason the CLI is built there and the GUI is not.
if(TARGET seabass_edit)
    target_link_libraries(corpus_test PRIVATE seabass_edit Qt6::Core)
    target_compile_definitions(corpus_test PRIVATE SEABASS_CORPUS_HAS_EDIT)
endif()
add_test(NAME corpus_test COMMAND corpus_test WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(corpus_test PROPERTIES LABELS "integration")

# Issue #21: the anonymized library, as a stick, through a full backup and
# an exact restore, the far end compared with the near end.
add_executable(library_backup_roundtrip_test tests/library_backup_roundtrip_test.cpp)
target_include_directories(library_backup_roundtrip_test PRIVATE src)
# stick_fixture.hpp #includes <sqlite3.h> directly, which needs the
# PUBLIC SQLite::SQLite3 alias on Windows -- see make_engine_library's
# comment above for why seabass_core alone isn't enough there.
target_link_libraries(library_backup_roundtrip_test PRIVATE seabass_core SQLite::SQLite3)
add_test(NAME library_backup_roundtrip_test COMMAND library_backup_roundtrip_test WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(library_backup_roundtrip_test PROPERTIES LABELS "integration")


# ---------------------------------------------------------------------------
# Keep assertions alive in test binaries, whatever the build type.
#
# 87 of the 88 test files are built out of assert() -- 2108 of them, none
# guarded. Three of CMake's four standard build types (Release,
# RelWithDebInfo, MinSizeRel) put -DNDEBUG in CMAKE_CXX_FLAGS_<CONFIG>,
# which turns every one of those into a no-op. The suite then passes
# without checking anything, and passes *faster*, so nothing looks wrong.
#
# Verified rather than assumed, both halves: a Release configure of this
# project really does put -DNDEBUG on the test targets, and
# `assert(1 == 2)` compiled with it exits 0.
#
# Target options are emitted after CMAKE_CXX_FLAGS_<CONFIG>, so -UNDEBUG
# here wins over the -DNDEBUG the build type added. Applied by name so a
# test added later is covered without anyone remembering this exists; the
# application keeps its NDEBUG, since only the tests depend on assert.
# ---------------------------------------------------------------------------
# MSVC test executables can't find their own DLLs at process launch,
# unlike MinGW/MSYS2 builds where the same shell's PATH already carries
# them -- confirmed directly: every Qt-linked test (seabass_qml_tests,
# open_folder_test, edit_session_save_loop_test, ...) exits immediately
# with 0xc0000135 (STATUS_DLL_NOT_FOUND) and no output under MSVC/ninja,
# because C:/Qt/<ver>/msvc2022_64/bin is never on PATH for a test
# process, only for whatever shell the developer happened to configure
# from. The four onelibrary_* tests fail the same way for a second,
# unrelated DLL: SqlCipherLibrary's constructor (sqlcipher_dyn.hpp)
# LoadLibrary()s libsqlcipher-0.dll at runtime, which this project never
# builds or vendors -- only MSYS2's UCRT64 environment ships it -- so an
# uncaught std::runtime_error from that constructor aborts the process
# before it prints anything.
#
# Derived from the already-found Qt6::Core target rather than
# CMAKE_PREFIX_PATH directly, since IMPORTED_LOCATION is the one property
# guaranteed to point at the actual DLL regardless of which config
# (Release/Debug) got resolved.
set(_seabass_test_path_dirs "")
if(MSVC AND TARGET Qt6::Core)
    foreach(_seabass_qt_loc_prop IMPORTED_LOCATION IMPORTED_LOCATION_RELEASE IMPORTED_LOCATION_DEBUG
            IMPORTED_LOCATION_RELWITHDEBINFO IMPORTED_LOCATION_MINSIZEREL)
        get_target_property(_seabass_qt_core_loc Qt6::Core ${_seabass_qt_loc_prop})
        if(_seabass_qt_core_loc)
            get_filename_component(_seabass_qt_bin_dir "${_seabass_qt_core_loc}" DIRECTORY)
            list(APPEND _seabass_test_path_dirs "${_seabass_qt_bin_dir}")
            break()
        endif()
    endforeach()
    # Not derivable from any CMake target: sqlcipher is never linked,
    # only ever dlopen()'d at runtime (see sqlcipher_dyn.hpp), so no
    # find_package/imported target exists to introspect. Matches
    # tools/deploy-windows.ps1's own $UcrtBin default for the same DLL.
    set(SEABASS_SQLCIPHER_BIN_DIR "C:/msys64/ucrt64/bin" CACHE PATH
        "Directory containing libsqlcipher-0.dll, for MSVC test runs")
    if(EXISTS "${SEABASS_SQLCIPHER_BIN_DIR}/libsqlcipher-0.dll")
        list(APPEND _seabass_test_path_dirs "${SEABASS_SQLCIPHER_BIN_DIR}")
    endif()
endif()
if(_seabass_test_path_dirs)
    list(JOIN _seabass_test_path_dirs ";" _seabass_test_path_prepend)
    # A literal ";" inside one list element must be escaped, or CMake's
    # own list(APPEND ...) below silently splits this single PATH value
    # back into separate (nonsensical) environment-list entries -- PATH
    # entries and CMake list items share the same separator character.
    string(REPLACE ";" "\\;" _seabass_test_path_prepend "${_seabass_test_path_prepend};$ENV{PATH}")
endif()

# No test may write into the user's real ~/Seabass.
#
# Since the QML tests build the real controllers, a test run created
# ~/Seabass/backups/full/TESTSTICK.zip and ~/Seabass/metadata/cues.db in
# the developer's own tree -- and, sharing one cues.db between parallel
# tests, made the suite intermittently fail under `ctest -j`.
#
# SEABASS_HOME (see infrastructure/paths/seabass_paths.hpp) redirects the
# local root, and it is applied to every registered test rather than to
# the few that were noticed: any test can reach a default path through a
# controller it merely constructs. Appended rather than assigned, because
# some tests already carry an ENVIRONMENT property.
# And no test's scratch space may sit in RAM. Tests build their scratch
# trees under the temp directory (tests/scratch_path.hpp), and /tmp is a
# tmpfs here: a few suites' worth of leftover fixture sticks and archives
# held 7 GB of memory that nothing would ever evict, and got builds
# OOM-killed. A directory under $HOME is on disk, where the page cache can
# drop it. POSIX only: TMPDIR is what temp_directory_path(), QTemporaryDir
# and mkstemp all read there; Windows' %TEMP% is on disk already.
#
# Created by a setup fixture rather than once at configure time, because
# libstdc++'s temp_directory_path() throws when the directory is missing,
# and deleting it between runs must not turn the whole suite red.
if(NOT WIN32 AND DEFINED ENV{HOME})
    set(SEABASS_TEST_TMPDIR "$ENV{HOME}/tmp/seabass-tests" CACHE PATH
        "Where tests keep their scratch trees (TMPDIR); on disk, not tmpfs")
    add_test(NAME seabass_test_tmpdir COMMAND ${CMAKE_COMMAND} -E make_directory "${SEABASS_TEST_TMPDIR}")
    set_tests_properties(seabass_test_tmpdir PROPERTIES FIXTURES_SETUP seabass_test_tmpdir)
endif()

get_property(_seabass_all_tests DIRECTORY PROPERTY TESTS)
foreach(_seabass_test IN LISTS _seabass_all_tests)
    get_test_property(${_seabass_test} ENVIRONMENT _seabass_test_env)
    if(_seabass_test_env STREQUAL "NOTFOUND")
        set(_seabass_test_env "")
    endif()
    list(APPEND _seabass_test_env "SEABASS_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home/${_seabass_test}")
    # And the settings store, which SEABASS_HOME does not cover: QSettings
    # resolves through the platform's own config location, so a test that
    # merely constructs a controller wrote into the developer's real
    # ~/.config/seabass/seabass.conf. That is not hypothetical -- it left
    # 232 dead opened-folder rows in one, every one of them a card on the
    # app's first page. Set here as well as in the tests that know they
    # persist something, because "merely constructs a controller" is most
    # of them.
    #
    # This covers the platforms whose settings store is a file under a
    # location these variables name. It does NOT cover Windows, where the
    # native store is the registry: a test that persists anything has to
    # force QSettings::IniFormat in-process as well (open_folder_test and
    # tests/qml/qml_test_main.cpp both do).
    #
    # Skipped where the test already names its own, rather than appending
    # a second value and leaving which one wins to CTest's ordering --
    # seabass_qml_tests sets XDG_CONFIG_HOME above.
    if(NOT _seabass_test_env MATCHES "XDG_CONFIG_HOME=")
        list(APPEND _seabass_test_env
            "XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home/${_seabass_test}/config")
    endif()
    if(NOT _seabass_test_env MATCHES "APPDATA=")
        list(APPEND _seabass_test_env
            "APPDATA=${CMAKE_CURRENT_BINARY_DIR}/test-home/${_seabass_test}/config")
    endif()
    if(SEABASS_TEST_TMPDIR AND NOT _seabass_test STREQUAL "seabass_test_tmpdir"
       AND NOT _seabass_test_env MATCHES "TMPDIR=")
        list(APPEND _seabass_test_env "TMPDIR=${SEABASS_TEST_TMPDIR}")
        set_property(TEST ${_seabass_test} APPEND PROPERTY FIXTURES_REQUIRED seabass_test_tmpdir)
    endif()
    if(_seabass_test_path_prepend AND NOT _seabass_test_env MATCHES "PATH=")
        list(APPEND _seabass_test_env "PATH=${_seabass_test_path_prepend}")
    endif()
    set_tests_properties(${_seabass_test} PROPERTIES ENVIRONMENT "${_seabass_test_env}")
endforeach()

get_property(_seabass_all_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
set(_seabass_flagged_targets "")
foreach(_seabass_target IN LISTS _seabass_all_targets)
    if(_seabass_target MATCHES "_test$|_tests$")
        target_compile_options(${_seabass_target} PRIVATE
            "$<IF:$<CXX_COMPILER_ID:MSVC>,/UNDEBUG,-UNDEBUG>"
            # See tests/msvc_assert_no_dialog.hpp: without this, a tripped
            # assert() blocks forever on a modal dialog instead of failing.
            "$<$<CXX_COMPILER_ID:MSVC>:/FI${CMAKE_SOURCE_DIR}/tests/msvc_assert_no_dialog.hpp>")
        list(APPEND _seabass_flagged_targets ${_seabass_target})
    endif()
endforeach()

# The loop above matches on NAME, so a test registered under a name that
# does not end in _test/_tests silently gets no flag and every assert()
# inside it dies in a Release build -- the exact failure this is here to
# prevent, reintroduced by nothing more than a naming choice.
#
# So check the other direction: every target an add_test() actually
# points at must be one the loop flagged. This is not the loop grading
# its own homework -- the two sets are built from different properties
# (BUILDSYSTEM_TARGETS filtered by name, against the COMMAND of each
# registered test), and it is the mismatch between them that is the bug.
#
# tests/assertions_live_test.cpp covers the other half: that the flag,
# once applied, really does keep NDEBUG out of the translation unit.
get_property(_seabass_registered_tests DIRECTORY PROPERTY TESTS)
foreach(_seabass_test IN LISTS _seabass_registered_tests)
    # Matched by name because that is how this project registers tests --
    # add_test(NAME x COMMAND x) for every one of them. The test's COMMAND
    # property is deliberately not used: it reads back as NOTFOUND here,
    # which silently made an earlier version of this check pass over
    # everything. Watched it fail before trusting it this time.
    if(TARGET ${_seabass_test} AND NOT ${_seabass_test} IN_LIST _seabass_flagged_targets)
        message(FATAL_ERROR
            "Test target '${_seabass_test}' did not receive -UNDEBUG, because its name does "
            "not end in _test or _tests. Every assert() in it would be a no-op in a "
            "Release/RelWithDebInfo/MinSizeRel build, and it would pass while checking "
            "nothing. Rename the target, or widen the match above.")
    endif()
endforeach()

endif()  # SEABASS_TESTS
