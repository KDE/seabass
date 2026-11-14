// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui

// Creating an Engine library has to tell the rest of the app that the
// stick now has one.
//
// What a stick HAS is a snapshot taken by detect(), and the only thing
// that re-takes it by itself is a udev hotplug event. This workflow
// writes a folder onto a stick that is already mounted, so nothing
// fires: without the re-detect the stick list goes on saying there is no
// Engine library, Browse offers no Engine side, and the Create card is
// still on offer for a library that now exists.
TestCase {
    id: testCase
    name: "EngineLibraryCreatorPage"
    width: 900
    height: 700
    visible: true
    when: windowShown

    Component {
        id: pageComponent
        EngineLibraryCreatorPage {
            width: 880
            height: 660
            stickLabel: "TESTSTICK"
            rekordboxPath: "/nonexistent/TESTSTICK/PIONEER"
        }
    }

    function makePage() {
        var page = createTemporaryObject(pageComponent, testCase, {
            mediaController: {calls: [], detect: function() { this.calls.push("detect"); }},
        });
        verify(page !== null, "the page must instantiate");
        return page;
    }

    // The signal the controller emits when the write is done, raised
    // directly: what is being checked is the page's reaction to it, not
    // the writing, which has its own tests and needs a real stick.
    function test_aFinishedWriteReDetectsTheStick() {
        var page = makePage();
        compare(page.mediaController.calls.length, 0, "nothing is re-detected before anything is written");
        page.controller.writeFinished({written: 12, total: 12, cancelled: false, error: ""});
        verify(page.mediaController.calls.indexOf("detect") >= 0,
               "a finished write must re-detect, or the stick list keeps saying there is no Engine library");
    }

    // A cancel is the one outcome that cannot have written to the stick:
    // the library is built in a scratch directory and copied across at
    // the end, so cancelling discards it. Re-detecting there would pay
    // for a rescan that can find nothing new.
    function test_aCancelledWriteDoesNotReDetect() {
        var page = makePage();
        page.controller.writeFinished({written: 0, total: 12, cancelled: true, error: "",
                                       detail: "Stopped at your request. Nothing was created on the stick."});
        compare(page.mediaController.calls.length, 0, "a cancelled run changed nothing to re-detect");
    }

    // tests/qml/ -> tests/fixtures/one_stick_two_catalogs: a hundred
    // tracks. A create syncs the disk some thirteen times for every track
    // (15,514 fdatasync calls for the 1,161 tracks of anonymized_library,
    // counted with strace on 2026-11-14), so with that fixture this file
    // took 56 s on CI when it passed and the 60 s below ran out when the
    // disk was busy: three pipelines in a row. The words on the dialog
    // are the same for a hundred tracks.
    function fixture() {
        return decodeURIComponent(Qt.resolvedUrl("../fixtures/one_stick_two_catalogs").toString()
            .replace(/^file:\/\//, "").replace(/^\/([A-Za-z]:)/, "$1"));
    }

    // The finish dialog leads with what the user can do now, then the
    // counts as detail (#91); the line under the button opens with the
    // same first sentence. A real create, onto a copy of the fixture's
    // rekordbox export in the scratch directory.
    function test_aCreatedLibrarySaysWhatToDoNext() {
        const stick = stickFixture.rekordboxOnlyStickCopy(fixture());
        verify(stick.length > 0, "the fixture copy must be made");
        const page = createTemporaryObject(pageComponent, testCase, {
            rekordboxPath: stick + "/PIONEER",
            mediaController: {calls: [], detect: function() { this.calls.push("detect"); }},
        });
        const dialog = findChild(page, "summaryDialog");
        page.controller.create(page.rekordboxPath, 2, "TESTSTICK");
        tryVerify(() => dialog.opened, 60000, "the create must finish and open its summary");
        compare(page.controller.errorMessage, "");
        compare(findChild(dialog, "headlineLabel").text, "Engine library created.");
        compare(findChild(dialog, "guidanceLabel").text,
                "Denon players can use this stick as it is, no export from Engine DJ needed. "
                + "After you change cues or playlists in rekordbox, run Sync after Rekordbox Export to carry them over.");
        verify(/^\d+( of \d+)? tracks? created\.$/.test(findChild(dialog, "countLabel").text),
               "the counts stay, underneath: " + findChild(dialog, "countLabel").text);
        // The dialog already said "Engine library created."; the status
        // line under the page keeps the counts, once, in plain plurals.
        verify(/^\d+ tracks? and \d+ cues? copied(; \d+ tracks? skipped \(no local file\))?\.$/
                   .test(page.controller.statusMessage),
               "status line: " + page.controller.statusMessage);
        dialog.close();
    }

    // A failed or cancelled create has nothing to lead with: no headline.
    function test_aFailedWriteHasNoHeadline() {
        const page = makePage();
        page.controller.writeFinished({written: 0, total: 12, unit: "tracks", verb: "created", cancelled: false,
                                       headline: "", guidance: "",
                                       error: "filesystem error: No space left on device"});
        const dialog = findChild(page, "summaryDialog");
        tryVerify(() => dialog.opened, 2000);
        compare(findChild(dialog, "countLabel").visible, true, "a label that can be seen at all");
        compare(findChild(dialog, "headlineLabel").visible, false);
        compare(findChild(dialog, "guidanceLabel").visible, false);
        compare(findChild(dialog, "countLabel").text, "0 of 12 tracks created.");
        dialog.close();
    }

    // A failure is not a cancel. The copy onto the stick is one recursive
    // copy, so a full stick or an I/O error part way leaves a partial
    // Engine Library folder there. Skip the re-detect and the list keeps
    // offering to create one, while the retry is refused by the creator's
    // own "already exists" check.
    function test_aFailedWriteStillReDetects() {
        var page = makePage();
        page.controller.writeFinished({written: 0, total: 12, cancelled: false,
                                       error: "filesystem error: No space left on device"});
        verify(page.mediaController.calls.indexOf("detect") >= 0,
               "a failed run may have left a partial library on the stick");
    }
}
