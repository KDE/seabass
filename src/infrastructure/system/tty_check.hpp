// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

// isatty()/STDIN_FILENO/STDOUT_FILENO: mingw-w64 provides <unistd.h> as a
// POSIX-compatibility shim; MSVC has no POSIX layer at all, not even that
// shim, and declares the same functionality as non-standard extensions in
// <io.h>, spelled with a leading underscore (_isatty, _fileno).
#ifdef _MSC_VER
#include <io.h>
#include <cstdio>
#else
#include <unistd.h>
#endif

namespace seabass::infrastructure::system
{

inline bool isStdoutTty()
{
#ifdef _MSC_VER
    return ::_isatty(::_fileno(stdout)) != 0;
#else
    return ::isatty(STDOUT_FILENO) != 0;
#endif
}

inline bool isStdinTty()
{
#ifdef _MSC_VER
    return ::_isatty(::_fileno(stdin)) != 0;
#else
    return ::isatty(STDIN_FILENO) != 0;
#endif
}

}  // namespace seabass::infrastructure::system
