// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The suite is built out of assert(): 2100-odd of them across nearly
// every test file, none guarded. Three of CMake's four standard build
// types put -DNDEBUG in CMAKE_CXX_FLAGS_<CONFIG>, which turns all of
// them into no-ops -- the suite then passes without checking anything,
// and passes faster, so nothing looks wrong.
//
// CMakeLists.txt adds -UNDEBUG to every test target to stop that. This
// file is the proof that it works, rather than the belief that it does:
// if the flag ever stops reaching a test target, this one fails to
// compile and says why, instead of the whole suite quietly going hollow.
//
// Compile-time rather than runtime on purpose. A runtime check would
// have to assert something false and catch the abort, which is exactly
// the behaviour under test -- so it would pass either way in a build
// where assertions are dead.
#ifdef NDEBUG
#error "NDEBUG is defined in a test target: every assert() in the suite is a no-op. \
CMakeLists.txt adds -UNDEBUG to targets matching _test/_tests; either this target \
stopped matching, or the loop that adds it no longer runs after every add_executable."
#endif

#include <cassert>
#include <cstdlib>
#include <iostream>

int main()
{
    // Belt as well as braces: prove an assert() actually traps, by
    // running the failing one in a child process. A build where
    // assertions are dead reaches the exit(0) instead.
    if (std::getenv("SEABASS_ASSERT_CHILD") != nullptr) {
        assert(1 == 2);
        // Only reachable if assert() did nothing.
        std::exit(0);
    }

    std::cout << "NDEBUG is not defined in this test target\n";
    std::cout << "all cases passed\n";
    return 0;
}
