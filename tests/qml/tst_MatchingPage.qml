// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui
import "PixelScale.js" as PixelScale

// Matching's candidates play from their cover, as Browse's rows do: the
// page asks for the track with what the player needs, and a streaming
// track, which has no file here, does not ask.
TestCase {
    id: testCase
    name: "MatchingPage"
    width: 700
    height: 500
    visible: true
    when: windowShown

    ScanController { id: realScan }
    Component {
        id: pageComponent
        MatchingPage {
            width: 700
            height: 500
            scanController: realScan
            keyNotation: "standard"
            anchorSourceId: "1"
            anchorTitle: "Anchor"
            anchorArtist: "A"
            anchorKey: "Am"
            anchorBpm: 128.0
            anchorArtworkPath: ""
            anchorPlaylistNames: []
            browseSelectedPlaylistIndex: -1
        }
    }
    Component { id: spyComponent; SignalSpy {} }

    function candidate(id, title, streaming) {
        return {sourceId: id, title: title, artist: "Artist", key: "Am", keyRelation: "Same key", camelotLabel: "8A",
                bpm: 128.0, rating: -1, artworkPath: "", fallbackArtworkPath: "", durationSeconds: 300,
                filePath: "/stick/Contents/" + title + ".mp3", streamingSource: streaming, playlistNames: [],
                cues: [{kind: "hot", hotCueNumber: 1, positionMs: 1000}]};
    }

    function playAreas(page) {
        const found = [];
        function walk(item) {
            if (item.objectName === "candidatePlayArea") found.push(item);
            for (let i = 0; i < item.children.length; ++i) walk(item.children[i]);
        }
        walk(page);
        return found;
    }

    // The edit mode's Save button floats over the list's bottom corner:
    // the list scrolls two rows past its last one, clear of it.
    function test_theListScrollsClearOfTheSaveButton() {
        const page = createTemporaryObject(pageComponent, testCase);
        verify(page !== null);
        const list = findChild(page, "candidatesList");
        verify(list !== null);
        verify(list.bottomMargin >= 2 * 40, "two rows of room: " + list.bottomMargin);
    }

    function test_aCandidatesCoverPlaysIt() {
        const page = createTemporaryObject(pageComponent, testCase);
        page.candidates = [candidate("2", "Bloom", ""), candidate("3", "Stream", "Beatport")];
        waitForRendering(page);
        const spy = createTemporaryObject(spyComponent, testCase, {target: page, signalName: "playRequested"});
        tryVerify(() => playAreas(page).length === 2, 2000, "a play area on each candidate's cover");
        const areas = playAreas(page);
        mouseClick(areas[0]);
        compare(spy.count, 1, "the cover plays");
        const asked = spy.signalArguments[0][0];
        compare(asked.sourceId, "2");
        compare(asked.filePath, "/stick/Contents/Bloom.mp3", "with the file the player needs");
        compare(asked.cues.length, 1);
        mouseClick(areas[1]);
        compare(spy.count, 1, "a streaming track's cover does not");
    }

    // ---- This Playlist's combo: blank rows (Sebastian's testing), a row
    // now and then an empty gap, the right height and no text. ----

    Component {
        id: fakeScanComponent
        QtObject {
            property var playlistNames: []
            property var playlistTrackCounts: ({})
            property int totalTrackCount: 0
            function findCompatibleTracks() { return []; }
        }
    }
    Component {
        id: fakeScanPageComponent
        MatchingPage {
            width: 700
            height: 500
            keyNotation: "standard"
            anchorSourceId: "1"
            anchorTitle: "Anchor"
            anchorArtist: "A"
            anchorKey: "Am"
            anchorBpm: 128.0
            anchorArtworkPath: ""
            anchorPlaylistNames: []
            browseSelectedPlaylistIndex: -1
        }
    }

    function namesFor(scan, n, prefix) {
        const names = [];
        const counts = {};
        for (let i = 1; i <= n; ++i) {
            const name = prefix + String(i).padStart(3, "0");
            names.push(name);
            counts[name] = i;
        }
        scan.playlistTrackCounts = counts;
        scan.totalTrackCount = n;
        scan.playlistNames = names;
    }

    function checkComboRows(combo, what) {
        const list = combo.popup.contentItem;
        tryVerify(() => list.count === combo.model.length
                 && (list.itemAtIndex(0) !== null || list.itemAtIndex(list.count - 1) !== null), 3000, what + ": rows built");
        waitForRendering(list);
        let built = 0;
        for (let i = 0; i < list.count; ++i) {
            const row = list.itemAtIndex(i);
            if (row === null) {
                continue;  // not built: scrolled out of the popup
            }
            ++built;
            const name = findChild(row, "playlistRowName");
            verify(name !== null, what + ": row " + i + " has its name label");
            compare(name.text, combo.model[i].name, what + ": row " + i + "'s text");
            verify(name.visible && name.width > 0 && name.height > 0,
                   what + ": row " + i + "'s name is drawn (" + name.width + " x " + name.height + ")");
            compare(row.height, 32, what + ": row " + i + "'s height");
            verify(row.visible && row.width >= list.width - 1,
                   what + ": row " + i + " as wide as the list (" + row.width + " of " + list.width + ")");
        }
        verify(built > 5, what + ": rows built " + built);
        checkRowsDrawn(list, "playlistRowName", what);
    }

    function test_thePlaylistComboHasNoBlankRows() {
        const scan = createTemporaryObject(fakeScanComponent, testCase);
        namesFor(scan, 30, "Playlist ");
        const page = createTemporaryObject(fakeScanPageComponent, testCase, {scanController: scan});
        const combo = findChild(page, "playlistCombo");
        verify(combo !== null);
        waitForRendering(page);
        for (let round = 0; round < 20; ++round) {
            mouseClick(combo);
            tryVerify(() => combo.popup.opened, 3000, "opens, round " + round);
            checkComboRows(combo, "round " + round);
            // And scrolled to its end, the rows below built as they come.
            combo.popup.contentItem.positionViewAtEnd();
            checkComboRows(combo, "scrolled, round " + round);
            combo.popup.close();
            tryVerify(() => !combo.popup.visible, 3000, "closes, round " + round);
        }
    }

    function test_thePlaylistComboHasNoBlankRowsRightAfterTheNamesArrive() {
        const scan = createTemporaryObject(fakeScanComponent, testCase);
        const page = createTemporaryObject(fakeScanPageComponent, testCase, {scanController: scan});
        const combo = findChild(page, "playlistCombo");
        waitForRendering(page);
        for (let round = 0; round < 20; ++round) {
            namesFor(scan, 30 + round % 3, round % 2 ? "Crate " : "Playlist ");
            mouseClick(combo);
            tryVerify(() => combo.popup.opened, 3000, "opens, round " + round);
            checkComboRows(combo, "after the names changed, round " + round);
            combo.popup.close();
            tryVerify(() => !combo.popup.visible, 3000, "closes, round " + round);
        }
    }

    function test_thePlaylistComboHasNoBlankRowsWhenTheNamesChangeWhileOpen() {
        const scan = createTemporaryObject(fakeScanComponent, testCase);
        namesFor(scan, 4, "Early ");
        const page = createTemporaryObject(fakeScanPageComponent, testCase, {scanController: scan});
        const combo = findChild(page, "playlistCombo");
        waitForRendering(page);
        for (let round = 0; round < 20; ++round) {
            mouseClick(combo);
            tryVerify(() => combo.popup.opened, 3000, "opens, round " + round);
            namesFor(scan, 30 + round % 3, round % 2 ? "Crate " : "Playlist ");
            checkComboRows(combo, "changed while open, round " + round);
            combo.popup.close();
            tryVerify(() => !combo.popup.visible, 3000, "closes, round " + round);
            namesFor(scan, 4 + round % 3, "Early ");
        }
    }

    // The text of every row in sight, as drawn: the window's own frame
    // (the popup is in its overlay), read under each row's name label,
    // must hold pixels unlike the row's ground beside it.
    function checkRowsDrawn(list, labelName, what) {
        const ground = list.Window.contentItem;
        const image = grabImage(ground);
        const s = PixelScale.scale(image, ground);
        const lum = c => 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b;
        let looked = 0;
        for (let i = 0; i < list.count; ++i) {
            const row = list.itemAtIndex(i);
            if (row === null || row.y - list.contentY < 0 || row.y - list.contentY + row.height > list.height) {
                continue;
            }
            if (row.mapToItem(ground, 0, row.height).y > ground.height) {
                continue;  // below the window's edge
            }
            const name = findChild(row, labelName);
            const back = lum(PixelScale.pixel(image, ground, row.mapToItem(ground, 4, row.height / 2).x,
                                              row.mapToItem(ground, 4, row.height / 2).y));
            const origin = name.mapToItem(ground, 0, 0);
            let ink = 0;
            for (let y = 0; y < name.height; y += 1 / s) {
                for (let x = 0; x < Math.min(name.width, 60); x += 1 / s) {
                    ink += Math.abs(lum(PixelScale.pixel(image, ground, origin.x + x, origin.y + y)) - back) > 0.15 ? 1 : 0;
                }
            }
            verify(ink >= 3, what + ": row " + i + " (" + name.text + ") is drawn with its text, " + ink + " pixels of ink");
            ++looked;
        }
        verify(looked > 4, what + ": rows looked at " + looked);
    }
}
