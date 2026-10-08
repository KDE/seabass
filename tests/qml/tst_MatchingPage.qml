// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui

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
}
