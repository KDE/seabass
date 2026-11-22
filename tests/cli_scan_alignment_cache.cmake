# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

# `seabass-cli scan` reads the stick's alignment cache and writes nothing on
# the stick, cache included.
#
# A scratch stick: the duplicate Engine fixture (two copies of one song, a
# hot cue on one) with two real files under it, copies of one signal 21 ms
# apart (tests/fixtures/alignment). Then:
#
#   1. no cache on the stick: the scan lines the copies up by decoding
#      them, and leaves every file on the stick as it was, with no new one;
#   2. a cache line saying the two files are the same bytes (they are
#      not): the scan lists the copies 0 ms apart, which only the cache
#      can have told it, and still leaves the stick as it was.
#
#   cmake -DCLI=<seabass-cli> -DENGINE=<duplicate_engine_library>
#         -DALIGNMENT=<tests/fixtures/alignment> -DSCRATCH=<dir>
#         -DMETHOD=<CachedAudioAlignmentProbe::MethodVersion> -P cli_scan_alignment_cache.cmake

foreach(var CLI ENGINE ALIGNMENT SCRATCH METHOD)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "${var} not given")
    endif()
endforeach()

file(REMOVE_RECURSE "${SCRATCH}")
file(MAKE_DIRECTORY "${SCRATCH}/folderA" "${SCRATCH}/folderB")
file(COPY "${ENGINE}/" DESTINATION "${SCRATCH}/Engine Library")
file(COPY_FILE "${ALIGNMENT}/a-ref-B.mp3" "${SCRATCH}/folderA/Duplicate Song.mp3")
file(COPY_FILE "${ALIGNMENT}/a-lead21-D.mp3" "${SCRATCH}/folderB/Duplicate Song.mp3")

# Every file under the stick and its hash.
function(snapshot out)
    file(GLOB_RECURSE files LIST_DIRECTORIES false RELATIVE "${SCRATCH}" "${SCRATCH}/*")
    list(SORT files)
    set(lines "")
    foreach(f IN LISTS files)
        file(SHA256 "${SCRATCH}/${f}" hash)
        string(APPEND lines "${f} ${hash}\n")
    endforeach()
    set(${out} "${lines}" PARENT_SCOPE)
endfunction()

function(scan out)
    execute_process(COMMAND "${CLI}" scan --verbose --engine "${SCRATCH}/Engine Library"
                    RESULT_VARIABLE rc OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "scan exited ${rc}:\n${stdout}\n${stderr}")
    endif()
    set(${out} "${stdout}" PARENT_SCOPE)
endfunction()

# 1. No cache: decoded, lined up, nothing written.
snapshot(before)
scan(output)
snapshot(after)
if(NOT before STREQUAL after)
    message(FATAL_ERROR "the scan changed the stick:\nbefore:\n${before}\nafter:\n${after}")
endif()
if(NOT output MATCHES "shift=2[0-9.]*ms")
    message(FATAL_ERROR "the copies were not lined up by decoding them:\n${output}")
endif()

# 2. A cache line, with each file's size and mtime as the cache stores
#    them (Unix seconds), saying the two are the same bytes.
foreach(side A B)
    file(SIZE "${SCRATCH}/folder${side}/Duplicate Song.mp3" size${side})
    file(TIMESTAMP "${SCRATCH}/folder${side}/Duplicate Song.mp3" mtime${side} "%s" UTC)
endforeach()
file(WRITE "${SCRATCH}/Seabass/caches/alignment.jsonl"
     "{\"a\":\"folderA/Duplicate Song.mp3\",\"amtime\":\"${mtimeA}\",\"asize\":\"${sizeA}\","
     "\"b\":\"folderB/Duplicate Song.mp3\",\"bmtime\":\"${mtimeB}\",\"bsize\":\"${sizeB}\","
     "\"decoder\":\"bytes\",\"method\":\"${METHOD}\",\"result\":\"same\"}\n")
snapshot(before)
scan(output)
snapshot(after)
if(NOT before STREQUAL after)
    message(FATAL_ERROR "the scan changed the stick:\nbefore:\n${before}\nafter:\n${after}")
endif()
if(NOT output MATCHES "shift=0ms")
    message(FATAL_ERROR "the scan did not read the stick's cache:\n${output}")
endif()

file(REMOVE_RECURSE "${SCRATCH}")
message(STATUS "cli_scan_alignment_cache: the scan read the cache and wrote nothing")
