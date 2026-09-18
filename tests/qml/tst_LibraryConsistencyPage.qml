// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui

// Library Health's findings page, driven by a stand-in controller: what
// is staged is said beside the buttons that staged it.
//
// It used to show one number for the page -- so staging 29 stray cue
// removals put "29 staged, not saved yet" next to "Stage All Safe
// Repairs", a button about missing files, and nothing at all next to the
// cue buttons that had just done the staging.
TestCase {
    id: testCase
    name: "LibraryConsistencyPage"
    width: 1000
    height: 800
    visible: true
    when: windowShown

    PlaybackController { id: realPlayback }

    // Only what the two notes and the rows around them read.
    Component {
        id: controllerComponent
        QtObject {
            property bool busy: false
            property bool writing: false
            property bool canUndo: false
            property bool stickReadOnly: false
            property int repairableCount: 0
            property int stagedCount: stagedIssueCount + stagedJunkCueCount
            property int stagedIssueCount: 0
            property int stagedJunkCueCount: 0
            property int artworkRepairableCount: 0
            property bool artworkRepairStaged: false
            property string errorMessage: ""
            property string statusMessage: ""
            property string scanningFormat: ""
            property int scanCurrent: 0
            property int scanTotal: 0
            property bool scanCancellable: false
            signal scanCancelled()
            property var issues: ListModel {}
            property var junkCues: ListModel {}
            property var playlistNames: []
            property var playlistTrackCounts: ({})
            function scan(a, b, c) {}
            function cancelScan() {}
        }
    }

    Component {
        id: pageComponent
        LibraryConsistencyPage {
            width: 980
            height: 760
            stickLabel: "TESTSTICK"
            rekordboxPath: "/nonexistent/TESTSTICK/PIONEER"
            enginePath: "/nonexistent/TESTSTICK/Engine Library"
            playbackController: realPlayback
        }
    }

    function test_eachChecksStagedWorkIsCountedBesideItsOwnButtons() {
        var controller = createTemporaryObject(controllerComponent, testCase);
        var page = createTemporaryObject(pageComponent, testCase, {sharedController: controller});
        verify(page !== null, "the page must instantiate");
        var missing = findChild(page, "stagedIssuesNote");
        var cues = findChild(page, "stagedJunkCuesNote");
        verify(missing !== null && cues !== null, "both checks must have a staged note");
        compare(missing.visible, false, "nothing staged, nothing said");
        compare(cues.visible, false);

        // Stray cues staged: the cue section says so, the missing-files
        // section stays quiet.
        controller.stagedJunkCueCount = 29;
        compare(cues.visible, true);
        compare(cues.text, "29 staged, not saved yet");
        compare(missing.visible, false, "a staged cue removal is not staged repair work");

        // And the other way round.
        controller.stagedJunkCueCount = 0;
        controller.stagedIssueCount = 4;
        compare(missing.visible, true);
        compare(missing.text, "4 staged, not saved yet");
        compare(cues.visible, false);

        // The page outlives the stand-in otherwise, and spends teardown
        // reading properties off a destroyed object.
        page.destroy();
        wait(0);
    }
}
