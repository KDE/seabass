// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

// Force-included (via /FI, see CMakeLists.txt's -UNDEBUG loop, which this
// rides alongside) into every *_test/*_tests target on MSVC. Without
// this, a failing assert() in a Debug build -- which is what /UNDEBUG
// exists to keep alive -- goes through the CRT's default _CRT_ASSERT
// report mode, which opens a "Debug Assertion Failed!" dialog box and
// blocks forever waiting for someone to click Abort. Under ctest (or any
// other headless/CI invocation) nobody ever will: confirmed directly --
// filesystem_backup_store_test and duplicate_cleanup_interruption_test,
// both genuinely failing a real assert(), sat until ctest's own
// --timeout killed them instead of failing in under a second the way
// every other assert()-based test here does. Redirecting both
// _CRT_ASSERT and _CRT_ERROR to stderr instead makes a tripped assert()
// print its message and abort() immediately, matching GCC/Clang's
// behavior (and this project's own Linux/MinGW test runs) exactly.
#ifdef _MSC_VER
#include <cstdio>
#include <crtdbg.h>

namespace seabass::testing::detail
{

struct DisableAssertDialog
{
    DisableAssertDialog()
    {
        _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
        _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    }
};

inline DisableAssertDialog g_disableAssertDialog;

// The MSVC CRT fully buffers stdout (a several-KB buffer, flushed only
// when full or on a normal exit()) whenever it is not attached to a real
// console -- which is exactly what ctest's own output capture is: a
// pipe, never a console. seabass_qml_tests writes its entire PASS/FAIL
// report (Qt Test's own plain-text logger) to stdout, and confirmed
// directly, every byte of it was silently lost under ctest -- the test
// visibly ran (its own qWarning()s, which go to stderr, always came
// through; real elapsed time in the hundreds of seconds), genuinely
// failed, and yet ctest recorded empty output for a real failure with no
// way to see why. exit()/return-from-main do flush stdio on a clean
// path, so this was never about *this* test crashing -- QtQuickTest's
// own internal exit path just never reaches that flush reliably here.
// Forcing stdout unbuffered from the very first static initializer,
// before QCoreApplication or anything else in main() runs, means every
// write lands immediately, on any exit path -- matching GCC/Clang's
// default of a real terminal being line-buffered closely enough that
// this project's Linux/MinGW runs never had to think about it.
struct UnbufferStdout
{
    UnbufferStdout() { std::setvbuf(stdout, nullptr, _IONBF, 0); }
};

inline UnbufferStdout g_unbufferStdout;

}  // namespace seabass::testing::detail
#endif
