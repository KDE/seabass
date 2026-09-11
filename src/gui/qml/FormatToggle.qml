// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Rekordbox/Engine mode switch shared by ScanPage, DuplicatesPage and
// LocalCuePage -- bound to AppSettingsController.preferredFormat, so the
// last-chosen format is remembered across all three (and across app
// restarts). Whichever side isn't present on this stick is disabled
// rather than hidden, so the control's shape stays stable.
RowLayout {
    id: root
    required property var appSettingsController
    property bool hasRekordbox: true
    property bool hasEngine: true
    spacing: 4

    ButtonGroup { id: group }

    // "◎"/"⬡" are original, generic glyphs -- not reproductions of either
    // company's actual logo -- used purely so the two modes are visually
    // distinct at a glance. See AboutPage.qml for the trademark note. Kept
    // in their own Label (not appended into the button's text) since the
    // symbol font that has these glyphs has no Latin letters of its own.
    component FormatButton: Button {
        id: btn
        property string glyph
        property string label
        checkable: true
        flat: !checked
        contentItem: RowLayout {
            spacing: 4
            Label {
                text: btn.glyph
                font.family: "Noto Sans Symbols2"
                color: btn.checked ? Theme.text : btn.palette.buttonText
            }
            Label {
                text: btn.label
                color: btn.checked ? Theme.text : btn.palette.buttonText
            }
        }
        // Explicit background rather than the active style's default --
        // see LibrarySourceToggle.qml's SourceButton for why (KDE's
        // Breeze style draws square corners here regardless of this
        // app's own Material palette).
        background: Rectangle {
            radius: 4
            color: btn.checked ? Theme.primary : (btn.hovered ? Theme.rowHover : "transparent")
            border.color: Theme.border
            border.width: btn.checked ? 0 : 1
        }
    }

    FormatButton {
        glyph: "◎"
        label: "DeviceLibrary"
        checked: root.appSettingsController.preferredFormat === "rekordbox"
        enabled: root.hasRekordbox
        ButtonGroup.group: group
        onClicked: root.appSettingsController.preferredFormat = "rekordbox"
    }
    FormatButton {
        glyph: "⬡"
        label: "Engine"
        checked: root.appSettingsController.preferredFormat === "engine"
        enabled: root.hasEngine
        ButtonGroup.group: group
        onClicked: root.appSettingsController.preferredFormat = "engine"
    }
}
