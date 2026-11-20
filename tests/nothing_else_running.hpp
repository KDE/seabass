// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include "infrastructure/system/rekordbox_process_detector.hpp"

#include <string>

namespace seabass::tests
{

// For the first line of main() in every non-live test binary whose code
// reaches the DJ-software guard (gui/write_guard.cpp and the controllers
// that ask the detector directly): the detector then sees no process at
// all, so no save in the suite is refused because rekordbox is open on
// the developer's machine, or because the release rig has its fake
// "rekordbox" running while the suite runs. Three suite runs went red
// that way on 2026-11-19, in tests about nothing of the sort.
//
// The guard itself is tested where it is meant to be: write_guard_test
// with scans of its own (and that this stub beats one naming rekordbox),
// and tests/qml-live/ against the rig's fake process, which the QML
// harness leaves on the real scan. tst_HarnessProcessScan (tests/qml) and
// LiveGuard (tests/qml-live) pin which lane got which. No test in the
// ordinary suite starts a process under a DJ application's name: the rig,
// a live lane or an installed Seabass on the same machine would see it.
inline bool noProcessIsRunning(const std::string &)
{
    return false;
}

inline void assumeNothingElseRunning()
{
    infrastructure::system::setProcessScanForTesting(&noProcessIsRunning);
}

}  // namespace seabass::tests
