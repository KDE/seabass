// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui


// The Cover Art page, both ways round: nothing to report on one library,
// and the right words on one where every track's art points at the
// computer that ran Engine's import.
//
// The controller is reached through the page's own consistencyController
// property, not findChild(): the page's instance carries an id and no
// objectName, so a findChild("consistencyController") returns null, and
// the assertions that used to sit behind `if (controller !== null)` were
// all skipped, every run, silently.
TestCase {
    id: testCase
    name: "CoverArtPage"
    width: 1000
    height: 700
    visible: true
    when: windowShown

    // tests/qml/ -> tests/fixtures/anonymized_library/engine, which is a
    // real Engine library: 1562 tracks whose AlbumArt.hash is an
    // "image://fileart//media/WHALESHARK2/..." path from the machine that
    // imported it, and no rekordbox art beside it here. A local path, not
    // a URL (see tst_SettingsPage.qml for why the naive strip is wrong).
    readonly property string fixtureEngineRoot: {
        var url = Qt.resolvedUrl("../fixtures/anonymized_library/engine").toString();
        return decodeURIComponent(url.replace(/^file:\/\//, "").replace(/^\/([A-Za-z]:)/, "$1"));
    }

    Component {
        id: pageComponent
        CoverArtPage {
            width: 980
            height: 660
            stickLabel: "TESTSTICK"
            rekordboxPath: "/nonexistent/TESTSTICK/PIONEER"
            enginePath: "/nonexistent/TESTSTICK/Engine Library"
        }
    }

    // A copy, never the fixture itself: pointing a page at a library opens
    // an edit session on the stick it sits on, which writes Seabass/backups
    // and a .write.lock beside it -- into the source tree, if that is where
    // the library is.
    property string mixedFaultLibrary: ""

    Component {
        id: fixturePageComponent
        CoverArtPage {
            width: 980
            height: 660
            stickLabel: "FIXTURE"
            rekordboxPath: ""
            enginePath: testCase.mixedFaultLibrary
        }
    }

    // A library whose only cover-art fault is deleted image files: the
    // rows are proper hashes, the Artwork folder is empty. Built here,
    // because the committed fixture carries both faults at once and cannot
    // show what the page says about this one on its own.
    property string missingImagesLibrary: ""

    Component {
        id: missingImagesPageComponent
        CoverArtPage {
            width: 980
            height: 660
            stickLabel: "MISSING"
            rekordboxPath: ""
            enginePath: testCase.missingImagesLibrary
        }
    }

    function test_theCoverArtNoticeDoesNotOfferAReimportForDeletedImages() {
        testCase.missingImagesLibrary = artworkFixture.libraryWithMissingImages(testCase.fixtureEngineRoot);
        verify(testCase.missingImagesLibrary.length > 0, "the fixture library must be built");
        var page = createTemporaryObject(missingImagesPageComponent, testCase);
        var controller = page.consistencyController;
        tryVerify(function() { return controller.busy === false; }, 60000, "the scan must finish");

        compare(controller.artworkError, "");
        verify(controller.artworkTracksWithArt > 0, "the copy's tracks ask for art");
        compare(controller.artworkMissingFileCount,
                controller.artworkTracksWithArt - controller.artworkBrokenRowCount,
                "every row that has a hash has lost its image file");
        compare(controller.artworkImportedCount, 0, "and none of them came from an import");
        compare(controller.artworkRepairableCount, 0);

        var explanation = findChild(page, "coverArtExplanation");
        verify(explanation.visible, "the page says what it found");
        verify(explanation.text.indexOf("Engine Library/Artwork") >= 0,
               "it says where the pictures should be: " + explanation.text);
        // The advice for an import these tracks never had. Offering it here
        // sent the user to re-import a rekordbox library that has nothing
        // to do with the fault.
        verify(explanation.text.indexOf("import rekordbox library") < 0,
               "and does not blame an import that never happened: " + explanation.text);
        verify(explanation.text.indexOf("None of their images are on this stick") < 0,
               "nor offer to copy in images that were never there: " + explanation.text);
    }

    // Both non-imported faults at once, which is where a shared closing
    // sentence attached itself to the wrong one: rows with no hash were
    // told Engine DJ would write their pictures again, which it cannot do
    // for a row with no hash to write them under.
    property string twoFaultLibrary: ""

    Component {
        id: twoFaultPageComponent
        CoverArtPage {
            width: 980
            height: 660
            stickLabel: "TWOFAULT"
            rekordboxPath: ""
            enginePath: testCase.twoFaultLibrary
        }
    }

    function test_eachCoverArtFaultCarriesItsOwnAdvice() {
        testCase.twoFaultLibrary =
            artworkFixture.libraryWithMissingImagesAndEmptyRows(testCase.fixtureEngineRoot, 3);
        verify(testCase.twoFaultLibrary.length > 0, "the fixture library must be built");
        var page = createTemporaryObject(twoFaultPageComponent, testCase);
        var controller = page.consistencyController;
        tryVerify(function() { return controller.busy === false; }, 60000, "the scan must finish");

        compare(controller.artworkImportedCount, 0);
        compare(controller.artworkRepairableCount, 0);
        verify(controller.artworkMissingFileCount > 0, "some rows have hashes whose file is gone");
        verify(controller.artworkBrokenRowCount > 0, "and some have no hash at all");

        var explanation = findChild(page, "coverArtExplanation");
        var text = explanation.text;
        verify(text.indexOf("Engine Library/Artwork") >= 0, "the missing-image fault is named: " + text);
        verify(text.indexOf("no image to look for") >= 0, "the hash-less fault is named: " + text);
        // The advice belongs to the fault it can help. Said last, after the
        // hash-less sentence, it described rows it cannot help at all.
        var adviceAt = text.indexOf("Engine DJ writes those again");
        var brokenAt = text.indexOf("point at an art row with nothing in it");
        verify(adviceAt >= 0, "the missing-image fault keeps its advice: " + text);
        verify(adviceAt < brokenAt, "and the advice comes before the fault it does not apply to: " + text);
        verify(text.indexOf("import rekordbox library") < 0,
               "neither fault came from an import: " + text);
    }

    // Staging is the whole point of staging: the button that offers the
    // fix must put it in the save queue and touch nothing. It wrote
    // straight to the stick once, which made the Save press -- and the
    // user's decision behind it -- meaningless.
    property string repairableLibrary: ""

    Component {
        id: repairablePageComponent
        CoverArtPage {
            width: 980
            height: 660
            stickLabel: "REPAIRABLE"
            rekordboxPath: ""
            enginePath: testCase.repairableLibrary
        }
    }

    function test_theFixStagesAndWritesNothingUntilSave() {
        testCase.repairableLibrary = artworkFixture.libraryWithRepairableArt(testCase.fixtureEngineRoot);
        verify(testCase.repairableLibrary.length > 0, "the fixture library must be built");
        var page = createTemporaryObject(repairablePageComponent, testCase);
        var controller = page.consistencyController;
        tryVerify(function() { return controller.busy === false; }, 300000, "the scan must finish");
        verify(controller.artworkRepairableCount > 0, "this library has art Seabass can copy in");
        var before = controller.artworkReadableCount;

        var button = findChild(page, "fixCoverArtButton");
        verify(button !== null && button.visible, "the fix is offered");
        button.clicked();

        compare(controller.artworkRepairStaged, true, "it is staged");
        var note = findChild(page, "coverArtStagedNote");
        verify(note !== null && note.visible, "and the page says it is not saved yet");
        compare(button.text, "Unstage", "the same button takes it back");
        // Nothing reached the library: the counts come from the database,
        // and they have not moved.
        compare(controller.artworkReadableCount, before, "nothing was written");
        compare(controller.artworkRepairableCount > 0, true);

        button.clicked();
        compare(controller.artworkRepairStaged, false, "unstaging takes it back out");
    }

    function test_theCoverArtNoticeStaysAwayWithNothingToReport() {
        var page = createTemporaryObject(pageComponent, testCase);
        verify(page !== null, "the page must instantiate");
        var controller = page.consistencyController;
        verify(controller !== null && controller !== undefined, "the page must have a controller");
        tryVerify(function() { return controller.busy === false; }, 60000, "the scan must finish");
        // The page is the report: with nothing to say it says so, and
        // offers no action.
        var headline = findChild(page, "coverArtHeadline");
        verify(headline !== null && headline.visible, "the page must say what it found");
        verify(headline.text.indexOf("No Engine library") >= 0 || headline.text.indexOf("can find it") >= 0,
               "it reports a library with nothing wrong: " + headline.text);
        var fix = findChild(page, "fixCoverArtButton");
        verify(fix === null || !fix.visible, "and offers no fix");
        compare(controller.artworkUnreadableCount, 0);
        compare(controller.artworkRepairableCount, 0);
        compare(controller.artworkRepairStaged, false);
        // A path that is not there is not a library that could not be read:
        // the audit never ran, so there is no fault to report either.
        compare(controller.artworkError, "");
    }

    function test_theCoverArtNoticeNamesTheFaultItFound() {
        testCase.mixedFaultLibrary = artworkFixture.libraryCopy(testCase.fixtureEngineRoot);
        verify(testCase.mixedFaultLibrary.length > 0, "the fixture copy must be made");
        var page = createTemporaryObject(fixturePageComponent, testCase);
        verify(page !== null, "the page must instantiate");
        var controller = page.consistencyController;
        tryVerify(function() { return controller.busy === false; }, 300000, "the fixture scan must finish");

        compare(controller.artworkError, "", "the fixture's database reads");
        verify(controller.artworkTracksWithArt > 0, "the fixture's tracks ask for art");
        compare(controller.artworkReadableCount, 0, "none of it is stored the way a player reads");
        compare(controller.artworkUnreadableCount, controller.artworkTracksWithArt);
        // The fixture carries both faults at once, which is exactly the
        // pair the notice used to describe with one sentence: most rows are
        // imported paths from the machine that ran the import, and the rest
        // are proper hash rows whose image file is not in the library. None
        // of their images are on this stick (no PIONEER/Artwork here), so
        // the repair has nothing to offer either way.
        verify(controller.artworkImportedCount > 0, "the fixture has imported paths");
        verify(controller.artworkMissingFileCount > 0, "and hash rows whose file is gone");
        compare(controller.artworkImportedCount + controller.artworkMissingFileCount
                + controller.artworkBrokenRowCount,
                controller.artworkUnreadableCount, "and nothing else");
        compare(controller.artworkRepairableCount, 0);

        var headline = findChild(page, "coverArtHeadline");
        verify(headline !== null && headline.visible, "a library in this state must say so");
        verify(headline.text.indexOf(String(controller.artworkUnreadableCount)) >= 0,
               "the headline counts them: " + headline.text);
        var explanation = findChild(page, "coverArtExplanation");
        verify(explanation.text.indexOf("import rekordbox library") >= 0,
               "and names the import that did it: " + explanation.text);
        verify(explanation.text.indexOf("Engine Library/Artwork") >= 0,
               "and the missing image files too, which are a different fault: " + explanation.text);
        verify(explanation.text.indexOf(String(controller.artworkImportedCount)) >= 0
               && explanation.text.indexOf(String(controller.artworkMissingFileCount)) >= 0,
               "with how many of each: " + explanation.text);
        var button = findChild(page, "fixCoverArtButton");
        verify(button === null || !button.visible, "nothing to copy in, so nothing to offer");
        var nothing = findChild(page, "coverArtNothingToDo");
        verify(nothing !== null && nothing.visible, "and the page says why there is no button");
    }
}
