// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick

// The quit leg of the storm, run as the app itself (seabass_qml_tests
// --storm-quit, see qml_test_main.cpp): a seed's walk until the window is
// closed at SEABASS_STORM_QUIT_STEP, then main()'s own ending. The parent
// (tst_Storm.qml) wants the process to end by itself, within a bound, with
// exit code 0.
Item {
    StormRunner {
        id: runner
        seed: stormFixture.envInt("SEABASS_STORM_SEED", 1)
        steps: stormFixture.envInt("SEABASS_STORM_STEPS", 40)
        quitStep: stormFixture.envInt("SEABASS_STORM_QUIT_STEP", 20)
        fixtureRoot: decodeURIComponent(Qt.resolvedUrl("../fixtures/anonymized_library").toString()
            .replace(/^file:\/\//, "").replace(/^\/([A-Za-z]:)/, "$1"))
        onFinished: {
            // A walk that failed before the quit is the in-suite run's to
            // report; here only the ending is on trial, so it ends anyway.
            stormFixture.log("STORM QUIT LEG WALKED seed " + runner.seed + " (" + (runner.failure || "no failure") + ")");
            Qt.quit();
        }
    }
    Component.onCompleted: {
        stormFixture.markQuitLeg();
        runner.start();
    }
}
