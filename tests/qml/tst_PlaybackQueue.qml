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

    // file:///C:/x on Windows leaves "/C:/x" after the scheme is cut, a path
    // that does not exist, so every row here read as a missing file.
    readonly property string aFile: Qt.resolvedUrl("tst_PlaybackQueue.qml").toString()
        .replace("file://", "").replace(/^\/([A-Za-z]:)/, "$1")

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

    // While a library is being written nothing moves through the queue:
    // that would open the library, unasked, in the middle of a save.
    function test_nothingMovesThroughTheQueueWhileALibraryIsWritten() {
        load("a");
        player.libraryBusy = true;
        player.next();
        compare(player.currentSourceId, "a", "next waits");
        compare(advanced.count, 0);
        player.libraryBusy = false;
        player.next();
        compare(player.currentSourceId, "d", "and works again afterwards");
        player.libraryBusy = false;
    }

    // Letting go of a queue lets go of its signals too. They used to be
    // kept, so every return to a list connected it once more and every
    // change to the list was then announced that many times over.
    SignalSpy { id: queueChanges; target: player; signalName: "queueChanged" }
    function test_aQueueLetGoOfIsNotListenedToAnyMore() {
        load("a");
        for (var round = 0; round < 3; ++round) {
            load("a", "/another/library");            // drops the queue
            player.setQueue(queue, "rekordbox", "/lib");
            load("a");
        }
        queueChanges.clear();
        queue.append({sourceId: "e", filePath: aFile, streamingSource: "", title: "Track e", artist: "Artist", artworkPath: ""});
        compare(queueChanges.count, 1, "one change to the list is one announcement");
    }

    // A page that goes takes its list with it.
    Component { id: shortLivedQueue; ListModel {} }
    function test_aQueueThatIsDestroyedEndsTheQueue() {
        var model = shortLivedQueue.createObject(testCase);
        model.append({sourceId: "a", filePath: aFile, streamingSource: "", title: "A", artist: "", artworkPath: ""});
        model.append({sourceId: "z", filePath: aFile, streamingSource: "", title: "Z", artist: "", artworkPath: ""});
        player.setQueue(model, "rekordbox", "/lib");
        load("a");
        compare(player.hasNext, true);
        queueChanges.clear();
        model.destroy();
        tryVerify(function() { return queueChanges.count > 0; }, 2000, "its going is announced");
        compare(player.hasNext, false);
        player.next();
        compare(player.currentSourceId, "a");
    }

    // Whether live levels are to be had is not known until audio has
    // actually arrived: Qt 6.8 has the means on every backend and uses
    // them on one. A display goes by this to choose its fallback.
    function test_liveLevelsAreNotClaimedBeforeAnyAudioArrives() {
        compare(player.liveLevels, false);
    }
}
