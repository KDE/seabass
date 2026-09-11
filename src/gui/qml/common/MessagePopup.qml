// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import SeabassGui

// The outcome of an action taken somewhere else on a long page -- e.g.
// LocalCuePage's "Restore From Here" buttons up in Backup History, whose
// result used to show only as a quiet line of text down in a different
// section, easy to miss entirely.
//
// Deliberately does NOT auto-dismiss. An earlier version did, on a timer,
// and that was explicitly rejected: the message needs to be seen and
// acknowledged, not flash past.
//
//   messagePopup.show("Backup restored.", false);
//   messagePopup.show("Could not read the archive.", true);
MessageDialog {
    id: popup

    severity: SeabassDialog.Info
    showReject: false
    acceptText: "OK"

    function show(message, isError) {
        popup.headline = message;
        popup.severity = isError ? SeabassDialog.Error : SeabassDialog.Info;
        popup.title = isError ? "That did not work" : "Done";
        popup.open();
    }
}
