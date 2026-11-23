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

    // Bound as Main.qml binds the app's one player: nothing is read
    // while a library is being written (see tst_CleanupPage).
    PlaybackController {
        id: realPlayback
        libraryBusy: EditSessionRegistry.anyWriting
    }
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

    // The two audio files the fixture's rows name, written beside the
    // library copy: "../folderA/Duplicate Song.mp3" and folderB's. The
    // copies' audio is lined up before any cue crosses between them, and
    // two files with the same bytes line up with no decoding at all; a
    // copy with no file, or with other bytes that do not decode, cannot be
    // lined up, and the group is then not fixable.
    function writeAudio(library, contentA, contentB) {
        const stick = library.substring(0, library.lastIndexOf("/"));
        verify(stickFixture.makeDirectory(stick + "/folderA") && stickFixture.makeDirectory(stick + "/folderB"));
        verify(stickFixture.writeText(stick + "/folderA/Duplicate Song.mp3", contentA));
        verify(stickFixture.writeText(stick + "/folderB/Duplicate Song.mp3", contentB));
        return library;
    }

    function initTestCase() {
        testCase.engineCopy = artworkFixture.libraryCopy(testCase.fixtureEngineRoot);
        verify(testCase.engineCopy.length > 0, "the fixture copy must be made");
        writeAudio(testCase.engineCopy, "the same bytes", "the same bytes");
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

    Component { id: spyComponent; SignalSpy {} }

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

    function saveSession(session) {
        let summary = null;
        const done = (result) => { summary = result; };
        session.saveFinished.connect(done);
        session.save();
        tryVerify(() => summary !== null, 300000, "the save finishes");
        session.saveFinished.disconnect(done);
        return summary;
    }

    // A save that landed whole is said over the list with the countdown
    // toast, in the summary's own words, and opens no summary dialog; the
    // groups are scanned again afterwards. A real save, on a copy of its
    // own: the shared one is what the other tests scan, and a save would
    // leave it with nothing to fix.
    function test_aFinishedSaveIsSaidOverTheList() {
        const own = artworkFixture.libraryCopy(testCase.fixtureEngineRoot);
        verify(own.length > 0, "the test's own copy must be made");
        writeAudio(own, "the same bytes", "the same bytes");
        const page = createTemporaryObject(pageComponent, testCase, {width: 960, height: 660, enginePath: own});
        verify(page, "page did not instantiate");
        tryVerify(() => findWhere(page.header, (item) => item.visible && item.text !== undefined
                                  && /^1 duplicate group/.test(String(item.text))) !== null,
                  5000, "the scan never reported the fixture's duplicate group");
        waitForRendering(page);
        mouseClick(stageButton(page));
        const session = EditSessionRegistry.sessionFor(EditSessionRegistry.libraryIdForPath(own));
        tryVerify(() => session.pendingCount > 0, 5000, "Stage All Fixable staged the group");
        const summary = saveSession(session);
        compare(summary.error, "", JSON.stringify(summary));
        verify(summary.written > 0, JSON.stringify(summary));

        const toast = findChild(page, "savedToast");
        tryCompare(toast, "showing", true, 5000);
        compare(findChild(toast, "confirmationToastLabel").text,
                summary.written + (summary.written === 1 ? " group" : " groups") + " copied");
        const dialog = findChild(page, "summaryDialog");
        verify(dialog === null || !dialog.visible, "no summary dialog over a save that landed whole");
        const controller = findChild(page, "duplicatesController");
        tryCompare(controller, "busy", false, 30000);
    }

    // The page says what it is second to: a copy is a copy, and Clean Up
    // Duplicates is the page that removes it. The button goes there.
    function test_thePageSuggestsCleanUpInstead() {
        const page = makePage(960);
        const suggestion = findChild(page, "cleanupSuggestion");
        verify(suggestion !== null && suggestion.visible, "said once there are groups");
        const spy = createTemporaryObject(spyComponent, testCase, {target: page, signalName: "cleanupRequested"});
        findChild(page, "cleanUpInsteadButton").clicked();
        tryCompare(spy, "count", 1, 2000);
        compare(spy.signalArguments[0][1], page.rekordboxPath);
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

    // Copies whose audio cannot be lined up are listed, not fixable, and
    // say why: nobody knows where one copy's cues belong on the other.
    function test_copiesThatCannotBeLinedUpAreNotCopied() {
        const own = writeAudio(artworkFixture.libraryCopy(testCase.fixtureEngineRoot), "one file", "another file");
        const page = createTemporaryObject(pageComponent, testCase, {width: 960, height: 660, enginePath: own});
        verify(page, "page did not instantiate");
        tryVerify(() => findWhere(page.header, (item) => item.visible && item.text !== undefined
                                  && /^1 duplicate group/.test(String(item.text))) !== null,
                  5000, "the group is still listed");
        const badge = findWhere(page, (item) => item.objectName === "kindBadge");
        verify(badge, "the group's badge was not found");
        compare(badge.label, "Not lined up");
        // And the way out, with its cost.
        verify(badge.tooltipText.includes("Clean Up Duplicates can remove the extra copies and free their space."),
               badge.tooltipText);
        verify(badge.tooltipText.includes("the cues of a copy it removes are not carried over"), badge.tooltipText);
        const note = findWhere(page, (item) => item.objectName === "alignmentNote");
        verify(note && note.visible, "the group says which copy");
        compare(note.text, "Copy 2 could not be lined up with copy 1, so no cues are copied between them.");
        // The numbers the note uses are on the copies' cards, once the
        // group is opened.
        const group = findWhere(page, (item) => item.expanded !== undefined && item.alignmentNote !== undefined);
        verify(group, "the group was not found");
        group.expanded = true;
        waitForPolish(page);
        verify(findWhere(page, (item) => item.visible && item.statusBadgeText === "COPY 1") !== null,
               "copy 1 is not numbered on the page");
        verify(findWhere(page, (item) => item.visible && item.statusBadgeText === "COPY 2") !== null,
               "copy 2 is not numbered on the page");
        const copyButton = findWhere(page, (item) => item.text === "Copy Cues" && item.clicked !== undefined);
        verify(copyButton === null || !copyButton.visible, "nothing to copy");
    }

    // Copies that are not sample-aligned are lined up, and the page says
    // by how much: copy 2's music starts 463 samples (21 ms) later. WAV
    // files, so no MP3 lead-in comes between the decoder and the cues.
    function test_aShiftedCopyIsSaid() {
        const own = artworkFixture.libraryCopy(testCase.fixtureEngineRoot);
        verify(artworkFixture.setTrackPath(own, 1, "../folderA/Duplicate Song.wav"));
        verify(artworkFixture.setTrackPath(own, 2, "../folderB/Duplicate Song.wav"));
        const stick = own.substring(0, own.lastIndexOf("/"));
        verify(artworkFixture.writeNoiseWav(stick + "/folderA/Duplicate Song.wav", 10, 0, 5));
        verify(artworkFixture.writeNoiseWav(stick + "/folderB/Duplicate Song.wav", 10, 463, 5));
        const page = createTemporaryObject(pageComponent, testCase, {width: 960, height: 660, enginePath: own});
        verify(page, "page did not instantiate");
        tryVerify(() => findWhere(page, (item) => item.objectName === "alignmentNote" && item.visible) !== null,
                  20000, "the shift is said");
        compare(findWhere(page, (item) => item.objectName === "alignmentNote").text,
                "Copy 2 starts 21 ms later than copy 1, so cues are moved to match.");
        compare(findWhere(page, (item) => item.objectName === "kindBadge").label, "Fixable");
    }

    // Copies that line up as they are say nothing about it.
    function test_copiesThatLineUpSayNothing() {
        const page = makePage(960);
        const note = findWhere(page, (item) => item.objectName === "alignmentNote");
        verify(note !== null, "the note's label is there");
        verify(!note.visible, "and empty: " + note.text);
        compare(findWhere(page, (item) => item.objectName === "kindBadge").label, "Fixable");
    }

    // One frame round the whole group: the header and, opened, the copies
    // below it. The frame used to be round the copies only, so an opened
    // group was a bare header floating over a box.
    function test_oneFrameRoundTheWholeGroup() {
        const page = makePage(960);
        const frame = findWhere(page, (item) => item.objectName === "groupFrame");
        verify(frame, "no group frame");
        const header = findWhere(frame, (item) => item.objectName === "groupHeader");
        const copies = findWhere(frame, (item) => item.objectName === "groupCopies");
        verify(header && copies, "the header and the copies must both sit inside the frame");
        verify(!copies.visible, "a group starts closed");
        compare(frame.height, header.height, "a closed frame is just its header");

        mouseClick(header);
        tryCompare(copies, "visible", true);
        tryVerify(() => copies.height > 0 && frame.height >= header.height + copies.height,
                  2000, "the opened frame must hold header and copies both");
        if (screenshotDir) {
            waitForRendering(page);
            grabImage(page).save(screenshotDir + "/DuplicatesPage-group-open.png");
        }
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
