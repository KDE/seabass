// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest

// A save that never finished can be undone from a session opened later
// (#48). The macOS round 8 check P3 pulled a stick mid-sync: the save's
// backup was complete on the stick, but Undo Last Save only knew the
// backups of a save made by the same session, in memory, and after
// Discard and a fresh look at the page it was offered nowhere.
// The sticks are built in C++, see ControllerFixture.sessionAfterInterruptedSave.
TestCase {
    name: "InterruptedSave"

    function test_aLaterSessionOffersTheUndo() {
        const s = controllerFixture.sessionAfterInterruptedSave("complete");
        compare(s.canUndo, true, "the interrupted save's backup is offered for undo");
        compare(s.interruptedSave, true, "and the page can say the save did not finish");
    }

    function test_aRecordTheSaveNeverFinishedIsNotOffered() {
        const s = controllerFixture.sessionAfterInterruptedSave("missing");
        compare(s.canUndo, false, "nothing to restore from, so nothing offered");
        compare(s.interruptedSave, false);
    }

    function test_noNoteNoUndo() {
        const s = controllerFixture.sessionAfterInterruptedSave("none");
        compare(s.canUndo, false);
        compare(s.interruptedSave, false);
    }
}
