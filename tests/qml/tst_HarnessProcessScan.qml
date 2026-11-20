// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest

// This lane runs on the "nothing running" process scan
// (tests/nothing_else_running.hpp), so no save in it is refused because
// rekordbox, or the release rig's fake one, runs on the machine. Read
// back from the detector by the harness (harnessProcessScan), so a
// harness that stopped installing the stub fails here and not, some day,
// in a test about something else. tests/qml-live's LiveGuard pins the
// other direction: the live lane keeps the real scan.
TestCase {
    name: "HarnessProcessScan"

    function test_ordinaryLaneIsOnTheStub() {
        compare(harnessProcessScan, "stubbed");
    }
}
