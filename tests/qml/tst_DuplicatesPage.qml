// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui

// Match Duplicate Cues' header, at the widths a real window actually gets.
//
// The row under the breadcrumb carries the group count, what could be
// freed, the audio-comparison note, the staged count and the two buttons.
// It was a RowLayout of labels at their natural width, so once the text
// grew past the window the row simply ran off the right edge, and the
// first thing to go was the button the page is for: "Stage All Fixable"
// was clipped in a window of ordinary size. tst_CleanupPage measures the
// same failure on Clean Up's filter row; this is the same measurement here.
TestCase {
    id: testCase
    name: "DuplicatesPage"
    width: 1000
    height: 700
    visible: true
    when: windowShown

    PlaybackController { id: realPlayback }
    AppSettingsController { id: realAppSettings }

    // A copy of the small synthetic Engine library with one track filed
    // twice, so the scan finds a group and the row shows the labels that
    // only appear then. A copy, because opening the page opens an edit
    // session that writes beside the library (see ArtworkFixture).
    readonly property string fixtureEngineRoot: {
        const url = Qt.resolvedUrl("../fixtures/duplicate_engine_library").toString();
        return decodeURIComponent(url.replace(/^file:\/\//, "").replace(/^\/([A-Za-z]:)/, "$1"));
    }
    property string engineCopy: ""

    Component {
        id: pageComponent
        DuplicatesPage {
            stickLabel: "TESTSTICK"
            rekordboxPath: ""
            enginePath: testCase.engineCopy
            playbackController: realPlayback
            appSettingsController: realAppSettings
        }
    }

    function initTestCase() {
        testCase.engineCopy = artworkFixture.libraryCopy(testCase.fixtureEngineRoot);
        verify(testCase.engineCopy.length > 0, "the fixture copy must be made");
    }

    function findWhere(item, predicate) {
        if (predicate(item)) {
            return item;
        }
        const kids = item.children ? item.children : [];
        for (let i = 0; i < kids.length; ++i) {
            const found = findWhere(kids[i], predicate);
            if (found) {
                return found;
            }
        }
        return null;
    }

    function stageButton(page) {
        return findWhere(page.header, (item) => item.text === "Stage All Fixable" && item.clicked !== undefined);
    }

    function makePage(pageWidth) {
        const page = createTemporaryObject(pageComponent, testCase, {width: pageWidth, height: 660});
        verify(page, "page did not instantiate");
        // The scan runs on a worker; the row is only complete once the
        // group count says it found the duplicate.
        tryVerify(() => findWhere(page.header, (item) => item.visible && item.text !== undefined
                                  && /^1 duplicate group/.test(String(item.text))) !== null,
                  5000, "the scan never reported the fixture's duplicate group");
        waitForRendering(page);
        return page;
    }

    function test_theActionRowStaysInsideThePage_data() {
        return [
            {tag: "960", pageWidth: 960},
            {tag: "700", pageWidth: 700},
            {tag: "520", pageWidth: 520},
            {tag: "380", pageWidth: 380},
        ];
    }

    function test_theActionRowStaysInsideThePage(row) {
        const page = makePage(row.pageWidth);
        const inner = page.width - Theme.pageMargin + 0.5;

        // The button the page is for, whole and inside the header's inset.
        const stage = stageButton(page);
        verify(stage, "the Stage All Fixable button was not found");
        verify(stage.visible);
        const left = stage.mapToItem(page, 0, 0).x;
        const right = stage.mapToItem(page, stage.width, 0).x;
        verify(right <= inner, "Stage All Fixable reaches " + right + " in a " + page.width + " page");
        verify(stage.width >= stage.implicitWidth - 0.5,
               "Stage All Fixable is squeezed to " + stage.width + " of " + stage.implicitWidth);

        // And every piece of text in the row with it, which is what a
        // reader sees run off the edge or slide under the buttons.
        const summaryRow = findWhere(page.header, (item) => item.objectName === "summaryRow");
        verify(summaryRow, "the summary row was not found");
        function check(item) {
            for (let i = 0; i < item.children.length; ++i) {
                const child = item.children[i];
                // Buttons are checked above, by their own edges; the
                // label a style draws inside one is not row text.
                if (!child.visible || child.clicked !== undefined) {
                    continue;
                }
                if (child.text !== undefined && child.width > 0) {
                    const p = child.mapToItem(page, 0, 0);
                    verify(p.x + child.width <= inner,
                           "\"" + child.text + "\" reaches " + (p.x + child.width) + " in a " + page.width + " page");
                    const clearOfButton = p.x + child.width <= left + 0.5
                        || p.y >= stage.mapToItem(page, 0, stage.height).y - 0.5;
                    verify(clearOfButton, "\"" + child.text + "\" runs under the Stage All Fixable button");
                }
                check(child);
            }
        }
        check(summaryRow);
    }

    function test_screenshot_data() {
        return test_theActionRowStaysInsideThePage_data();
    }

    function test_screenshot(row) {
        if (!screenshotDir) {
            skip("SEABASS_SCREENSHOT_DIR not set");
        }
        const page = makePage(row.pageWidth);
        wait(100);
        grabImage(page).save(screenshotDir + "/DuplicatesPage-" + row.pageWidth + ".png");
    }
}
