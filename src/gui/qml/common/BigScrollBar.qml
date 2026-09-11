// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import SeabassGui

// The real QtQuick.Controls ScrollBar, just re-skinned -- the platform
// style's own auto-attached overlay scrollbar (KDE Breeze, on this
// project's dev desktop) renders as a near-invisible hairline, hard to
// even notice let alone grab, so every scrollable list uses this instead
// of relying on the style default. Assign to a Flickable's
// `ScrollBar.vertical` attached property.
//
// Sized and toned down to read as a native overlay scrollbar rather than
// a custom widget -- thin, and its track only appears on hover/press
// (matching how a real native overlay scrollbar behaves), just
// noticeably easier to see and grab than the platform default that
// prompted this file to exist in the first place.
ScrollBar {
    id: control
    policy: ScrollBar.AsNeeded
    implicitWidth: 10
    padding: 2

    contentItem: Rectangle {
        implicitWidth: 6
        implicitHeight: 40
        radius: width / 2
        color: control.pressed ? Theme.accent : Theme.textMuted
        opacity: control.pressed ? 0.85 : (control.active ? 0.6 : 0.35)
        Behavior on opacity { NumberAnimation { duration: 150 } }
    }

    background: Rectangle {
        implicitWidth: 10
        radius: width / 2
        color: Theme.groupBackground
        opacity: control.active ? 1.0 : 0.0
        Behavior on opacity { NumberAnimation { duration: 150 } }
    }
}
