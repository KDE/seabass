// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest

// An edit session knows both catalogs on its stick, whichever page opened
// it. Clean Up, Add Cue and Settings name only their own catalog, but a
// save's closing step needs Engine too: after export.pdb moves, Engine's
// record of the rekordbox import is brought level. A session Clean Up
// opened straight from the stick list had no Engine path, so the save
// left Engine behind and a Denon player then offered to import the
// rekordbox library over the Engine one -- issue #42's symptom by a second
// route, found by shakedown round 8 (W5) on Linux and macOS alike.
// The sessions are built in C++, see ControllerFixture.sessionCatalogsAfter.
TestCase {
    name: "EditSessionCatalogs"

    function test_openedOnRekordboxFindsEngine() {
        const s = controllerFixture.sessionCatalogsAfter(true, true, "rekordbox");
        compare(s.rekordbox, s.root + "/PIONEER");
        compare(s.engine, s.root + "/Engine Library",
                "a Clean Up session knows where Engine is, so the save can keep its import record level");
    }

    function test_openedOnEngineFindsRekordbox() {
        const s = controllerFixture.sessionCatalogsAfter(true, true, "engine");
        compare(s.rekordbox, s.root + "/PIONEER");
    }

    function test_noEngineOnTheStickNoEnginePath() {
        const s = controllerFixture.sessionCatalogsAfter(true, false, "rekordbox");
        compare(s.engine, "", "nothing is invented for a catalog that is not on the stick");
    }

    function test_aGivenEnginePathIsKept() {
        const s = controllerFixture.sessionCatalogsAfter(true, true, "rekordbox", "/somewhere/else/Engine Library");
        compare(s.engine, "/somewhere/else/Engine Library", "a path the page named is never replaced by a guess");
    }
}
