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
// The guard itself is tested where it is meant to be: the detector's own
// test against a real child process, write_guard_test with a scan of its
// own, dj_software_independence_test with the rig's fake process alive,
// and tests/qml-live/ (which the QML harness leaves on the real scan).
inline bool noProcessIsRunning(const std::string &)
{
    return false;
}

inline void assumeNothingElseRunning()
{
    infrastructure::system::setProcessScanForTesting(&noProcessIsRunning);
}

}  // namespace seabass::tests
