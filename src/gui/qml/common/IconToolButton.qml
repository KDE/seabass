// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import SeabassGui

// Every icon-only tool button in Seabass, with the glyph Seabass draws
// itself.
//
// Under KDE's desktop style (org.kde.desktop) a ToolButton has no content
// item: its background asks QStyle to paint the whole button, icon and
// all, and QStyle paints a bundled SVG in its own tone and ignores
// icon.color. On Kelp that came out a dim grey on a dark ground, the
// header's About and Preferences almost impossible to read while the
// hand-drawn heart beside them was fine. Basic and Material honour
// icon.color, so neither the Mac nor the default test lane saw it.
//
// So the style keeps the button's frame, hover and press, and the glyph
// is a SeabassIcon in `iconColor`: the same ink under every style
// (tst_IconToolButton measures it). Set `iconName`, not icon.source; a
// disabled button is dimmed the way SeabassCheckBox is, by opacity, so
// there is no need to pass a dimmer colour. `text` is still what an
// assistive reader announces.
ToolButton {
    id: control

    // A bundled Breeze icon's name (see Theme.iconUrl).
    property string iconName
    property color iconColor: Theme.text
    // Width and height of the glyph, in pixels. By default Breeze's own
    // toolbar size, which is what KDE's style drew a raw ToolButton's
    // icon at, so the button keeps the size it had there (Basic's frame
    // is larger than either and does not change).
    property real iconSize: Theme.scaled(22)

    // Still needed with a content item of our own: the KDE style paints
    // the label itself, over the glyph, unless told the button is
    // icon-only. The icon's size (with no source) keeps the style's
    // frame the size it gave the button when it drew the icon itself.
    display: AbstractButton.IconOnly
    icon.width: control.iconSize
    icon.height: control.iconSize

    contentItem: Item {
        implicitWidth: control.iconSize
        implicitHeight: control.iconSize
        SeabassIcon {
            objectName: "iconToolButtonGlyph"
            anchors.centerIn: parent
            iconName: control.iconName
            size: control.iconSize
            color: control.iconColor
            opacity: control.enabled ? 1 : 0.5
        }
    }
}
