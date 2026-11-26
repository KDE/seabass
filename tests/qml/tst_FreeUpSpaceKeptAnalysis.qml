// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui

// Free Up Space's kept original analyses, on a stick copy with kept
// entries planted: two of tracks on the stick, one of a track gone. The
// section lists them in two groups with their size, the gone one ticked
// and the others not; the confirmation says what removing them means;
// Remove deletes the ticked group's file and nothing else.
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

    function test_a_listsTicksAndRemoves() {
        const stick = stickFixture.stickCopy(testCase.fixtureRoot);
        verify(stick.length > 0, "the fixture must copy");
        const kept = stickFixture.plantKeptAnalyses(stick, 2, 1);
        compare(kept.length, 3, "three kept entries planted");
        const page = createTemporaryObject(pageComponent, testCase,
                                           {rekordboxPath: stick + "/PIONEER", enginePath: stick + "/Engine Library"});
        verify(page !== null, "the page must instantiate");
        const section = findChild(page, "keptAnalysisSection");
        verify(section !== null, "the page has the section");
        tryCompare(section.controller, "totalCount", 3, 10000);
        compare(findChild(page, "keptAnalysisCountLabel").text, "Original analyses kept for 3 track(s)");
        const groups = section.controller.groups;
        compare(groups.length, 2, "one group of tracks on the stick, one of tracks gone");
        compare(groups[0].gone, false);
        compare(groups[0].count, 2);
        compare(groups[0].included, false, "the tracks on the stick start unticked");
        compare(groups[1].gone, true);
        compare(groups[1].count, 1);
        compare(groups[1].included, true, "the track gone starts ticked");
        verify(groups[0].bytes > 0 && groups[1].bytes > 0, "each group has its size");
        compare(section.controller.includedCount, 1);
        verify(!findChild(page, "keptAnalysisEmpty").visible);

        // The confirmation says what is lost.
        const dialog = findChild(page, "confirmRemoveKeptDialog");
        verify(dialog !== null);
        verify(dialog.headline.indexOf("removed for good") >= 0, dialog.headline);
        verify(dialog.detailText.indexOf("no longer on the stick") >= 0, dialog.detailText);
        section.controller.setIncluded(0, true);
        verify(dialog.detailText.indexOf("\"Restore original analysis\" will no longer be possible") >= 0,
               dialog.detailText);
        verify(dialog.detailText.indexOf("Seabass's analysis stays on the stick") >= 0, dialog.detailText);
        section.controller.setIncluded(0, false);

        const button = findChild(page, "removeKeptAnalysisButton");
        tryVerify(() => button.enabled, 5000);
        mouseClick(button);
        tryVerify(() => dialog.opened, 5000);
        dialog.accept();
        tryCompare(section.controller, "totalCount", 2, 10000);
        verify(stickFixture.fileExists(kept[0]) && stickFixture.fileExists(kept[1]), "the kept of tracks on the stick stay");
        verify(!stickFixture.fileExists(kept[2]), "the one of a track gone is removed");
    }
}
