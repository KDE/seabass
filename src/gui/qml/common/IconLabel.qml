// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import SeabassGui

// A Label with an icon in front of its first line: what a line of text
// that used to begin with a glyph ("⚠ 3 of 40 entries…") uses now that
// icons are SVGs, which cannot go inside a string. The icon is one line
// tall, and lines that wrap stay indented past it.
Label {
    id: root
    // A bundled Breeze icon's name (see SeabassIcon), or none.
    property string iconName: ""
    property color iconColor: root.color

    leftPadding: root.iconName.length > 0 ? mark.width + Theme.rowSpacing / 2 : 0

    FontMetrics {
        id: metrics
        font: root.font
    }
    SeabassIcon {
        id: mark
        visible: root.iconName.length > 0
        iconName: root.iconName
        size: Math.round(metrics.height)
        color: root.iconColor
        y: root.topPadding
    }
}
