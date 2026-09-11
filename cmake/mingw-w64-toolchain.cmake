# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: BSD-2-Clause

# Cross-compile Seabass (CLI and, when Qt6-for-mingw is available via
# CMAKE_PREFIX_PATH + QT_HOST_PATH, the GUI too) for 64-bit Windows using
# mingw-w64.
#
# Usage (CLI only):
#   cmake -S . -B build-mingw \
#       -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-toolchain.cmake \
#       -DCMAKE_PREFIX_PATH=<path to an installed vendored zlib prefix -- see third_party/zlib/README.md>
#   cmake --build build-mingw --target seabass-cli
#
# Usage (GUI too, once Qt6 win64_mingw + a version-matched Linux host Qt
# are installed -- see docs/windows-build.md):
#   cmake -S . -B build-mingw \
#       -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-toolchain.cmake \
#       -DCMAKE_PREFIX_PATH="<zlib prefix>;/home/sebas/Qt6-mingw/6.8.0/mingw_64" \
#       -DQT_HOST_PATH=/home/sebas/Qt6-linux-host/6.8.0/gcc_64
#   cmake --build build-mingw --target seabass

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# Qt's official win64_mingw prebuilt binaries are built against the
# posix-threading mingw-w64 C++ runtime, not Ubuntu's default
# update-alternatives selection (win32-threading) -- must pin the
# -posix suffixed compiler pair explicitly or linking against a
# prebuilt Qt install mismatches/fails.
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc-posix)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++-posix)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)

# CMAKE_PREFIX_PATH entries (the vendored zlib prefix, the mingw Qt6
# install, ...) are appended to CMAKE_FIND_ROOT_PATH itself rather than
# switching the *_MODE settings below to BOTH. BOTH lets find_path/
# find_library/find_package fall through to plain host system
# directories (e.g. /usr/include) whenever a package isn't found under
# the root path -- which actually happened: some Qt6 component's CMake
# config internally does its own find_package() (Threads/OpenSSL-ish),
# and under BOTH that resolved to the host's /usr/include, getting
# injected as an -isystem flag on every mingw compile and breaking the
# cross-compile (host glibc headers shadowing the mingw sysroot's).
# Folding the vendored prefixes into CMAKE_FIND_ROOT_PATH and keeping
# ONLY avoids that: every find_* stays confined to the mingw sysroot
# plus these explicitly vendored/downloaded prefixes, never the host.
set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32 ${CMAKE_PREFIX_PATH})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
