// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui

// The real PlaybackController walking a queue: which row is next, which
// rows it steps over, and when there is no next at all. The "audio" is
// this file itself -- it exists, which is all the queue asks of a row;
// that the player then cannot decode it does not matter here.
TestCase {
    id: testCase
    name: "PlaybackQueue"
    when: windowShown
    visible: true
    width: 100
    height: 100

    readonly property string aFile: Qt.resolvedUrl("tst_PlaybackQueue.qml").toString().replace("file://", "")

    PlaybackController { id: player }
    SignalSpy { id: advanced; target: player; signalName: "advanced" }

    ListModel { id: queue }

    function init() {
        player.stop();
        queue.clear();
        var rows = [
            {sourceId: "a", filePath: aFile, streamingSource: ""},
            {sourceId: "b", filePath: aFile, streamingSource: "tidal"},   // streams: nothing here to play
            {sourceId: "c", filePath: "/nonexistent/c.mp3", streamingSource: ""},
            {sourceId: "d", filePath: aFile, streamingSource: ""},
        ];
        for (var i = 0; i < rows.length; ++i) {
            rows[i].title = "Track " + rows[i].sourceId;
            rows[i].artist = "Artist";
            rows[i].artworkPath = "";
            queue.append(rows[i]);
        }
        player.setQueue(queue, "rekordbox", "/lib");
        advanced.clear();
    }

    function load(sourceId, libraryPath) {
        player.load("rekordbox", libraryPath || "/lib", sourceId, aFile, "Track " + sourceId, "Artist", "", []);
    }

    function test_nextStepsOverWhatCannotBePlayed() {
        load("a");
        compare(player.currentQueueRow(), 0);
        compare(player.hasNext, true);
        compare(player.hasPrevious, false);

        player.next();
        compare(player.currentSourceId, "d", "b streams and c's file is gone: d is next");
        compare(player.title, "Track d");
        compare(advanced.count, 1);
        compare(advanced.signalArguments[0][0], "a", "and says which track it left");
        compare(player.hasNext, false);

        player.next();
        compare(player.currentSourceId, "d", "at the end of the list next does nothing");
        compare(advanced.count, 1);

        player.previous();
        compare(player.currentSourceId, "a");
    }

    // The queue is the list as it is NOW. Sort it under a playing track
    // and "next" is whatever now follows that track.
    function test_theQueueIsTheListAsItIsNow() {
        load("a");
        queue.move(3, 0, 1);              // d, a, b, c
        compare(player.currentQueueRow(), 1);
        compare(player.hasNext, false, "nothing playable follows a any more");
        compare(player.hasPrevious, true);
        player.previous();
        compare(player.currentSourceId, "d");

        queue.remove(0);                  // d filtered out of the list
        compare(player.currentQueueRow(), -1);
        compare(player.hasNext, false, "a track that is not in the list has no next");
        compare(player.hasPrevious, false);
    }

    // Ids are per library. The same id loaded from another library is not
    // this queue's row, and ends the queue.
    function test_aTrackFromAnotherLibraryEndsTheQueue() {
        load("a", "/another/library");
        compare(player.currentQueueRow(), -1);
        compare(player.hasNext, false);
        load("a");
        compare(player.hasNext, false, "and it stays ended until a list offers itself again");
        player.setQueue(queue, "rekordbox", "/lib");
        compare(player.hasNext, true);
    }

    function test_withNothingLoadedThereIsNoNext() {
        compare(player.hasNext, false);
        player.next();
        player.previous();
        player.skipBeats(4);
        compare(player.hasTrack, false, "and asking changes nothing");
    }
}
