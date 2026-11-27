// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import SeabassGui
import "RecordingSave.js" as RecordingSave

// What a recording's Save as FLAC is or came to, on its row: waits, is
// being saved (and at which step), saved where, or why not. Read from the
// window's KeepRecordingController, the one bookkeeping both the
// Recordings page and Browse's Own Recordings ask; "" when no save of it
// was asked for in this session.
Label {
    id: root
    objectName: "keptLabel"

    // A KeepRecordingController; null: nothing to say.
    property var keepController: null
    // The stick the recording is on, and its path, as the controller knows
    // them.
    property string stickRoot: ""
    property string path: ""

    readonly property int revision: root.keepController !== null ? root.keepController.revision : 0
    readonly property var kept: root.keepController !== null && root.path.length > 0
        ? ((root.revision, root.keepController.outcomeFor(root.stickRoot, root.path)) ?? ({})) : ({})
    readonly property string keptState: root.kept.state ?? ""
    // Asked for and not ended: not asked for again.
    readonly property bool inProgress: root.keptState === "queued" || root.keptState === "running"

    visible: text.length > 0
    text: RecordingSave.stateLine(root.kept, root.keptState === "running" ? root.keepController.progressStep : "")
    elide: Text.ElideMiddle
    color: RecordingSave.stateFailed(root.keptState) ? Theme.danger
        : root.keptState === "kept" ? Theme.good : Theme.textMuted
    font.pointSize: Theme.fontSmall
}
