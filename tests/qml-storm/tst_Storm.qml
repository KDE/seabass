// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui

// The storm (docs/testing.md): per seed, the real app is walked through
// sticks going in and out, pages opened, cancelled and left, reads that are
// slow, stuck or failing, and saves a stick is pulled out of; then it is
// quit in a process of its own, which has to end by itself. Every failing
// seed is named with the steps that led to it, and replays with
// SEABASS_STORM_FIRST_SEED=<seed> SEABASS_STORM_SEEDS=1.
//
// SEABASS_STORM_SEEDS (default 3), SEABASS_STORM_STEPS (default 150) and
// SEABASS_STORM_FIRST_SEED (default 1) scale it; SEABASS_STORM_QUIT_STEPS
// (default 40) is the walk the quit leg takes before it closes the window;
// SEABASS_STORM_FAILURES names a file every failing seed is appended to.
TestCase {
    id: testCase
    name: "Storm"
    when: windowShown

    Component {
        id: runnerComponent
        StormRunner {}
    }

    function fixture() {
        return decodeURIComponent(Qt.resolvedUrl("../fixtures/anonymized_library").toString()
            .replace(/^file:\/\//, "").replace(/^\/([A-Za-z]:)/, "$1"));
    }

    function runSeed(seed, steps) {
        const runner = runnerComponent.createObject(testCase, {
            seed: seed, steps: steps, fixtureRoot: fixture(),
        });
        runner.start();
        // Generous: a seed is bounded by its own checks, this only stops
        // a walk that stopped walking.
        tryVerify(() => runner.done, 30 * 60 * 1000, "seed " + seed + " must finish its walk");
        const failure = runner.failure;
        const log = runner.log.slice();
        runner.destroy();
        return failure.length > 0 ? failure + "\n  steps:\n    " + log.join("\n    ") : "";
    }

    function test_storm() {
        verify(stormFixture.env("SEABASS_IGNORE_REMOVABLE_MEDIA").length > 0,
               "the storm must never see a real stick: SEABASS_IGNORE_REMOVABLE_MEDIA has to be set");
        const first = stormFixture.envInt("SEABASS_STORM_FIRST_SEED", 1);
        const seeds = stormFixture.envInt("SEABASS_STORM_SEEDS", 3);
        const steps = stormFixture.envInt("SEABASS_STORM_STEPS", 150);
        const quitSteps = stormFixture.envInt("SEABASS_STORM_QUIT_STEPS", 40);
        const failed = [];
        for (let seed = first; seed < first + seeds; ++seed) {
            stormFixture.log("STORM seed " + seed + ": " + steps + " steps");
            let failure = runSeed(seed, steps);
            if (failure.length === 0 && quitSteps > 0) {
                const quitAt = 1 + (seed * 7919) % quitSteps;
                const leg = stormFixture.runQuitLeg(Qt.resolvedUrl("StormQuitDriver.qml").toString(), seed,
                                                    quitSteps, quitAt, 60000);
                if (leg.length > 0) {
                    failure = "seed " + seed + ", quit leg at step " + quitAt + ": " + leg;
                }
            }
            if (failure.length > 0) {
                stormFixture.log("STORM FAILED " + failure);
                stormFixture.recordFailure("seed " + seed + ": " + failure.split("\n")[0]);
                failed.push(seed);
            } else {
                stormFixture.log("STORM seed " + seed + " passed");
            }
        }
        compare(failed.length, 0, "seeds that failed: " + failed.join(", "));
    }
}
