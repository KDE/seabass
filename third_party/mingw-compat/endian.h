// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

// Shim for mingw-w64, which has no <endian.h> (that's Linux/BSD-specific).
// Only on this target's include path when cross-compiling for Windows --
// see the WIN32 branch in the top-level CMakeLists.txt that adds
// third_party/mingw-compat to kaitai_cpp_stl_runtime's include path.
// kaitaistream.cpp's own platform-detection preamble already has a
// dedicated _MSC_VER branch that doesn't need this at all (real MSVC has
// no <endian.h> either) -- this shim exists purely because mingw-w64's
// GCC doesn't define _MSC_VER, so it falls through to the
// Linux/BSD-only #include <endian.h> branch instead. Every Seabass
// target this project builds only ever targets x86_64, which is always
// little-endian, so that's hardcoded here rather than detected.
#define __LITTLE_ENDIAN 1234
#define __BIG_ENDIAN 4321
#define __BYTE_ORDER __LITTLE_ENDIAN
