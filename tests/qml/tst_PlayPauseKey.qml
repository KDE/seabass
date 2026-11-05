// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
import QtQuick
import QtQuick.Controls
import QtTest
import SeabassGui

// Space plays or pauses, and keeps doing so over a modal dialog.
TestCase {
    id: testCase
    name: "PlayPauseKey"
    when: windowShown
    visible: true
    width: 640
    height: 480

    QtObject {
        id: fakePlayer
        property bool hasTrack: true
        property int toggles: 0
        function togglePlay() { toggles++; }
    }
    PlayPauseShortcut {
        id: shortcut
        playbackController: fakePlayer
    }
    MessageDialog {
        id: question
        severity: SeabassDialog.Question
        title: "A question"
        headline: "Over the music."
        acceptText: "Yes"
    }

    function init() {
        fakePlayer.hasTrack = true;
        fakePlayer.toggles = 0;
        shortcut.active = true;
        if (question.visible) {
            question.close();
            tryCompare(question, "visible", false, 5000);
        }
        testCase.forceActiveFocus();
    }

    function test_spaceTogglesPlayback() {
        keyClick(Qt.Key_Space);
        compare(fakePlayer.toggles, 1);
        fakePlayer.hasTrack = false;
        keyClick(Qt.Key_Space);
        compare(fakePlayer.toggles, 1, "nothing loaded, nothing to toggle");
    }

    function test_spaceStillTogglesOverAModalDialog() {
        question.open();
        tryCompare(question, "visible", true, 5000);
        keyClick(Qt.Key_Space);
        compare(fakePlayer.toggles, 1, "the dialog does not take the key from the player");
        compare(question.visible, true, "and the key does not answer the question");
    }

    function test_notWhileTyping() {
        shortcut.active = false;
        keyClick(Qt.Key_Space);
        compare(fakePlayer.toggles, 0);
    }
}
