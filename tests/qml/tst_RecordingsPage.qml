// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtTest
import SeabassGui
import "Breadcrumb.js" as Breadcrumb

// Clean Up Recordings: the page, with a QtObject standing in for
// RecordingsController (same properties, signals and calls), and the
// Housekeeping hub's card for it, with a stand-in for the quick summary.
// What moves and what stays is the use case's to prove
// (clean_up_recordings_test); this is what the page says and asks.
TestCase {
    id: testCase
    name: "RecordingsPage"
    width: 1000
    height: 700
    visible: true
    when: windowShown

    readonly property double gib: 1024 * 1024 * 1024

    Component {
        id: fakeControllerComponent
        QtObject {
            id: fake
            property var calls: []
            property bool listing: false
            property bool working: false
            readonly property bool busy: fake.listing || fake.working
            property bool listed: true
            property int recordingCount: rows.count
            property double totalBytes: 0
            property double stickTotalBytes: 58 * 1024 * 1024 * 1024
            property double stickFreeBytes: 11.3 * 1024 * 1024 * 1024
            property int selectedCount: 0
            property double selectedBytes: 0
            property var leftAlone: []
            property var unreadableFolders: []
            property int filesDone: 0
            property int filesTotal: 0
            property string currentItem: ""
            property bool cancelRequested: false
            property string errorMessage: ""
            property ListModel recordings: ListModel { id: rows }
            signal finished(var summary)
            signal lockRefused(var holder)

            function recount() {
                let count = 0;
                let bytes = 0;
                let total = 0;
                for (let i = 0; i < rows.count; ++i) {
                    total += rows.get(i).sizeBytes;
                    if (rows.get(i).included) {
                        count++;
                        bytes += rows.get(i).sizeBytes;
                    }
                }
                fake.selectedCount = count;
                fake.selectedBytes = bytes;
                fake.totalBytes = total;
            }
            function load(label, rekordbox, engine) { fake.calls.push("load:" + label + "|" + rekordbox + "|" + engine); }
            function setIncluded(row, included) {
                fake.calls.push("include:" + row + ":" + included);
                rows.setProperty(row, "included", included);
                fake.recount();
            }
            function setAllIncluded(included) {
                fake.calls.push("all:" + included);
                for (let i = 0; i < rows.count; ++i) {
                    rows.setProperty(i, "included", included);
                }
                fake.recount();
            }
            function deleteSelected() { fake.calls.push("delete"); }
            function cancel() { fake.calls.push("cancel"); }
        }
    }

    Component {
        id: pageComponent
        RecordingsPage {
            width: 980
            height: 680
            stickLabel: "WHALESHARK"
            rekordboxPath: "/media/u/WHALESHARK/PIONEER"
            enginePath: "/media/u/WHALESHARK/Engine Library"
        }
    }

    Component {
        id: stackComponent
        StackView { width: testCase.width; height: testCase.height }
    }
    Component {
        id: fillerComponent
        Item {}
    }
    Component {
        id: spyComponent
        SignalSpy {}
    }

    function addRecordings(controller) {
        const rows = controller.recordings;
        rows.append({path: "/media/u/WHALESHARK/Sessions/Milkshaken.wav", fileName: "Milkshaken.wav",
                     folderName: "Sessions", source: "engine", sizeBytes: 2239137764,
                     modified: new Date(2026, 4, 7, 20, 6), durationSeconds: 12693.5, included: false});
        rows.append({path: "/media/u/WHALESHARK/Sessions/Session-0002.wav", fileName: "Session-0002.wav",
                     folderName: "Sessions", source: "engine", sizeBytes: 111384620,
                     modified: new Date(2026, 6, 12, 2, 30), durationSeconds: 631.4, included: false});
        rows.append({path: "/media/u/WHALESHARK/PIONEER REC/REC001.WAV", fileName: "REC001.WAV",
                     folderName: "PIONEER REC", source: "pioneer", sizeBytes: 457387052,
                     modified: new Date(2017, 0, 1, 1, 0), durationSeconds: -1, included: false});
        controller.recount();
    }

    // The stack first: temporary objects go in the order they were made,
    // so the page is gone before the stand-in it binds to.
    function makePage(setup) {
        const stack = createTemporaryObject(stackComponent, testCase);
        const controller = createTemporaryObject(fakeControllerComponent, testCase);
        if (setup) {
            setup(controller);
        }
        // Home, then the Housekeeping hub, then the page.
        stack.push(fillerComponent, {}, StackView.Immediate);
        stack.push(fillerComponent, {}, StackView.Immediate);
        const page = stack.push(pageComponent, {controller: controller}, StackView.Immediate);
        verify(page !== null);
        waitForRendering(page);
        return page;
    }

    function saveScreenshot(item, name) {
        if (!screenshotDir || screenshotDir.length === 0) return;
        grabImage(item).save(screenshotDir + "/" + name + ".png");
    }

    function row(page, index) {
        const list = findChild(page, "recordingsList");
        list.positionViewAtIndex(index, ListView.Contain);
        return findChild(list, "recordingRow" + index);
    }

    readonly property double total: 2239137764 + 111384620 + 457387052

    function test_listShowsEveryRecordingWithWhatItIs() {
        const page = makePage(addRecordings);
        compare(page.controller.calls[0], "load:WHALESHARK|/media/u/WHALESHARK/PIONEER|/media/u/WHALESHARK/Engine Library");
        compare(findChild(page, "recordingsList").count, 3);

        const first = row(page, 0);
        compare(findChild(first, "fileNameLabel").text, "Milkshaken.wav");
        const details = findChild(first, "detailsLabel").text;
        verify(details.indexOf("Sessions (Engine OS)") === 0, details);
        verify(details.indexOf("3:31:34") > 0, "the length, h:mm:ss: " + details);
        compare(findChild(first, "sizeLabel").text, Theme.humanBytes(2239137764));

        // No length known: folder and date only, nothing where a length
        // would go. Compared whole, with the date rendered by the call the
        // page makes on the same Date, so the expectation carries this
        // machine's locale and time zone rather than one of its own. (The
        // search for "0:00" this replaces failed on CI, where the C locale
        // writes the date as "1 Jan 2017 01:00:00".)
        const pioneer = row(page, 2);
        const stamp = new Date(2017, 0, 1, 1, 0);
        compare(findChild(pioneer, "detailsLabel").text,
                "PIONEER REC (Pioneer) · " + stamp.toLocaleString(Qt.locale(), Locale.ShortFormat));

        compare(findChild(page, "totalLabel").text,
                Theme.humanBytes(total) + " in 3 recordings, stick has " + Theme.humanBytes(11.3 * gib) + " free");
        compare(findChild(page, "emptyLabel").visible, false);
        compare(findChild(page, "leftAloneLabel").visible, false);
        compare(findChild(page, "copyButton"), null, "nothing is copied: there is no copy action");

        const crumb = Breadcrumb.read(page);
        compare(crumb.stick, "WHALESHARK");
        compare(crumb.middle, "Housekeeping");
        compare(crumb.title, "Clean Up Recordings");
        saveScreenshot(page, "recordings-page");
    }

    // Nothing is ticked until the DJ ticks it, and Delete waits for that.
    function test_nothingStartsTickedAndDeleteWaitsForATick() {
        const page = makePage(addRecordings);
        for (let i = 0; i < 3; ++i) {
            compare(findChild(row(page, i), "includeBox").checked, false, "row " + i + " starts unticked");
        }
        compare(page.controller.selectedCount, 0);
        compare(findChild(page, "selectedLabel").text, "0 selected (" + Theme.humanBytes(0) + ")");
        compare(findChild(page, "deleteButton").enabled, false, "nothing ticked, nothing to delete");

        mouseClick(findChild(row(page, 1), "includeBox"));
        compare(page.controller.calls[page.controller.calls.length - 1], "include:1:true");
        compare(page.controller.selectedCount, 1);
        verify(findChild(page, "deleteButton").enabled);
    }

    function test_selectAllAndDeselectAll() {
        const page = makePage(addRecordings);
        const del = findChild(page, "deleteButton");
        // The action sits in the row with Select All, a plain button like
        // Delete Selected Files on Delete Orphaned Files.
        compare(del.text, "Delete Selected Recordings");
        compare(del.highlighted, false);
        compare(del.parent, findChild(page, "selectAllButton").parent);
        mouseClick(findChild(page, "selectAllButton"));
        compare(page.controller.calls[page.controller.calls.length - 1], "all:true");
        compare(page.controller.selectedCount, 3);
        compare(findChild(page, "selectedLabel").text, "3 selected (" + Theme.humanBytes(total) + ")");
        verify(del.enabled);
        verify(findChild(row(page, 2), "includeBox").checked);

        mouseClick(findChild(page, "selectNoneButton"));
        compare(findChild(page, "selectNoneButton").text, "Deselect All");
        compare(page.controller.calls[page.controller.calls.length - 1], "all:false");
        compare(page.controller.selectedCount, 0);
        compare(del.enabled, false);
        compare(findChild(row(page, 0), "includeBox").checked, false);
    }

    // The space bar Clean Up Duplicates shows, fed by the ticks.
    function test_theSpaceBarFollowsTheTicks() {
        const page = makePage(addRecordings);
        const bar = findChild(page, "spaceBar");
        verify(bar !== null);
        verify(bar.known, "the stick's size is known, so the bar shows");
        verify(findChild(page, "spaceBarFrame").visible);
        compare(bar.totalBytes, 58 * gib);
        compare(bar.freeBytes, 11.3 * gib);
        compare(bar.reclaimableBytes, total, "everything listed could be freed");
        compare(bar.reclaimBytes, 0, "nothing ticked, nothing freed");
        verify(findChild(bar, "reclaimCaption").text.indexOf("across every recording below") > 0,
               findChild(bar, "reclaimCaption").text);

        mouseClick(findChild(row(page, 0), "includeBox"));
        compare(bar.reclaimBytes, 2239137764, "the ticked recording's bytes");
        verify(findChild(bar, "reclaimCaption").text.indexOf("the recordings you've ticked") > 0,
               findChild(bar, "reclaimCaption").text);
        mouseClick(findChild(row(page, 2), "includeBox"));
        compare(bar.reclaimBytes, 2239137764 + 457387052);
        mouseClick(findChild(page, "selectAllButton"));
        compare(bar.reclaimBytes, total);
        saveScreenshot(page, "recordings-page-all-ticked");
        mouseClick(findChild(page, "selectNoneButton"));
        compare(bar.reclaimBytes, 0);
    }

    function test_deleteAsksFirstAndSaysItIsFinal() {
        const page = makePage(addRecordings);
        mouseClick(findChild(page, "selectAllButton"));
        mouseClick(findChild(page, "deleteButton"));
        compare(page.controller.calls.indexOf("delete"), -1, "nothing is deleted before the answer");
        const dialog = findChild(page, "confirmDeleteDialog");
        tryVerify(() => dialog.opened);
        compare(dialog.title, "Delete 3 Recordings?");
        compare(dialog.headline, "This deletes 3 recordings from WHALESHARK and frees " + Theme.humanBytes(total) + ".");
        compare(dialog.detailText, "They will be gone for good.");
        compare(dialog.acceptText, "Delete");
        saveScreenshot(page, "recordings-confirm-delete");
        dialog.reject();
        compare(page.controller.calls.indexOf("delete"), -1, "Cancel deletes nothing");
        // Gone, not closing: a modal still fading out takes the next click.
        tryVerify(() => !dialog.visible && !dialog.opened, 2000, "the confirmation goes away");

        mouseClick(findChild(page, "deleteButton"));
        tryVerify(() => dialog.opened);
        dialog.accept();
        compare(page.controller.calls[page.controller.calls.length - 1], "delete");
    }

    function test_oneRecordingIsNamedInTheSingular() {
        const page = makePage(addRecordings);
        mouseClick(findChild(row(page, 1), "includeBox"));
        mouseClick(findChild(page, "deleteButton"));
        const dialog = findChild(page, "confirmDeleteDialog");
        tryVerify(() => dialog.opened);
        compare(dialog.title, "Delete 1 Recording?");
        compare(dialog.headline, "This deletes 1 recording from WHALESHARK and frees " + Theme.humanBytes(111384620) + ".");
        dialog.reject();
    }

    function test_whatIsNotARecordingIsNamed() {
        const page = makePage(controller => {
            addRecordings(controller);
            controller.leftAlone = [{fileName: "notes.txt", folderName: "Sessions", reason: "not an audio file"},
                                    {fileName: "older", folderName: "Sessions", reason: "a folder"}];
        });
        const label = findChild(page, "leftAloneLabel");
        verify(label.visible);
        compare(label.text, "Left alone, not recordings: notes.txt in Sessions, older in Sessions.");
    }

    function test_aStickWithoutRecordings() {
        const page = makePage(null);
        compare(findChild(page, "recordingsList").count, 0);
        verify(findChild(page, "emptyLabel").visible);
        compare(findChild(page, "emptyLabel").text, "No recordings on this stick.");
        compare(findChild(page, "deleteButton").enabled, false);
        compare(findChild(page, "selectAllButton").enabled, false);
        compare(findChild(page, "spaceBarFrame").visible, false);
    }

    function test_theRunShowsProgressAndCanBeCancelled() {
        const page = makePage(addRecordings);
        mouseClick(findChild(page, "selectAllButton"));
        page.controller.filesTotal = 3;
        page.controller.filesDone = 1;
        page.controller.currentItem = "Deleting Session-0002.wav";
        page.controller.working = true;
        waitForRendering(page);
        compare(findChild(page, "unitsLabel").text, "1 / 3 recordings");
        compare(findChild(page, "currentItemLabel").text, "Deleting Session-0002.wav");
        compare(findChild(page, "deleteButton").enabled, false);
        saveScreenshot(page, "recordings-deleting");
        mouseClick(findChild(page, "cancelButton"));
        compare(page.controller.calls[page.controller.calls.length - 1], "cancel");
    }

    function test_theSummaryAfterARun() {
        const page = makePage(addRecordings);
        page.controller.finished({written: 2, total: 3, unit: "recordings", verb: "deleted",
                                  cancelled: false, error: "",
                                  warning: "One recording was not deleted: REC001.WAV (no longer on the stick)."});
        const dialog = findChild(page, "summaryDialog");
        tryVerify(() => dialog.opened);
        compare(findChild(dialog, "countLabel").text, "2 of 3 recordings deleted.");
        verify(findChild(dialog, "detailLabel").text.indexOf("REC001.WAV") >= 0);
        saveScreenshot(page, "recordings-summary");
        dialog.close();
    }

    // ---- the Housekeeping hub's card --------------------------------

    Component {
        id: probeComponent
        QtObject {
            property var summary: ({count: 0, bytes: 0, sources: []})
            property int asked: 0
            function summarize(rekordbox, engine) { asked++; return summary; }
        }
    }
    Component {
        id: hubComponent
        DuplicatesHubPage {
            stickLabel: "WHALESHARK"
            rekordboxPath: "/nonexistent/WHALESHARK/PIONEER"
            enginePath: "/nonexistent/WHALESHARK/Engine Library"
        }
    }
    Component {
        id: registryComponent
        QtObject {
            property var lockedByOther: []
            function libraryIdForPath(path) { return "lib-whaleshark"; }
            function refreshLocks() {}
            function lockHolder(id) { return ({}); }
            function removeLock(id) {}
        }
    }

    function makeHub(summary, lockedByOther) {
        const stack = createTemporaryObject(stackComponent, testCase);
        const probe = createTemporaryObject(probeComponent, testCase, {summary: summary});
        const registry = createTemporaryObject(registryComponent, testCase, {lockedByOther: lockedByOther || []});
        stack.push(fillerComponent, {}, StackView.Immediate);
        const hub = stack.push(hubComponent, {recordingsProbe: probe, editRegistry: registry}, StackView.Immediate);
        waitForRendering(hub);
        verify(probe.asked > 0, "the hub asks for the summary when it opens");
        return hub;
    }

    function test_hubCardSaysWhatIsThere_data() {
        const g = 1024 * 1024 * 1024;
        return [
            {tag: "engine and pioneer", summary: {count: 3, bytes: 6.2 * g, sources: ["engine", "pioneer"]},
             subtitle: "3 recordings, " + Theme.humanBytes(6.2 * g) + ", from Engine OS and a Pioneer deck", enabled: true},
            {tag: "one alphatheta", summary: {count: 1, bytes: 0.5 * g, sources: ["alphatheta"]},
             subtitle: "1 recording, " + Theme.humanBytes(0.5 * g) + ", from an AlphaTheta deck", enabled: true},
            {tag: "all three", summary: {count: 4, bytes: 2 * g, sources: ["engine", "pioneer", "alphatheta"]},
             subtitle: "4 recordings, " + Theme.humanBytes(2 * g)
                       + ", from Engine OS, a Pioneer deck and an AlphaTheta deck", enabled: true},
            {tag: "none", summary: {count: 0, bytes: 0, sources: []},
             subtitle: "No recordings on this stick", enabled: false},
            {tag: "unreadable", summary: {count: 0, bytes: 0, sources: [], unreadable: true},
             subtitle: "Could not read the recording folders on this stick", enabled: false},
        ];
    }

    function test_hubCardSaysWhatIsThere(data) {
        const hub = makeHub(data.summary);
        const card = findChild(hub, "recordingsCard");
        verify(card !== null);
        compare(card.cardTitle, "Clean Up Recordings");
        compare(card.cardSubtitle, data.subtitle);
        compare(card.enabled, data.enabled);
        compare(card.readOnly, false);
        if (data.tag === "engine and pioneer") {
            saveScreenshot(hub, "housekeeping-hub-recordings");
        }
    }

    function test_hubCardOpensThePage() {
        const hub = makeHub({count: 2, bytes: 1000, sources: ["engine"]});
        const spy = createTemporaryObject(spyComponent, testCase, {target: hub, signalName: "recordingsRequested"});
        mouseClick(findChild(hub, "recordingsCard"));
        compare(spy.count, 1);
        compare(spy.signalArguments[0][0], "WHALESHARK");
    }

    function test_hubCardIsReadOnlyWhileAnotherInstanceEdits() {
        const hub = makeHub({count: 2, bytes: 1000, sources: ["engine"]}, ["lib-whaleshark"]);
        const card = findChild(hub, "recordingsCard");
        compare(card.readOnly, true);
        const spy = createTemporaryObject(spyComponent, testCase, {target: hub, signalName: "recordingsRequested"});
        mouseClick(card);
        compare(spy.count, 0, "a read-only card does not open the page");
    }
}
