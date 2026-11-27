// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui

// Free Up Space's kept original analyses, on a stick copy with kept
// entries planted: two of rekordbox's of tracks on the stick, one of a
// track not on it under its name, the player's of one of those, and the
// player's of another track alone (Beat Grids & Waveforms', not listed). The section
// lists them in groups with their size, every group unticked; the
// confirmation says what removing them means (Restore Engine's Analysis
// too, for the player's); Remove deletes the ticked group's file and
// nothing else.
TestCase {
    id: testCase
    name: "FreeUpSpaceKeptAnalysis"
    width: 1000
    height: 900
    visible: true
    when: windowShown

    readonly property string fixtureRoot: {
        const url = Qt.resolvedUrl("../fixtures/anonymized_library").toString();
        return decodeURIComponent(url.replace(/^file:\/\//, "").replace(/^\/([A-Za-z]:)/, "$1"));
    }

    Component {
        id: pageComponent
        PendingDeletionsPage {
            width: 980
            height: 860
            stickLabel: "TESTSTICK"
        }
    }

    // Nothing kept: the section is not shown, not even while it lists,
    // and the orphaned files' list is as tall as without the section.
    function test_b_nothingKeptTakesNoRoom() {
        const stick = stickFixture.stickCopy(testCase.fixtureRoot);
        verify(stick.length > 0, "the fixture must copy");
        const page = createTemporaryObject(pageComponent, testCase,
                                           {rekordboxPath: stick + "/PIONEER", enginePath: stick + "/Engine Library"});
        verify(page !== null);
        const section = findChild(page, "keptAnalysisSection");
        compare(section.visible, false, "not shown while it lists for the first time");
        tryCompare(section.controller, "busy", false, 10000);
        compare(section.visible, false, "not shown with nothing kept");
        waitForPolish(page);
        const list = findChild(page, "pendingListView");
        const withSection = list.height;
        section.parent = null;  // the page as it would be without the section
        waitForPolish(page);
        tryCompare(list, "height", withSection, 2000, "the list's height is what it is without the section");
    }

    function test_a_listsTicksAndRemoves() {
        const stick = stickFixture.stickCopy(testCase.fixtureRoot);
        verify(stick.length > 0, "the fixture must copy");
        const kept = stickFixture.plantKeptAnalyses(stick, 2, 1);
        compare(kept.length, 3, "three kept entries planted");
        // The player's analysis of a track Analyse again kept rekordbox's
        // of: listed. One kept alone (Beat Grids & Waveforms) is not.
        const keptEngine = stickFixture.plantKeptEngineAnalysis(stick, "kept 0.wav");
        const engineAlone = stickFixture.plantKeptEngineAnalysis(stick, "player alone.wav");
        verify(engineAlone.length > 0);
        verify(keptEngine.length > 0, "the player's kept entry planted");
        const page = createTemporaryObject(pageComponent, testCase,
                                           {rekordboxPath: stick + "/PIONEER", enginePath: stick + "/Engine Library"});
        verify(page !== null, "the page must instantiate");
        const section = findChild(page, "keptAnalysisSection");
        verify(section !== null, "the page has the section");
        tryCompare(section.controller, "totalCount", 4, 10000);
        compare(findChild(page, "keptAnalysisCountLabel").text, "Original analyses kept for 4 track(s)");
        const groups = section.controller.groups;
        compare(groups.length, 3, "rekordbox's of tracks on the stick, the player's, and one of a track gone");
        compare(groups[0].kind, "rekordbox");
        compare(groups[0].gone, false);
        compare(groups[0].count, 2);
        compare(groups[1].kind, "engine");
        compare(groups[1].gone, false);
        compare(groups[2].kind, "rekordbox");
        compare(groups[2].gone, true);
        compare(groups[2].count, 1);
        for (const group of groups) {
            compare(group.included, false, "every group starts unticked, the one of a track gone too");
            verify(group.bytes > 0, "each group has its size");
        }
        compare(section.controller.includedCount, 0);
        verify(section.visible, "shown with something kept");
        const button = findChild(page, "removeKeptAnalysisButton");
        compare(button.enabled, false, "nothing to remove until something is ticked");

        // The confirmation says what is lost, and what is known.
        const dialog = findChild(page, "confirmRemoveKeptDialog");
        verify(dialog !== null);
        section.controller.setIncluded(2, true);
        verify(dialog.headline.indexOf("removed for good") >= 0, dialog.headline);
        verify(dialog.detailText.indexOf("not on the stick under the name they were kept for any more") >= 0,
               dialog.detailText);
        verify(dialog.detailText.indexOf("safe") < 0, "nothing is called safe to remove: " + dialog.detailText);
        section.controller.setIncluded(0, true);
        verify(dialog.detailText.indexOf("\"Restore original analysis\" will no longer be possible") >= 0,
               dialog.detailText);
        verify(dialog.detailText.indexOf("Seabass's analysis stays on the stick") >= 0, dialog.detailText);
        verify(dialog.detailText.indexOf("Restore Engine's Analysis") < 0, "no Engine entry ticked yet");
        section.controller.setIncluded(1, true);
        verify(dialog.detailText.indexOf("\"Restore Engine's Analysis\" on the Beat Grids & Waveforms page will no "
                                         + "longer be possible for the 1 track(s)") >= 0, dialog.detailText);
        section.controller.setIncluded(0, false);
        section.controller.setIncluded(1, false);

        tryVerify(() => button.enabled, 5000);
        mouseClick(button);
        tryVerify(() => dialog.opened, 5000);
        dialog.accept();
        tryCompare(section.controller, "totalCount", 3, 10000);
        verify(stickFixture.fileExists(kept[0]) && stickFixture.fileExists(kept[1]) && stickFixture.fileExists(keptEngine),
               "the kept of tracks on the stick stay");
        verify(stickFixture.fileExists(engineAlone), "the player's analysis kept alone is not Free Up Space's");
        verify(!stickFixture.fileExists(kept[2]), "the ticked one of a track gone is removed");
    }
}
