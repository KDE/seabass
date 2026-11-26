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
            property string currentWavPath: ""
            property string currentRecording: ""
            property string currentFileName: ""
            property string currentFolder: ""
            property string progressStep: ""
            property double progress: 0
            property var queued: []
            property var lastFinished: ({})
            property int revision: 0
            property string errorMessage: ""
            property string coverPath: ""
            property string coverPreview: ""
            property int coverWidth: 0
            property int coverHeight: 0
            property string coverError: ""
            property bool coverPreparing: false
            property var outcomes: ({})
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
            function fileNameFor(artist, title) {
                const a = artist.trim();
                const t = title.trim();
                return (a.length > 0 && t.length > 0 ? a + " - " + t : (t.length > 0 ? t : (a.length > 0 ? a : "Recording")))
                    + ".flac";
            }
            function folderProblem(folder) {
                if (folder.trim().length === 0) return "Choose a folder for the file.";
                if (folder.indexOf("/blocked") >= 0) return "A file is in the way of this folder.";
                return "";
            }
            function targetProblem(folder, artist, title) {
                const name = fakeKeep.fileNameFor(artist, title);
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
            function cancel() { fakeKeep.calls.push("cancel"); }
            function outcomeFor(path) { return fakeKeep.outcomes[path] ?? ({}); }
            function dismissFinished() { fakeKeep.calls.push("dismiss"); fakeKeep.lastFinished = ({}); }
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

    // A recording plays as a plain file, in the form of the deck that made it.
    function test_playPlaysTheRecording() {
        const page = makePage(addRecordings);
        const player = page.playbackController;
        mouseClick(findChild(row(page, 0), "playButton"));
        compare(player.loads.length, 1);
        compare(player.loads[0].format, "engine", "Sessions/ is Engine OS's");
        compare(player.loads[0].libraryPath, "/media/u/WHALESHARK/Engine Library");
        compare(player.loads[0].filePath, "/media/u/WHALESHARK/Sessions/Milkshaken.wav");
        compare(player.loads[0].title, "Milkshaken.wav");
        compare(player.loads[0].artist, "Sessions (Engine OS)");

        mouseClick(findChild(row(page, 2), "playButton"));
        compare(player.loads[1].format, "rekordbox", "a REC folder is a Pioneer or AlphaTheta deck's");
        compare(player.loads[1].libraryPath, "/media/u/WHALESHARK/PIONEER");
        compare(player.loads[1].filePath, "/media/u/WHALESHARK/PIONEER REC/REC001.WAV");
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
            controller.recordings.append({path: "/media/u/WHALESHARK/Sessions/Set.mp3", fileName: "Set.mp3",
                                          folderName: "Sessions", source: "engine", sizeBytes: 1000,
                                          modified: new Date(2026, 1, 1), durationSeconds: 10, included: false});
        });
        for (let i = 0; i < 3; ++i) {
            verify(findChild(row(page, i), "keepButton").visible, "row " + i + " is a WAV and can be kept");
        }
        compare(findChild(row(page, 3), "keepButton").visible, false, "an MP3 is not offered: the encoder reads WAVs");
        compare(findChild(row(page, 0), "keepButton").text, "Keep…");
        compare(findChild(row(page, 0), "keepButton").ToolTip.text,
                "Keep this recording on this computer, as a FLAC file with tags and a cover");

        page = makePage(addRecordings, null, false);
        compare(findChild(row(page, 0), "keepButton").visible, false, "Experimental off: nothing to keep");
        compare(findChild(page, "keepProgress").visible, false, "and no strip");

        page = makePage(addRecordings, keep => { keep.available = false; });
        compare(findChild(row(page, 0), "keepButton").visible, false, "a build that cannot encode offers nothing");
    }

    function test_theFormSuggests() {
        const page = makePage(addRecordings);
        const dialog = openForm(page, 1);
        compare(dialog.title, "Keep This Recording");
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
        compare(form.wavPath, "/media/u/WHALESHARK/Sessions/Milkshaken.wav");
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
        keep.currentWavPath = "/media/u/WHALESHARK/Sessions/Milkshaken.wav";
        keep.currentRecording = "Milkshaken.wav";
        keep.currentFileName = "DJ Kelp - Late Set.flac";
        keep.currentFolder = "/home/dj/Music/Own Recordings";
        keep.progressStep = "checking";
        keep.progress = 0.7;
        keep.queued = [{wavPath: "/media/u/WHALESHARK/Sessions/Session-0002.wav", recording: "Session-0002.wav",
                        fileName: "DJ Kelp - Early Set.flac", folder: "/home/dj/Music/Own Recordings"}];
        keep.outcomes = {
            "/media/u/WHALESHARK/Sessions/Milkshaken.wav": {state: "running", fileName: "DJ Kelp - Late Set.flac",
                                                            folder: "/home/dj/Music/Own Recordings"},
            "/media/u/WHALESHARK/Sessions/Session-0002.wav": {state: "queued", fileName: "DJ Kelp - Early Set.flac",
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
                "Keeping Milkshaken.wav: checking, 70% (1 more waiting)");
        compare(findChild(strip, "passiveProgressBar").value, 0.7, "one bar for the whole job");
        compare(findChild(row(page, 0), "keptLabel").text, "Being kept as DJ Kelp - Late Set.flac");
        compare(findChild(row(page, 1), "keptLabel").text, "Waits to be kept as DJ Kelp - Early Set.flac");
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
        compare(steps.map(s => s.text), ["Encoding", "Checking", "Tags and cover"]);
        const counts = findAll(strip, "passiveProgressStepCount").filter(c => c.visible);
        compare(counts.map(c => c.text), ["70%"], "the step in hand with its count");
        const lines = findAll(strip, "passiveProgressActivityLine").map(l => l.text);
        compare(lines, ["Writing DJ Kelp - Late Set.flac in /home/dj/Music/Own Recordings",
                        "Waiting: Session-0002.wav, as DJ Kelp - Early Set.flac"], "what is in hand, and what waits");
        compare(findChild(strip, "passiveProgressNote").text,
                "The recording on the stick is only read. Leaving this page stops the work.");
        saveScreenshot(page, "recordings-keep-progress-open");
        mouseClick(findChild(strip, "passiveProgressCancel"));
        compare(page.keepController.calls[page.keepController.calls.length - 1], "cancel", "Cancel by mouse");
    }

    // Two pieces of a step's count: the steps before are done, the one in
    // hand is now, those after come later.
    function test_theStepsFollowTheJob_data() {
        return [
            {tag: "space", step: "space", states: ["now", "later", "later"], word: "encoding"},
            {tag: "encoding", step: "encoding", states: ["now", "later", "later"], word: "encoding"},
            {tag: "checking", step: "checking", states: ["done", "now", "later"], word: "checking"},
            {tag: "tags", step: "tags", states: ["done", "done", "now"], word: "tags and cover"},
        ];
    }
    function test_theStepsFollowTheJob(data) {
        const page = makePage(addRecordings, keep => { busyKeep(keep); keep.queued = []; keep.progressStep = data.step; });
        const strip = findChild(page, "keepProgress");
        compare(strip.steps.map(s => s.state), data.states);
        compare(findChild(strip, "passiveProgressSummary").text, "Keeping Milkshaken.wav: " + data.word + ", 70%");
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
        keep.lastFinished = {state: "kept", wavPath: "/media/u/WHALESHARK/Sessions/Milkshaken.wav",
                             recording: "Milkshaken.wav", fileName: "DJ Kelp - Late Set.flac",
                             folder: "/home/dj/Music/Own Recordings",
                             flacPath: "/home/dj/Music/Own Recordings/DJ Kelp - Late Set.flac", message: ""};
        keep.outcomes = {"/media/u/WHALESHARK/Sessions/Milkshaken.wav": keep.lastFinished};
        keep.progress = 1;
        keep.revision++;
    }

    function test_whenDone() {
        const page = makePage(addRecordings, keptKeep);
        const strip = findChild(page, "keepProgress");
        verify(strip.visible, "the strip says it is done");
        compare(findChild(strip, "passiveProgressSummary").text, "Kept Milkshaken.wav as DJ Kelp - Late Set.flac");
        compare(findChild(strip, "passiveProgressBar").value, 1, "the bar is full");
        compare(strip.cancellable, false, "nothing to cancel");
        const kept = findChild(row(page, 0), "keptLabel");
        verify(kept.visible);
        compare(kept.text, "Kept as DJ Kelp - Late Set.flac in /home/dj/Music/Own Recordings");
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

    function test_whenItFails() {
        const page = makePage(addRecordings, keep => {
            keep.lastFinished = {state: "failed", wavPath: "/media/u/WHALESHARK/Sessions/Milkshaken.wav",
                                 recording: "Milkshaken.wav", fileName: "DJ Kelp - Late Set.flac",
                                 folder: "/home/dj/Music/Own Recordings", flacPath: "",
                                 message: "There is not enough room in /home/dj/Music/Own Recordings."};
            keep.outcomes = {"/media/u/WHALESHARK/Sessions/Milkshaken.wav": keep.lastFinished};
            keep.revision++;
        });
        const strip = findChild(page, "keepProgress");
        compare(findChild(strip, "passiveProgressSummary").text, "Milkshaken.wav was not kept");
        compare(findChild(strip, "passiveProgressAction_show"), null, "nothing to show");
        mouseClick(findChild(strip, "passiveProgressArea"), 10, 4);
        compare(findChild(strip, "passiveProgressNote").text, "There is not enough room in /home/dj/Music/Own Recordings.");
        compare(findChild(row(page, 0), "keptLabel").text,
                "Not kept: There is not enough room in /home/dj/Music/Own Recordings.");
        compare(findChild(row(page, 0), "showKeptButton").visible, false);
    }
}
