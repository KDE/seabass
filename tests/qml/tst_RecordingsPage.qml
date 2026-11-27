// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtTest
import SeabassGui
import "Breadcrumb.js" as Breadcrumb

// Clean Up Recordings: the page, with a QtObject standing in for
// RecordingsController (same properties, signals and calls). Its card
// on the home is tst_StickToolCards.qml's.
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
            property string stickRoot: "/media/u/TESTSTICK"
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
            function deleteSelected() {
                fake.calls.push("delete");
                testCase.events.push("delete");
            }
            function selectedPaths() {
                const out = [];
                for (let i = 0; i < rows.count; ++i) {
                    if (rows.get(i).included) {
                        out.push(rows.get(i).path);
                    }
                }
                return out;
            }
            function cancel() { fake.calls.push("cancel"); }
        }
    }

    // What the page asked of the controller and the player, in order.
    property var events: []

    // KeepRecordingController's shape: the form's checks follow the real
    // rules closely enough to drive the page; the rules themselves are
    // keep_recording_test's and keep_recording_controller_test's.
    Component {
        id: fakeKeepComponent
        QtObject {
            id: fakeKeep
            property var calls: []
            property bool available: true
            property bool busy: false
            property bool cancelRequested: false
            // The page's stick; an entry that names no stick is of it.
            readonly property string homeStick: "/media/u/TESTSTICK"
            property string currentStickRoot: fakeKeep.homeStick
            property string currentWavPath: ""
            property string currentRecording: ""
            property string currentFileName: ""
            property string currentFolder: ""
            property string progressStep: ""
            property double progress: 0
            property var queued: []
            property var finishedJobs: []
            // The sticks cancelFor() was asked for, in order.
            property var cancelledSticks: []
            property int revision: 0
            property string errorMessage: ""
            property string coverPath: ""
            property string coverPreview: ""
            property int coverWidth: 0
            property int coverHeight: 0
            property string coverError: ""
            property bool coverPreparing: false
            property var outcomes: ({})
            // The kept copies, as the real one keeps them apart from the
            // outcomes: path -> {recording, fileName, folder, there (the
            // FLAC is still where and as it was kept)}.
            property var keptCopies: ({})
            // File names already in the folder.
            property var taken: []
            property bool takes: true
            property var lastForm: null
            signal finished(var outcome)

            function canKeep(path) { return path.toLowerCase().endsWith(".wav"); }
            function suggestionsFor(name, recorded) {
                fakeKeep.calls.push("suggest:" + name);
                return {artist: "DJ Kelp", title: name.replace(/\.[^.]*$/, "").replace(/_/g, " "),
                        album: "Own Recordings", genre: "Techno",
                        year: recorded && !isNaN(recorded.getTime()) ? recorded.getFullYear() : 0,
                        folder: "/home/dj/Music/Own Recordings"};
            }
            // The folder the form names, as the last name was asked for.
            property string nameFolder: ""
            function fileNameFor(artist, title, folder) {
                fakeKeep.nameFolder = folder;
                return fakeKeep.nameOf(artist, title);
            }
            function nameOf(artist, title) {
                const a = artist.trim();
                const t = title.trim();
                return (a.length > 0 && t.length > 0 ? a + " - " + t : (t.length > 0 ? t : (a.length > 0 ? a : "Recording")))
                    + ".flac";
            }
            function textProblem(field, text) {
                const names = {artist: "The artist", title: "The track name", album: "The album", genre: "The genre"};
                return /[\u0000-\u001f\u007f\u0085\u2028\u2029]/.test(text.trim())
                    ? names[field] + " cannot hold a line break, a tab or another control character." : "";
            }
            // The real rule: a kept copy first (kept while its FLAC is
            // there as it was, gone otherwise), whatever came after it;
            // else any outcome is a keep asked for and not ended kept.
            function stickOf(entry) { return entry.stickRoot ?? fakeKeep.homeStick; }
            function runsFor(stick) { return fakeKeep.busy && fakeKeep.currentStickRoot === stick; }
            function runsElsewhere(stick) { return fakeKeep.busy && fakeKeep.currentStickRoot !== stick; }
            function queuedFor(stick) { return fakeKeep.queued.filter(job => fakeKeep.stickOf(job) === stick); }
            function finishedJobsFor(stick) { return fakeKeep.finishedJobs.filter(job => fakeKeep.stickOf(job) === stick); }
            function hasWorkFor(stick) { return fakeKeep.runsFor(stick) || fakeKeep.queuedFor(stick).length > 0; }
            function keepNotesFor(stick, paths) {
                const notKept = [];
                const kept = [];
                const keptGone = [];
                for (const path of paths) {
                    const copy = fakeKeep.keptCopies[path];
                    if (copy && fakeKeep.stickOf(copy) === stick) {
                        (copy.there ? kept : keptGone).push({recording: copy.recording, fileName: copy.fileName,
                                                             folder: copy.folder});
                        continue;
                    }
                    const outcome = fakeKeep.outcomes[path];
                    if (outcome && fakeKeep.stickOf(outcome) === stick) {
                        notKept.push(outcome.recording);
                    }
                }
                return {notKept: notKept, kept: kept, keptGone: keptGone};
            }
            function folderProblem(folder) {
                if (folder.trim().length === 0) return "Choose a folder for the file.";
                if (folder.indexOf("/blocked") >= 0) return "A file is in the way of this folder.";
                return "";
            }
            function targetProblem(folder, artist, title) {
                const name = fakeKeep.nameOf(artist, title);
                return fakeKeep.taken.indexOf(name) >= 0
                    ? name + " is in this folder already. Change the artist or the track name: nothing is ever replaced."
                    : "";
            }
            function yearProblem(year) {
                const text = year.trim();
                if (text.length === 0) return "";
                const n = Number(text);
                return /^[0-9]+$/.test(text) && n >= 1900 && n <= 2100 ? "" : "The year is a number from 1900 to 2100, or empty.";
            }
            function pickCover(path) {
                fakeKeep.calls.push("cover:" + path);
                fakeKeep.coverPath = path;
                fakeKeep.coverError = "";
                fakeKeep.coverPreview = path.length > 0 ? "qrc:/qt/qml/SeabassGui/qml/icons/seabass_icon.png" : "";
                fakeKeep.coverWidth = path.length > 0 ? 1200 : 0;
                fakeKeep.coverHeight = path.length > 0 ? 1200 : 0;
            }
            function keep(form) {
                fakeKeep.calls.push("keep");
                fakeKeep.lastForm = form;
                if (!fakeKeep.takes) {
                    fakeKeep.errorMessage = "The cover is still being prepared.";
                }
                return fakeKeep.takes;
            }
            function cancelFor(stick) {
                fakeKeep.calls.push("cancel");
                fakeKeep.cancelledSticks = fakeKeep.cancelledSticks.concat([stick]);
            }
            function outcomeFor(stick, path) {
                const outcome = fakeKeep.outcomes[path];
                return outcome && fakeKeep.stickOf(outcome) === stick ? outcome : ({});
            }
            function dismissFinishedFor(stick) {
                fakeKeep.calls.push("dismiss");
                fakeKeep.finishedJobs = fakeKeep.finishedJobs.filter(job => fakeKeep.stickOf(job) !== stick);
            }
            function showInFolder(path) { fakeKeep.calls.push("show:" + path); }
            function localPath(path) { return path.startsWith("file://") ? path.substring(7) : path; }
            function folderUrl(folder) { return "file://" + folder; }
        }
    }

    Component {
        id: fakePlayerComponent
        QtObject {
            id: player
            property bool hasTrack: false
            property string currentSourceId: ""
            property var loads: []
            function loadFile(format, libraryPath, filePath, title, artist) {
                player.loads.push({format: format, libraryPath: libraryPath, filePath: filePath,
                                   title: title, artist: artist});
                player.hasTrack = true;
                player.currentSourceId = "recording:" + filePath;
            }
            function stop() {
                testCase.events.push("stop");
                player.hasTrack = false;
                player.currentSourceId = "";
            }
        }
    }

    Component {
        id: pageComponent
        RecordingsPage {
            width: 980
            height: 680
            stickLabel: "TESTSTICK"
            rekordboxPath: "/media/u/TESTSTICK/PIONEER"
            enginePath: "/media/u/TESTSTICK/Engine Library"
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
        rows.append({path: "/media/u/TESTSTICK/Sessions/Milkshaken.wav", fileName: "Milkshaken.wav",
                     folderName: "Sessions", source: "engine", sizeBytes: 2239137764,
                     modified: new Date(2026, 4, 7, 20, 6), durationSeconds: 12693.5, included: false,
                     unfinished: false, heldSeconds: -1, declaredSeconds: -1, declaredUnusable: false,
                     missingBytes: -1, holdsNoAudio: false});
        rows.append({path: "/media/u/TESTSTICK/Sessions/Session-0002.wav", fileName: "Session-0002.wav",
                     folderName: "Sessions", source: "engine", sizeBytes: 111384620,
                     modified: new Date(2026, 6, 12, 2, 30), durationSeconds: 631.4, included: false,
                     unfinished: false, heldSeconds: -1, declaredSeconds: -1, declaredUnusable: false,
                     missingBytes: -1, holdsNoAudio: false});
        rows.append({path: "/media/u/TESTSTICK/PIONEER REC/REC001.WAV", fileName: "REC001.WAV",
                     folderName: "PIONEER REC", source: "pioneer", sizeBytes: 457387052,
                     modified: new Date(2017, 0, 1, 1, 0), durationSeconds: -1, included: false,
                     unfinished: false, heldSeconds: -1, declaredSeconds: -1, declaredUnusable: false,
                     missingBytes: -1, holdsNoAudio: false});
        controller.recount();
    }

    // The stack first: temporary objects go in the order they were made,
    // so the page is gone before the stand-in it binds to.
    function makePage(setup, keepSetup, keepEnabled) {
        const stack = createTemporaryObject(stackComponent, testCase);
        const controller = createTemporaryObject(fakeControllerComponent, testCase);
        const player = createTemporaryObject(fakePlayerComponent, testCase);
        const keep = createTemporaryObject(fakeKeepComponent, testCase);
        testCase.events = [];
        if (setup) {
            setup(controller);
        }
        if (keepSetup) {
            keepSetup(keep);
        }
        // Home, then the page.
        stack.push(fillerComponent, {}, StackView.Immediate);
        const page = stack.push(pageComponent, {controller: controller, playbackController: player,
                                                keepController: keep, keepEnabled: keepEnabled !== false},
                                StackView.Immediate);
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
        compare(page.controller.calls[0], "load:TESTSTICK|/media/u/TESTSTICK/PIONEER|/media/u/TESTSTICK/Engine Library");
        compare(findChild(page, "recordingsList").count, 3);

        const first = row(page, 0);
        compare(findChild(first, "fileNameLabel").text, "Milkshaken.wav");
        const details = findChild(first, "detailsLabel").text;
        verify(details.indexOf("Sessions (Engine OS)") === 0, details);
        // 12693.5 seconds: cut to whole seconds as a clock shows them, as
        // the C++ texts count, never rounded up.
        verify(details.indexOf("3:31:33") > 0, "the length, h:mm:ss, cut to whole seconds: " + details);
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
        compare(crumb.stick, "TESTSTICK");
        compare(crumb.middle, "");
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
        // Delete Selected Files on Free Up Space.
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
        compare(dialog.headline, "This deletes 3 recordings from TESTSTICK and frees " + Theme.humanBytes(total) + ".");
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
        compare(dialog.headline, "This deletes 1 recording from TESTSTICK and frees " + Theme.humanBytes(111384620) + ".");
        dialog.reject();
    }

    // A recording plays as a plain file, in the form of the deck that made it.
    function test_playPlaysTheRecording() {
        const page = makePage(addRecordings);
        const player = page.playbackController;
        mouseClick(findChild(row(page, 0), "playButton"));
        compare(player.loads.length, 1);
        compare(player.loads[0].format, "engine", "Sessions/ is Engine OS's");
        compare(player.loads[0].libraryPath, "/media/u/TESTSTICK/Engine Library");
        compare(player.loads[0].filePath, "/media/u/TESTSTICK/Sessions/Milkshaken.wav");
        compare(player.loads[0].title, "Milkshaken.wav");
        compare(player.loads[0].artist, "Sessions (Engine OS)");

        mouseClick(findChild(row(page, 2), "playButton"));
        compare(player.loads[1].format, "rekordbox", "a REC folder is a Pioneer or AlphaTheta deck's");
        compare(player.loads[1].libraryPath, "/media/u/TESTSTICK/PIONEER");
        compare(player.loads[1].filePath, "/media/u/TESTSTICK/PIONEER REC/REC001.WAV");
        compare(player.loads[1].artist, "PIONEER REC (Pioneer)");
        compare(page.controller.selectedCount, 0, "playing ticks nothing");
    }

    // The player lets go of a recording about to be deleted before the
    // delete starts, and of nothing else.
    function test_aPlayingRecordingIsStoppedBeforeItIsDeleted_data() {
        return [
            {tag: "the ticked one plays", play: 0, tick: [0, 2], stopped: true},
            {tag: "another one plays", play: 1, tick: [0, 2], stopped: false},
            {tag: "nothing plays", play: -1, tick: [0], stopped: false},
        ];
    }
    function test_aPlayingRecordingIsStoppedBeforeItIsDeleted(data) {
        const page = makePage(addRecordings);
        if (data.play >= 0) {
            mouseClick(findChild(row(page, data.play), "playButton"));
            verify(page.playbackController.hasTrack);
        }
        for (const index of data.tick) {
            mouseClick(findChild(row(page, index), "includeBox"));
        }
        mouseClick(findChild(page, "deleteButton"));
        const dialog = findChild(page, "confirmDeleteDialog");
        tryVerify(() => dialog.opened);
        compare(testCase.events.length, 0, "nothing happens before the answer");
        dialog.accept();
        compare(testCase.events, data.stopped ? ["stop", "delete"] : ["delete"]);
        compare(page.playbackController.hasTrack, data.play >= 0 && !data.stopped);
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

    // ---- Keep this recording ----

    function openForm(page, index) {
        mouseClick(findChild(row(page, index), "keepButton"));
        const dialog = findChild(page, "keepDialog");
        tryVerify(() => dialog.opened, 3000, "the form opens");
        return dialog;
    }
    function field(page, name) {
        return findChild(findChild(page, "keepDialog"), name);
    }
    // Every item of that name under `item`, in the order they are laid out.
    function findAll(item, objectName) {
        const found = [];
        const seen = [];
        const walk = function(node) {
            if (seen.indexOf(node) >= 0) return;
            seen.push(node);
            if (node.objectName === objectName) found.push(node);
            for (let i = 0; i < node.children.length; ++i) walk(node.children[i]);
        };
        walk(item);
        return found;
    }
    function typeInto(item, text) {
        item.forceActiveFocus();
        item.selectAll();
        item.remove(0, item.length);
        item.insert(0, text);
    }

    function test_keepIsOfferedForWavsWithExperimentalOn() {
        let page = makePage(controller => {
            addRecordings(controller);
            controller.recordings.append({path: "/media/u/TESTSTICK/Sessions/Set.mp3", fileName: "Set.mp3",
                                          folderName: "Sessions", source: "engine", sizeBytes: 1000,
                                          modified: new Date(2026, 1, 1), durationSeconds: 10, included: false,
                                          unfinished: false, heldSeconds: -1, declaredSeconds: -1, declaredUnusable: false,
                     missingBytes: -1, holdsNoAudio: false});
        });
        for (let i = 0; i < 3; ++i) {
            verify(findChild(row(page, i), "keepButton").visible, "row " + i + " is a WAV and can be kept");
        }
        compare(findChild(row(page, 3), "keepButton").visible, false, "an MP3 is not offered: the encoder reads WAVs");
        compare(findChild(row(page, 0), "keepButton").text, "Save as FLAC...");
        compare(findChild(row(page, 0), "keepButton").ToolTip.text,
                "Save this recording on this computer as a FLAC file with tags and a cover");

        page = makePage(addRecordings, null, false);
        compare(findChild(row(page, 0), "keepButton").visible, false, "Experimental off: nothing to keep");
        compare(findChild(page, "keepProgress").visible, false, "and no strip");

        page = makePage(addRecordings, keep => { keep.available = false; });
        compare(findChild(row(page, 0), "keepButton").visible, false, "a build that cannot encode offers nothing");
    }

    // Experimental off, the page is the released one: nothing on it
    // speaks of saving as FLAC, the action's name included.
    function test_experimentalOffSaysNothingOfSavingAsFlac() {
        const page = makePage(addRecordings, null, false);
        const said = [];
        const walk = function(node) {
            if (node.visible === false) return;
            if (typeof node.text === "string" && /FLAC|Save as|[Kk]eep/.test(node.text)) said.push(node.text);
            for (let i = 0; i < node.children.length; ++i) walk(node.children[i]);
        };
        walk(page);
        compare(JSON.stringify(said), "[]");
        // On, the action is there under its name.
        const on = makePage(addRecordings);
        compare(findChild(row(on, 0), "keepButton").text, "Save as FLAC...");
    }

    function test_theFormSuggests() {
        const page = makePage(addRecordings);
        const dialog = openForm(page, 1);
        compare(dialog.title, "Save Recording as FLAC");
        compare(dialog.headline, "Session-0002.wav becomes a FLAC file with tags and a cover on this computer.");
        compare(dialog.detailText, "The recording on the stick stays as it is.");
        verify(page.keepController.calls.indexOf("suggest:Session-0002.wav") >= 0);
        verify(page.keepController.calls.indexOf("cover:") >= 0, "the form starts without a cover");
        compare(field(page, "keepArtistField").text, "DJ Kelp", "the artist of the last time");
        compare(field(page, "keepTitleField").text, "Session-0002", "the track name from the file name");
        compare(field(page, "keepAlbumField").text, "Own Recordings");
        compare(field(page, "keepGenreField").text, "Techno", "the genre of the last time");
        compare(field(page, "keepYearField").text, "2026", "the year the recording was made");
        compare(field(page, "keepFolderField").text, "/home/dj/Music/Own Recordings");
        compare(field(page, "keepFileNameLabel").text, "Saved as DJ Kelp - Session-0002.flac");
        compare(page.keepController.nameFolder, "/home/dj/Music/Own Recordings",
                "the name is asked for in the folder it goes to, which decides how long it may be");
        compare(field(page, "keepCoverLabel").text, "No cover");
        compare(field(page, "keepNoCoverButton").enabled, false, "no cover to take away");
        compare(field(page, "keepProblemsLabel").visible, false, "the suggestions are a form that can start");
        verify(field(page, "keepStartButton").enabled);
        verify(findChild(dialog, "experimentalBadge").visible, "the form says it is experimental");

        // A cover picked: its preview and the size it is embedded at.
        page.keepController.pickCover("/home/dj/Pictures/cover.png");
        compare(field(page, "keepCoverLabel").text, "Embedded as 1200 × 1200 pixels");
        verify(field(page, "keepNoCoverButton").enabled);
        tryVerify(() => field(page, "keepCoverPreview").status === Image.Ready);

        // One left line: every field starts where the one above it does.
        verify(waitForPolish(page.Window.window));
        const left = field(page, "keepArtistField").mapToItem(dialog.contentItem, 0, 0).x;
        for (const name of ["keepTitleField", "keepAlbumField", "keepGenreField", "keepYearField", "keepCoverFrame",
                            "keepFolderField"]) {
            compare(field(page, name).mapToItem(dialog.contentItem, 0, 0).x, left, name + " is on the line");
        }
        // The pointer off every button, so the picture shows no hover.
        mouseMove(page, 2, page.height - 2);
        saveScreenshot(page, "recordings-keep-form");
        dialog.close();
    }

    function test_theFormChecks_data() {
        return [
            {tag: "no track name", name: "keepTitleField", text: "  ", problem: "The track name is needed."},
            {tag: "a year too early", name: "keepYearField", text: "1850",
             problem: "The year is a number from 1900 to 2100, or empty."},
            {tag: "a year that is not one", name: "keepYearField", text: "20x6",
             problem: "The year is a number from 1900 to 2100, or empty."},
            {tag: "no folder", name: "keepFolderField", text: "", problem: "Choose a folder for the file."},
            {tag: "a file in the way", name: "keepFolderField", text: "/home/dj/blocked/sets",
             problem: "A file is in the way of this folder."},
            {tag: "a name that is taken", name: "keepTitleField", text: "Late Set",
             problem: "DJ Kelp - Late Set.flac is in this folder already. Change the artist or the track name: nothing is ever replaced."},
        ];
    }
    function test_theFormChecks(data) {
        const page = makePage(addRecordings, keep => { keep.taken = ["DJ Kelp - Late Set.flac"]; });
        const dialog = openForm(page, 0);
        typeInto(field(page, data.name), data.text);
        const problems = field(page, "keepProblemsLabel");
        verify(problems.visible, "the form says what is wrong");
        compare(problems.text, data.problem);
        compare(field(page, "keepStartButton").enabled, false, "and does not start");
        keyClick(Qt.Key_Return);
        compare(page.keepController.calls.indexOf("keep"), -1, "Enter starts nothing either");
        verify(dialog.opened, "the form stays open");
        if (data.tag === "a name that is taken") {
            saveScreenshot(page, "recordings-keep-form-taken");
        }
        // Put right, it can start.
        typeInto(field(page, data.name), data.name === "keepFolderField" ? "/home/dj/Music/Own Recordings"
                                         : data.name === "keepYearField" ? "" : "Other Set");
        compare(problems.visible, false);
        verify(field(page, "keepStartButton").enabled);
        dialog.close();
    }

    function test_theFormByKeyboard() {
        const page = makePage(addRecordings);
        const dialog = openForm(page, 0);
        const order = ["keepArtistField", "keepTitleField", "keepAlbumField", "keepGenreField", "keepYearField",
                       "keepCoverButton", "keepFolderField", "keepFolderButton", "keepStartButton", "keepCancelButton"];
        tryVerify(() => field(page, order[0]).activeFocus, 2000, "the form starts at the artist");
        for (let i = 1; i < order.length; ++i) {
            keyClick(Qt.Key_Tab);
            verify(field(page, order[i]).activeFocus, "Tab goes on to " + order[i]);
        }
        // Escape closes it, and nothing is kept.
        keyClick(Qt.Key_Escape);
        tryVerify(() => !dialog.visible, 2000, "Escape closes the form");
        compare(page.keepController.calls.indexOf("keep"), -1, "nothing is kept");

        // Enter in a field starts it, with what the form holds.
        openForm(page, 0);
        tryVerify(() => field(page, "keepArtistField").activeFocus);
        keyClick(Qt.Key_Tab);
        typeInto(field(page, "keepTitleField"), "Late Set");
        typeInto(field(page, "keepYearField"), "2025");
        field(page, "keepTitleField").forceActiveFocus();
        keyClick(Qt.Key_Return);
        compare(page.keepController.calls.filter(c => c === "keep").length, 1, "Enter starts it");
        const form = page.keepController.lastForm;
        compare(form.wavPath, "/media/u/TESTSTICK/Sessions/Milkshaken.wav");
        compare(form.recording, "Milkshaken.wav");
        compare(form.artist, "DJ Kelp");
        compare(form.title, "Late Set");
        compare(form.album, "Own Recordings");
        compare(form.genre, "Techno");
        compare(form.year, "2025");
        compare(form.folder, "/home/dj/Music/Own Recordings");
        tryVerify(() => !dialog.visible, 2000, "the form closes once the job is taken");
    }

    function test_aRefusedStartKeepsTheForm() {
        const page = makePage(addRecordings, keep => { keep.takes = false; });
        const dialog = openForm(page, 0);
        mouseClick(field(page, "keepStartButton"));
        compare(page.keepController.calls.filter(c => c === "keep").length, 1);
        verify(dialog.opened, "refused: the form stays");
        compare(field(page, "keepProblemsLabel").text, "The cover is still being prepared.", "and says why");
        dialog.close();
    }

    function busyKeep(keep) {
        keep.busy = true;
        keep.currentWavPath = "/media/u/TESTSTICK/Sessions/Milkshaken.wav";
        keep.currentRecording = "Milkshaken.wav";
        keep.currentFileName = "DJ Kelp - Late Set.flac";
        keep.currentFolder = "/home/dj/Music/Own Recordings";
        keep.progressStep = "checking";
        keep.progress = 0.7;
        keep.queued = [{wavPath: "/media/u/TESTSTICK/Sessions/Session-0002.wav", recording: "Session-0002.wav",
                        fileName: "DJ Kelp - Early Set.flac", folder: "/home/dj/Music/Own Recordings"}];
        keep.outcomes = {
            "/media/u/TESTSTICK/Sessions/Milkshaken.wav": {state: "running", fileName: "DJ Kelp - Late Set.flac",
                                                            folder: "/home/dj/Music/Own Recordings"},
            "/media/u/TESTSTICK/Sessions/Session-0002.wav": {state: "queued", fileName: "DJ Kelp - Early Set.flac",
                                                              folder: "/home/dj/Music/Own Recordings"},
        };
        keep.revision++;
    }

    function test_theStripShowsTheJobAndTheQueue() {
        const page = makePage(addRecordings, busyKeep);
        const strip = findChild(page, "keepProgress");
        verify(waitForPolish(page.Window.window));
        verify(strip.visible && strip.height > 0, "the strip shows while a recording is kept");
        compare(findChild(strip, "passiveProgressSummary").text,
                "Saving Milkshaken.wav as FLAC: checking, 70% (1 more waiting)");
        compare(findChild(strip, "passiveProgressBar").value, 0.7, "one bar for the whole job");
        compare(findChild(row(page, 0), "keptLabel").text, "Being saved as DJ Kelp - Late Set.flac: checking");
        compare(findChild(row(page, 1), "keptLabel").text, "Waits to be saved as DJ Kelp - Early Set.flac");
        compare(findChild(row(page, 0), "keepButton").enabled, false, "a recording in hand is not asked for again");
        compare(findChild(row(page, 1), "keepButton").enabled, false, "nor one that waits");
        verify(findChild(row(page, 2), "keepButton").enabled, "another one can join the queue");
        mouseClick(findChild(page, "selectAllButton"));
        compare(findChild(page, "deleteButton").enabled, false, "nothing is deleted while a recording is kept");
        verify(findChild(row(page, 0), "playButton").enabled, "the page stays usable");

        mouseClick(findChild(strip, "passiveProgressArea"), 10, 4);
        verify(strip.expanded, "a click opens the panel");
        verify(waitForPolish(page.Window.window));
        const steps = findAll(strip, "passiveProgressStep");
        compare(steps.map(s => s.text), ["Encoding", "Checking", "Tags and cover", "Saving"]);
        const counts = findAll(strip, "passiveProgressStepCount").filter(c => c.visible);
        compare(counts.map(c => c.text), ["70%"], "the step in hand with its count");
        const lines = findAll(strip, "passiveProgressActivityLine").map(l => l.text);
        compare(lines, ["Writing DJ Kelp - Late Set.flac in /home/dj/Music/Own Recordings",
                        "Waiting: Session-0002.wav, as DJ Kelp - Early Set.flac"], "what is in hand, and what waits");
        compare(findChild(strip, "passiveProgressNote").text,
                "The recording on the stick is only read. The work goes on when you leave this page.");
        saveScreenshot(page, "recordings-keep-progress-open");
        mouseClick(findChild(strip, "passiveProgressCancel"));
        compare(page.keepController.calls[page.keepController.calls.length - 1], "cancel", "Cancel by mouse");
        compare(page.keepController.cancelledSticks, ["/media/u/TESTSTICK"], "of this stick");
        compare(findChild(page, "otherStickKeepLabel").visible, false, "no other stick's work to speak of");
    }

    // The keep controller is the window's, for every stick: another
    // stick's job in hand is not this page's, and one plain line says it
    // is running, so that a job of this stick is seen to wait for it.
    function test_anotherSticksWorkIsSaid() {
        const page = makePage(addRecordings, keep => {
            keep.busy = true;
            keep.currentStickRoot = "/media/u/OTHERSTICK";
            keep.currentWavPath = "/media/u/OTHERSTICK/Sessions/Elsewhere.wav";
            keep.currentRecording = "Elsewhere.wav";
            keep.currentFileName = "Elsewhere.flac";
            keep.currentFolder = "/home/dj/Music/Own Recordings";
            keep.progressStep = "encoding";
            keep.progress = 0.4;
            keep.queued = [{stickRoot: "/media/u/OTHERSTICK", wavPath: "/media/u/OTHERSTICK/Sessions/Later.wav",
                            recording: "Later.wav", fileName: "Later.flac", folder: "/home/dj/Music/Own Recordings"},
                           {wavPath: "/media/u/TESTSTICK/Sessions/Session-0002.wav", recording: "Session-0002.wav",
                            fileName: "DJ Kelp - Early Set.flac", folder: "/home/dj/Music/Own Recordings"}];
            keep.finishedJobs = [{state: "kept", stickRoot: "/media/u/OTHERSTICK", wavPath: "/media/u/OTHERSTICK/Sessions/Before.wav",
                                  recording: "Before.wav", fileName: "Before.flac", folder: "/f", flacPath: "/f/Before.flac",
                                  leftBehind: "", message: ""}];
            keep.outcomes = {
                "/media/u/TESTSTICK/Sessions/Session-0002.wav": {state: "queued", fileName: "DJ Kelp - Early Set.flac",
                                                                  folder: "/home/dj/Music/Own Recordings"},
            };
            keep.revision++;
        });
        verify(waitForPolish(page.Window.window));
        const other = findChild(page, "otherStickKeepLabel");
        verify(other.visible && other.height > 0, "the line about another stick's work shows");
        compare(other.text, "A recording from another stick is being saved as FLAC. What you save from this stick "
                + "waits until that has ended.");
        const strip = findChild(page, "keepProgress");
        verify(strip.visible, "the strip shows this stick's job that waits");
        compare(findChild(strip, "passiveProgressSummary").text, "1 recording(s) waiting to be saved as FLAC");
        compare(findChild(strip, "passiveProgressBar").value, 0, "nothing of this stick's is under way yet");
        mouseClick(findChild(strip, "passiveProgressArea"), 10, 4);
        verify(waitForPolish(page.Window.window));
        compare(findAll(strip, "passiveProgressActivityLine").map(l => l.text),
                ["Waiting: Session-0002.wav, as DJ Kelp - Early Set.flac"],
                "this stick's job only, nothing of the other stick's");
        compare(findChild(row(page, 1), "keptLabel").text, "Waits to be saved as DJ Kelp - Early Set.flac");
        mouseClick(findChild(page, "selectAllButton"));
        compare(findChild(page, "deleteButton").enabled, false, "a job of this stick waits: nothing is deleted");
        verify(strip.cancellable, "this stick's waiting job can be dropped");
        mouseClick(findChild(strip, "passiveProgressCancel"));
        compare(page.keepController.cancelledSticks, ["/media/u/TESTSTICK"], "the cancel is of this stick");

        // Only the other stick has work: this stick's strip has nothing to
        // show, its Delete is free, and the line stays.
        page.keepController.queued = [page.keepController.queued[0]];
        page.keepController.revision++;
        tryVerify(() => !strip.visible, 2000, "no job of this stick: no strip");
        verify(waitForPolish(page.Window.window));
        verify(other.visible, "the line about the other stick stays");
        verify(findChild(page, "deleteButton").enabled, "another stick's work does not hold off this stick's Delete");
        saveScreenshot(page, "recordings-keep-another-stick");

        // The other stick's work ends: the line goes.
        page.keepController.busy = false;
        page.keepController.queued = [];
        page.keepController.revision++;
        tryVerify(() => !other.visible, 2000, "nothing runs: the line goes");
    }

    // Two pieces of a step's count: the steps before are done, the one in
    // hand is now, those after come later.
    function test_theStepsFollowTheJob_data() {
        return [
            {tag: "space", step: "space", states: ["now", "later", "later", "later"], word: "encoding"},
            {tag: "encoding", step: "encoding", states: ["now", "later", "later", "later"], word: "encoding"},
            {tag: "checking", step: "checking", states: ["done", "now", "later", "later"], word: "checking"},
            {tag: "tags", step: "tags", states: ["done", "done", "now", "later"], word: "tags and cover"},
            {tag: "placing", step: "placing", states: ["done", "done", "done", "now"], word: "saving"},
        ];
    }
    function test_theStepsFollowTheJob(data) {
        const page = makePage(addRecordings, keep => { busyKeep(keep); keep.queued = []; keep.progressStep = data.step; });
        const strip = findChild(page, "keepProgress");
        compare(strip.steps.map(s => s.state), data.states);
        compare(findChild(strip, "passiveProgressSummary").text, "Saving Milkshaken.wav as FLAC: " + data.word + ", 70%");
    }

    function test_cancelByKeyboard() {
        const page = makePage(addRecordings, busyKeep);
        const strip = findChild(page, "keepProgress");
        findChild(page, "selectNoneButton").forceActiveFocus();
        for (let i = 0; i < 40 && !strip.activeFocus; ++i) {
            keyClick(Qt.Key_Tab);
        }
        verify(strip.activeFocus, "Tab reaches the strip");
        keyClick(Qt.Key_Return);
        verify(strip.expanded, "Return opens it");
        keyClick(Qt.Key_Escape);
        compare(strip.expanded, false, "Escape closes it");
        keyClick(Qt.Key_Space);
        verify(strip.expanded, "Space opens it too");
        keyClick(Qt.Key_Tab);
        verify(findChild(strip, "passiveProgressCancel").activeFocus, "Tab goes on to Cancel");
        keyClick(Qt.Key_Return);
        compare(page.keepController.calls[page.keepController.calls.length - 1], "cancel", "Return on it cancels");
    }

    function keptKeep(keep) {
        const done = {state: "kept", wavPath: "/media/u/TESTSTICK/Sessions/Milkshaken.wav",
                      recording: "Milkshaken.wav", fileName: "DJ Kelp - Late Set.flac",
                      folder: "/home/dj/Music/Own Recordings",
                      flacPath: "/home/dj/Music/Own Recordings/DJ Kelp - Late Set.flac", leftBehind: "", message: ""};
        keep.finishedJobs = [done];
        keep.outcomes = {"/media/u/TESTSTICK/Sessions/Milkshaken.wav": done};
        keep.progress = 1;
        keep.revision++;
    }

    function test_whenDone() {
        const page = makePage(addRecordings, keptKeep);
        const strip = findChild(page, "keepProgress");
        verify(strip.visible, "the strip says it is done");
        compare(findChild(strip, "passiveProgressSummary").text, "Saved Milkshaken.wav as DJ Kelp - Late Set.flac");
        compare(findChild(strip, "passiveProgressBar").value, 1, "the bar is full");
        compare(strip.cancellable, false, "nothing to cancel");
        const kept = findChild(row(page, 0), "keptLabel");
        verify(kept.visible);
        compare(kept.text, "Saved as DJ Kelp - Late Set.flac in /home/dj/Music/Own Recordings");
        const show = findChild(row(page, 0), "showKeptButton");
        verify(show.visible);
        compare(show.text, "Show in Folder");
        compare(findChild(row(page, 1), "showKeptButton").visible, false, "only on the row that was kept");
        verify(findChild(row(page, 0), "keepButton").enabled, "it can be kept again, under another name");
        verify(findChild(page, "deleteButton").enabled === false, "nothing ticked");
        mouseClick(findChild(page, "selectAllButton"));
        verify(findChild(page, "deleteButton").enabled, "Delete is back once nothing runs: the WAV is never deleted here");
        saveScreenshot(page, "recordings-keep-done");

        mouseClick(show);
        compare(page.keepController.calls[page.keepController.calls.length - 1],
                "show:/home/dj/Music/Own Recordings/DJ Kelp - Late Set.flac");
        mouseClick(findChild(strip, "passiveProgressAction_show"));
        compare(page.keepController.calls[page.keepController.calls.length - 1],
                "show:/home/dj/Music/Own Recordings/DJ Kelp - Late Set.flac", "the strip shows the folder too");
        mouseClick(findChild(strip, "passiveProgressAction_close"));
        compare(page.keepController.calls[page.keepController.calls.length - 1], "dismiss");
        tryVerify(() => !strip.visible, 2000, "closed, the strip goes");
        verify(kept.visible, "the row still says where it was kept");
    }

    // Text a tag cannot hold is said in the form, which does not start.
    function test_theFormRefusesControlCharacters_data() {
        return [
            {tag: "a tab in the artist", name: "keepArtistField", text: "DJ\tKelp",
             problem: "The artist cannot hold a line break, a tab or another control character."},
            {tag: "a line separator in the album", name: "keepAlbumField", text: "Own Recordings",
             problem: "The album cannot hold a line break, a tab or another control character."},
            {tag: "an escape in the genre", name: "keepGenreField", text: "Tech\u001bno",
             problem: "The genre cannot hold a line break, a tab or another control character."},
        ];
    }
    function test_theFormRefusesControlCharacters(data) {
        const page = makePage(addRecordings);
        const dialog = openForm(page, 0);
        field(page, data.name).text = data.text;
        const problems = field(page, "keepProblemsLabel");
        verify(problems.visible, "the form says what is wrong");
        compare(problems.text, data.problem);
        compare(field(page, "keepStartButton").enabled, false, "and does not start");
        field(page, data.name).text = "Plain";
        compare(problems.visible, false);
        dialog.close();
    }

    // The confirmation names, by file name, every ticked recording a keep
    // was asked for and did not end kept, apart from what is deleted; one
    // that was kept says where.
    function test_deleteNamesWhatWasNotKept() {
        const page = makePage(addRecordings, keep => {
            keep.outcomes = {
                "/media/u/TESTSTICK/Sessions/Milkshaken.wav": {state: "failed", recording: "Milkshaken.wav",
                                                                fileName: "A.flac", folder: "/f", message: "no room"},
                "/media/u/TESTSTICK/Sessions/Session-0002.wav": {state: "dropped", recording: "Session-0002.wav",
                                                                  fileName: "B.flac", folder: "/f", message: ""},
                "/media/u/TESTSTICK/PIONEER REC/REC001.WAV": {state: "kept", recording: "REC001.WAV",
                                                               fileName: "C.flac", folder: "/home/dj/Music/Own Recordings"},
            };
            keep.keptCopies = {
                "/media/u/TESTSTICK/PIONEER REC/REC001.WAV": {recording: "REC001.WAV", fileName: "C.flac",
                                                               folder: "/home/dj/Music/Own Recordings", there: true},
            };
            keep.revision++;
        });
        mouseClick(findChild(page, "selectAllButton"));
        mouseClick(findChild(page, "deleteButton"));
        const dialog = findChild(page, "confirmDeleteDialog");
        tryVerify(() => dialog.opened);
        const notKept = findChild(dialog, "notKeptLabel");
        verify(notKept.visible, "the recordings not kept are named");
        compare(notKept.text, "Not saved as FLAC, though you asked for it: Milkshaken.wav, Session-0002.wav.");
        compare(dialog.detailText, "They will be gone for good.", "apart from the sentence about deleting");
        compare(findChild(dialog, "keptNotesLabel").text,
                "Saved as FLAC on this computer: REC001.WAV as C.flac in /home/dj/Music/Own Recordings.");
        saveScreenshot(page, "recordings-confirm-delete-not-kept");
        dialog.reject();
        tryVerify(() => !dialog.visible && !dialog.opened, 2000);

        // Only the kept one ticked: nothing is named as not kept.
        mouseClick(findChild(page, "selectNoneButton"));
        mouseClick(findChild(row(page, 2), "includeBox"));
        mouseClick(findChild(page, "deleteButton"));
        tryVerify(() => dialog.opened);
        compare(notKept.visible, false, "nothing ticked was left unkept");
        verify(findChild(dialog, "keptNotesLabel").visible);
        dialog.reject();
    }

    // A cancel drops the jobs that waited: each row says so, and the
    // strip names them with the one that was stopped.
    function test_aCancelNamesWhatItDropped() {
        const page = makePage(addRecordings, keep => {
            const dropped = (path, name) => ({state: "dropped", wavPath: path, recording: name, fileName: name + ".flac",
                                              folder: "/f", flacPath: "", leftBehind: "", message: ""});
            const one = dropped("/media/u/TESTSTICK/Sessions/Session-0002.wav", "Session-0002.wav");
            const two = dropped("/media/u/TESTSTICK/PIONEER REC/REC001.WAV", "REC001.WAV");
            const stopped = {state: "cancelled", wavPath: "/media/u/TESTSTICK/Sessions/Milkshaken.wav",
                             recording: "Milkshaken.wav", fileName: "M.flac", folder: "/f", flacPath: "",
                             leftBehind: "", message: ""};
            keep.finishedJobs = [one, two, stopped];
            keep.outcomes = {};
            keep.outcomes[one.wavPath] = one;
            keep.outcomes[two.wavPath] = two;
            keep.outcomes[stopped.wavPath] = stopped;
            keep.revision++;
        });
        compare(findChild(row(page, 1), "keptLabel").text, "Not saved: stopped before it started");
        compare(findChild(row(page, 2), "keptLabel").text, "Not saved: stopped before it started");
        compare(findChild(row(page, 0), "keptLabel").text, "Not saved: stopped while it was being saved");
        const strip = findChild(page, "keepProgress");
        verify(strip.visible);
        compare(findChild(strip, "passiveProgressSummary").text, "3 of 3 recording(s) not saved");
        mouseClick(findChild(strip, "passiveProgressArea"), 10, 4);
        verify(waitForPolish(page.Window.window));
        compare(findAll(strip, "passiveProgressActivityLine").map(l => l.text),
                ["Stopped: Milkshaken.wav, nothing of it was saved",
                 "Not saved, stopped before it started: 2 recording(s): Session-0002.wav, REC001.WAV"]);
        saveScreenshot(page, "recordings-keep-dropped");
    }

    // A failure stays in the strip while the next job runs.
    function test_aFailureStaysWhileTheNextRuns() {
        const page = makePage(addRecordings, keep => {
            busyKeep(keep);
            keep.finishedJobs = [{state: "failed", wavPath: "/media/u/TESTSTICK/PIONEER REC/REC001.WAV",
                                  recording: "REC001.WAV", fileName: "R.flac", folder: "/f", flacPath: "",
                                  leftBehind: "", message: "There is not enough room in /f."}];
        });
        const strip = findChild(page, "keepProgress");
        compare(findChild(strip, "passiveProgressSummary").text,
                "Saving Milkshaken.wav as FLAC: checking, 70% (1 more waiting, 1 not saved)");
        mouseClick(findChild(strip, "passiveProgressArea"), 10, 4);
        verify(waitForPolish(page.Window.window));
        const lines = findAll(strip, "passiveProgressActivityLine").map(l => l.text);
        compare(lines[lines.length - 1], "Not saved: REC001.WAV: There is not enough room in /f.");
    }

    // A cancel that could not take its file away says what was left.
    function test_aFileLeftBehindIsSaid() {
        const page = makePage(addRecordings, keep => {
            const stopped = {state: "cancelled", wavPath: "/media/u/TESTSTICK/Sessions/Milkshaken.wav",
                             recording: "Milkshaken.wav", fileName: "M.flac", folder: "/home/dj/Kept", flacPath: "",
                             leftBehind: "/home/dj/Kept/M.flac",
                             message: "The unfinished M.flac could not be removed from /home/dj/Kept (busy)."};
            keep.finishedJobs = [stopped];
            keep.outcomes = {"/media/u/TESTSTICK/Sessions/Milkshaken.wav": stopped};
            keep.revision++;
        });
        const strip = findChild(page, "keepProgress");
        compare(findChild(strip, "passiveProgressSummary").text,
                "Stopped: Milkshaken.wav was not saved, and a file was left in /home/dj/Kept");
        compare(findChild(row(page, 0), "keptLabel").text,
                "Not saved, stopped: The unfinished M.flac could not be removed from /home/dj/Kept (busy).");
    }

    // Experimental switched off while a job runs: the strip and its
    // Cancel stay until the work has ended.
    function test_theStripOutlastsExperimentalOff() {
        const page = makePage(addRecordings, busyKeep, false);
        const strip = findChild(page, "keepProgress");
        verify(strip.visible, "the strip shows while the job runs");
        verify(strip.cancellable, "and can be cancelled");
        compare(findChild(row(page, 0), "keepButton").visible, false, "nothing new is offered");
        page.keepController.busy = false;
        page.keepController.queued = [];
        tryVerify(() => !strip.visible, 2000, "once the work has ended and nothing is listed, it goes");
    }

    function test_whenItFails() {
        const page = makePage(addRecordings, keep => {
            const failed = {state: "failed", wavPath: "/media/u/TESTSTICK/Sessions/Milkshaken.wav",
                            recording: "Milkshaken.wav", fileName: "DJ Kelp - Late Set.flac",
                            folder: "/home/dj/Music/Own Recordings", flacPath: "", leftBehind: "",
                            message: "There is not enough room in /home/dj/Music/Own Recordings."};
            keep.finishedJobs = [failed];
            keep.outcomes = {"/media/u/TESTSTICK/Sessions/Milkshaken.wav": failed};
            keep.revision++;
        });
        const strip = findChild(page, "keepProgress");
        compare(findChild(strip, "passiveProgressSummary").text, "Milkshaken.wav was not saved");
        compare(findChild(strip, "passiveProgressAction_show"), null, "nothing to show");
        mouseClick(findChild(strip, "passiveProgressArea"), 10, 4);
        verify(waitForPolish(page.Window.window));
        // The reason once, on the job's own line; the note does not repeat it.
        const reason = "There is not enough room in /home/dj/Music/Own Recordings.";
        compare(findAll(strip, "passiveProgressActivityLine").map(l => l.text),
                ["Not saved: Milkshaken.wav: " + reason]);
        compare(findChild(strip, "passiveProgressNote").text, "");
        compare(findChild(row(page, 0), "keptLabel").text,
                "Not saved: There is not enough room in /home/dj/Music/Own Recordings.");
        compare(findChild(row(page, 0), "showKeptButton").visible, false);
    }

    // Kept once, then a second keep that failed: the confirmation still
    // says kept, the first FLAC being there. One whose FLAC is gone is
    // said apart, in the warning's style, and not as kept.
    function test_deleteSaysWhichKeptCopiesAreStillThere() {
        const page = makePage(addRecordings, keep => {
            keep.outcomes = {
                "/media/u/TESTSTICK/Sessions/Milkshaken.wav": {state: "failed", recording: "Milkshaken.wav",
                                                                fileName: "Again.flac", folder: "/f", message: "no room"},
                "/media/u/TESTSTICK/Sessions/Session-0002.wav": {state: "kept", recording: "Session-0002.wav",
                                                                  fileName: "B.flac", folder: "/f"},
            };
            keep.keptCopies = {
                "/media/u/TESTSTICK/Sessions/Milkshaken.wav": {recording: "Milkshaken.wav", fileName: "A.flac",
                                                                folder: "/f", there: true},
                "/media/u/TESTSTICK/Sessions/Session-0002.wav": {recording: "Session-0002.wav", fileName: "B.flac",
                                                                  folder: "/f", there: false},
            };
            keep.revision++;
        });
        mouseClick(findChild(row(page, 0), "includeBox"));
        mouseClick(findChild(row(page, 1), "includeBox"));
        mouseClick(findChild(page, "deleteButton"));
        const dialog = findChild(page, "confirmDeleteDialog");
        tryVerify(() => dialog.opened);
        compare(findChild(dialog, "notKeptLabel").visible, false, "kept once and then failed: not named as not kept");
        compare(findChild(dialog, "keptNotesLabel").text, "Saved as FLAC on this computer: Milkshaken.wav as A.flac in /f.");
        const gone = findChild(dialog, "keptGoneLabel");
        verify(gone.visible, "a kept copy that is gone is said");
        compare(gone.text, "Saved as FLAC earlier, but that file is no longer there as it was saved: Session-0002.wav as B.flac in /f.");
        compare(gone.color, Theme.danger, "in the warning's colour");
        verify(gone.font.bold, "and weight");
        saveScreenshot(page, "recordings-confirm-delete-kept-gone");
        dialog.reject();
    }

    // A recording whose header was not completed and declares no length:
    // the row says so with the audio the file holds; the form says it
    // plainly, the cause only as what usually leads there, and starts only
    // once the box is ticked, and the choice goes with the job.
    function test_anUnfinishedRecording() {
        const page = makePage(controller => {
            addRecordings(controller);
            controller.recordings.setProperty(1, "unfinished", true);
            controller.recordings.setProperty(1, "heldSeconds", 3723.4);
        });
        const details = findChild(row(page, 1), "detailsLabel").text;
        verify(details.endsWith("header not completed, the file holds 1:02:03 of audio"), details);
        verify(findChild(row(page, 0), "detailsLabel").text.indexOf("not completed") < 0, "a finished one says nothing of it");

        const dialog = openForm(page, 1);
        const notice = field(page, "keepUnfinishedNotice");
        verify(notice.visible, "the form says the header was not completed");
        compare(field(page, "keepUnfinishedLabel").text,
                "The header of this recording was not completed: it does not say how long the audio is. The file "
                + "holds 1:02:03 of audio. This happens when a player is switched off or a stick is pulled while "
                + "recording. The recording on the stick is not changed.");
        const box = field(page, "keepUnfinishedBox");
        compare(box.text, "Keep the audio the file holds");
        compare(box.checked, false, "the box starts unticked");
        compare(field(page, "keepStartButton").enabled, false, "and the form does not start without it");
        keyClick(Qt.Key_Return);
        compare(page.keepController.calls.indexOf("keep"), -1, "Enter starts nothing either");
        saveScreenshot(page, "recordings-keep-form-unfinished");
        mouseClick(box);
        verify(box.checked);
        verify(field(page, "keepStartButton").enabled, "ticked, it can start");
        mouseClick(field(page, "keepStartButton"));
        compare(page.keepController.lastForm.keepUnfinished, true, "the choice goes with the job");
        tryVerify(() => !dialog.visible, 2000);

        // Opened again for it, or for another: the box starts unticked,
        // and a finished recording shows no notice and sends no choice.
        openForm(page, 1);
        compare(field(page, "keepUnfinishedBox").checked, false, "opened again: unticked again");
        findChild(page, "keepDialog").close();
        tryVerify(() => !findChild(page, "keepDialog").visible, 2000);
        openForm(page, 0);
        compare(field(page, "keepUnfinishedNotice").visible, false, "a finished recording: no notice");
        mouseClick(field(page, "keepStartButton"));
        compare(page.keepController.lastForm.keepUnfinished, false, "and no choice sent");
    }

    // Cut short: the header declares more than the file holds. The row and
    // the form give both lengths, and the form opened for another such
    // recording right after shows that one's.
    function test_aRecordingCutShortShowsBothLengths() {
        const page = makePage(controller => {
            addRecordings(controller);
            controller.recordings.setProperty(1, "unfinished", true);
            controller.recordings.setProperty(1, "heldSeconds", 2530.2);
            controller.recordings.setProperty(1, "declaredSeconds", 3600);
            controller.recordings.setProperty(0, "unfinished", true);
            controller.recordings.setProperty(0, "heldSeconds", 61);
        });
        const details = findChild(row(page, 1), "detailsLabel").text;
        verify(details.endsWith("header not completed: it declares 1:00:00, the file holds 42:10"), details);
        openForm(page, 1);
        compare(field(page, "keepUnfinishedLabel").text,
                "The header of this recording was not completed: the header declares 1:00:00 of audio, the file "
                + "holds 42:10. This happens when a player is switched off or a stick is pulled while recording. "
                + "The recording on the stick is not changed.");
        saveScreenshot(page, "recordings-keep-form-cut-short");
        findChild(page, "keepDialog").close();
        tryVerify(() => !findChild(page, "keepDialog").visible, 2000);
        openForm(page, 0);
        verify(field(page, "keepUnfinishedLabel").text.indexOf("does not say how long") >= 0,
               "one that declares no length: its own words, not the last one's lengths");
    }

    // Cut short by less than a second: both lengths print the same, so the
    // bytes missing are said, in the row and in the form. Lengths are cut
    // to whole seconds (42:10.9 is 42:10), as the C++ texts count.
    function test_aRecordingCutShortByLessThanASecond() {
        const page = makePage(controller => {
            addRecordings(controller);
            controller.recordings.setProperty(1, "unfinished", true);
            controller.recordings.setProperty(1, "heldSeconds", 2530.9);
            controller.recordings.setProperty(1, "declaredSeconds", 2530.95);
            controller.recordings.setProperty(1, "missingBytes", 9600);
            controller.recordings.setProperty(0, "unfinished", true);
            controller.recordings.setProperty(0, "heldSeconds", 2530.2);
            controller.recordings.setProperty(0, "declaredSeconds", 3600);
            controller.recordings.setProperty(0, "missingBytes", 10706400);
        });
        const details = findChild(row(page, 1), "detailsLabel").text;
        verify(details.endsWith("header not completed: it declares 42:10, the file holds 42:10 (9600 byte(s) less "
                                + "than the header declares)"), details);
        verify(findChild(row(page, 0), "detailsLabel").text.indexOf("less than the header") < 0,
               "lengths that print apart: no bytes said");
        openForm(page, 1);
        compare(field(page, "keepUnfinishedLabel").text,
                "The header of this recording was not completed: the header declares 42:10 of audio, the file "
                + "holds 42:10 (9600 byte(s) less than the header declares). This happens when a player is switched "
                + "off or a stick is pulled while recording. The recording on the stick is not changed.");
    }

    // A declared length no recording has is not printed as one.
    function test_aDeclaredLengthThatIsNotUsable() {
        const page = makePage(controller => {
            addRecordings(controller);
            controller.recordings.setProperty(1, "unfinished", true);
            controller.recordings.setProperty(1, "heldSeconds", 61.5);
            controller.recordings.setProperty(1, "declaredUnusable", true);
            controller.recordings.setProperty(1, "missingBytes", 1e15);
        });
        const details = findChild(row(page, 1), "detailsLabel").text;
        verify(details.endsWith("header not completed: its length is not usable, the file holds 1:01"), details);
        openForm(page, 1);
        compare(field(page, "keepUnfinishedLabel").text,
                "The header of this recording was not completed: the header's length is not usable, and the file "
                + "holds 1:01 of audio. This happens when a player is switched off or a stick is pulled while "
                + "recording. The recording on the stick is not changed.");
    }

    // A WAV without one whole frame of audio: the row says so and offers
    // no keeping.
    function test_aRecordingThatHoldsNoAudio() {
        const page = makePage(controller => {
            addRecordings(controller);
            controller.recordings.setProperty(1, "holdsNoAudio", true);
        });
        const details = findChild(row(page, 1), "detailsLabel").text;
        verify(details.endsWith("holds no audio"), details);
        verify(!findChild(row(page, 1), "keepButton").visible, "not offered to keep");
        verify(findChild(row(page, 0), "keepButton").visible, "another WAV still is");
    }

    // A kept job's notes show when it is done: the header was not
    // completed, and how much was kept.
    function test_theNotesOfAKeptJob() {
        const page = makePage(addRecordings, keep => {
            keptKeep(keep);
            const done = Object.assign({}, keep.finishedJobs[0]);
            done.notes = ["The header of this recording was not completed and did not say how long the audio is, "
                          + "so the file was read to its end. The FLAC holds 1:02:03 of audio, every whole frame "
                          + "the file held. The recording on the stick was not changed."];
            keep.finishedJobs = [done];
            keep.revision++;
        });
        const strip = findChild(page, "keepProgress");
        mouseClick(findChild(strip, "passiveProgressArea"), 10, 4);
        compare(findChild(strip, "passiveProgressNote").text,
                "The header of this recording was not completed and did not say how long the audio is, so the file "
                + "was read to its end. The FLAC holds 1:02:03 of audio, every whole frame the file held. The "
                + "recording on the stick was not changed.");
    }

    // Experimental off: the rows are the released page's, whatever the
    // controller hands over of a WAV's header (it reads none then): folder,
    // date and the length rounded to the nearest second, no header texts.
    // On: the header texts, and lengths cut to whole seconds.
    function test_experimentalOffShowsTheReleasedRows() {
        const states = controller => {
            addRecordings(controller);
            controller.recordings.setProperty(1, "unfinished", true);
            controller.recordings.setProperty(1, "heldSeconds", 2530.2);
            controller.recordings.setProperty(1, "declaredSeconds", 3600);
            controller.recordings.setProperty(2, "holdsNoAudio", true);
        };
        const stamp = (date) => date.toLocaleString(Qt.locale(), Locale.ShortFormat);
        let page = makePage(states, null, false);
        compare(findChild(row(page, 0), "detailsLabel").text,
                "Sessions (Engine OS) · " + stamp(new Date(2026, 4, 7, 20, 6)) + " · 3:31:34",
                "12693.5 seconds, rounded as the released page rounds it");
        compare(findChild(row(page, 1), "detailsLabel").text,
                "Sessions (Engine OS) · " + stamp(new Date(2026, 6, 12, 2, 30)) + " · 10:31",
                "an unfinished header says nothing: the length the listing gave");
        compare(findChild(row(page, 2), "detailsLabel").text,
                "PIONEER REC (Pioneer) · " + stamp(new Date(2017, 0, 1, 1, 0)),
                "one that holds no audio says nothing of it either");

        page = makePage(states);
        verify(findChild(row(page, 0), "detailsLabel").text.endsWith(" · 3:31:33"), "on: cut to whole seconds");
        verify(findChild(row(page, 1), "detailsLabel").text.endsWith(
                   "header not completed: it declares 1:00:00, the file holds 42:10"),
               "on: the header's state");
        verify(findChild(row(page, 2), "detailsLabel").text.endsWith("holds no audio"), "on: no audio said");
    }

    // A count in a sentence takes its plural from the translation (%n).
    function test_countsTakeTheirPluralForm() {
        const page = makePage(controller => {
            addRecordings(controller);
            controller.recordings.setProperty(1, "unfinished", true);
            controller.recordings.setProperty(1, "heldSeconds", 2530.2);
            controller.recordings.setProperty(1, "declaredSeconds", 2530.5);
            controller.recordings.setProperty(1, "missingBytes", 1);
        });
        verify(findChild(row(page, 1), "detailsLabel").text.endsWith(
                   "(1 byte(s) less than the header declares)"),
               findChild(row(page, 1), "detailsLabel").text);
        page.keepController.finishedJobs = [
            {state: "kept", recording: "A.wav", fileName: "A.flac", folder: "/f", flacPath: "/f/A.flac",
             leftBehind: ""},
            {state: "failed", recording: "B.wav", fileName: "B.flac", folder: "/f", flacPath: "", leftBehind: "",
             message: "no room"},
        ];
        compare(findChild(findChild(page, "keepProgress"), "passiveProgressSummary").text,
                "1 of 2 recording(s) not saved");
    }

    // The keep controller outlives the page (it lives with the window): a
    // page made again for it shows the work under way and what ended.
    function test_theStripComesBackWhenThePageIsOpenedAgain() {
        const keep = createTemporaryObject(fakeKeepComponent, testCase);
        busyKeep(keep);
        const open = () => {
            const stack = createTemporaryObject(stackComponent, testCase);
            const controller = createTemporaryObject(fakeControllerComponent, testCase);
            addRecordings(controller);
            const player = createTemporaryObject(fakePlayerComponent, testCase);
            stack.push(fillerComponent, {}, StackView.Immediate);
            const page = stack.push(pageComponent, {controller: controller, playbackController: player,
                                                    keepController: keep, keepEnabled: true}, StackView.Immediate);
            waitForRendering(page);
            return {stack: stack, page: page};
        };
        let opened = open();
        verify(findChild(opened.page, "keepProgress").visible, "the strip shows the job");
        let destroyed = false;
        opened.page.Component.destruction.connect(() => { destroyed = true; });
        opened.stack.pop(StackView.Immediate);
        tryVerify(() => destroyed, 2000, "the page is left and destroyed");
        compare(keep.calls.indexOf("cancel"), -1, "leaving the page does not stop the work");
        opened = open();
        const strip = findChild(opened.page, "keepProgress");
        verify(strip.visible, "opened again: the strip is there");
        verify(findChild(strip, "passiveProgressSummary").text.indexOf("Saving Milkshaken.wav as FLAC") === 0,
               findChild(strip, "passiveProgressSummary").text);
        compare(findChild(row(opened.page, 0), "keptLabel").text, "Being saved as DJ Kelp - Late Set.flac: checking");
    }

    // A second name of a kept FLAC that could not be removed: the strip
    // names the leftover.
    function test_aKeptFlacsSecondNameIsSaid() {
        const page = makePage(addRecordings, keep => {
            keptKeep(keep);
            const done = Object.assign({}, keep.finishedJobs[0]);
            done.leftBehind = "/home/dj/Music/Own Recordings/"
                + "DJ Kelp - Late Set.flac.00000000000000aa.seabass-encode-tmp";
            keep.finishedJobs = [done];
            keep.revision++;
        });
        compare(findChild(findChild(page, "keepProgress"), "passiveProgressSummary").text,
                "Saved Milkshaken.wav as DJ Kelp - Late Set.flac, and a second file was left in "
                + "/home/dj/Music/Own Recordings");
    }
}
