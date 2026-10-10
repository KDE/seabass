// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import SeabassGui

// A table column header that sorts its table: TableHeaderLabel's eyebrow
// type, clickable, with a small arrow after the label on the column the
// table is sorted by (up for ascending, down for descending).
//
// The page owns the sort; this only shows it and asks for a change.
// `sortField` and `sortAscending` are the page's current sort, and a
// click (or Space/Enter with focus) emits sortRequested(sortKey). The
// page decides what that means: a new column sorts ascending, the
// sorted one flips, and a column sorted descending goes back to the
// table's own order, which `unsortedName` names for the tooltip.
//
// The arrow's room is part of the header's width whether the arrow is
// shown or not, so a header never changes width when its column becomes
// the sorted one: the columns are fixed widths that mirror the rows
// below, and a header that grew would push its neighbours off them.
Item {
    id: root
    required property string label
    required property string sortKey
    property string sortField: ""
    property bool sortAscending: true
    // What the table goes back to after descending, as the tooltip says it.
    property string unsortedName: "the original order"
    readonly property bool active: root.sortField === root.sortKey

    signal sortRequested(string key)

    // The same chevron and size as LibrarySourceToggle's indicator; its
    // drawn stroke is about the height of the label's lower case.
    readonly property real indicatorSize: Theme.iconSizeSmall * 0.5
    // Breeze draws the chevron inside a 22 px box with room either side,
    // so the arrow sits a little off the label without a gap of its own.
    readonly property real indicatorGap: Theme.scaled(1)

    implicitWidth: caption.implicitWidth + root.indicatorGap + root.indicatorSize
    // The label's height alone: the icon's box is mostly empty above and
    // below the chevron, and the header row keeps the height it had.
    implicitHeight: caption.implicitHeight

    activeFocusOnTab: true
    Accessible.role: Accessible.ColumnHeader
    Accessible.name: root.label
    // What the header's tooltip says: what a click will do.
    readonly property string toolTipText: {
        if (!root.active) {
            return "Sort by " + root.label;
        }
        return root.sortAscending
            ? "Sorted by " + root.label + ", ascending. Click to sort descending"
            : "Sorted by " + root.label + ", descending. Click for " + root.unsortedName;
    }
    Accessible.description: root.toolTipText
    Accessible.onPressAction: root.sortRequested(root.sortKey)

    Keys.onSpacePressed: root.sortRequested(root.sortKey)
    Keys.onReturnPressed: root.sortRequested(root.sortKey)
    Keys.onEnterPressed: root.sortRequested(root.sortKey)

    // The hover pill reaches a little past the label on each side, so
    // the label itself stays on the column's left line with the rows.
    Rectangle {
        objectName: "sortHeaderHover"
        x: -Theme.scaled(4)
        y: -Theme.scaled(2)
        // Around the label and its arrow, not the whole column: Title
        // fills the list's width, and a pill that wide reads as a row.
        width: caption.width + root.indicatorGap + root.indicatorSize + Theme.scaled(8)
        height: root.height + Theme.scaled(4)
        radius: Theme.scaled(4)
        color: tapArea.pressed ? Theme.rowPressed : Theme.rowHover
        visible: tapArea.containsMouse || root.activeFocus
    }

    TableHeaderLabel {
        id: caption
        objectName: "sortHeaderLabel"
        label: root.label
        anchors.verticalCenter: parent.verticalCenter
        width: Math.min(implicitWidth, root.width - root.indicatorGap - root.indicatorSize)
        elide: Text.ElideRight
        // The sorted column's label joins its arrow in full ink.
        color: root.active || tapArea.containsMouse ? Theme.text : Theme.textMuted
    }

    SeabassIcon {
        objectName: "sortIndicator"
        visible: root.active
        iconName: root.sortAscending ? "arrow-up" : "arrow-down"
        size: root.indicatorSize
        color: Theme.text
        x: Theme.snap(caption.width + root.indicatorGap)
        y: Theme.snap((root.height - height) / 2)
    }

    MouseArea {
        id: tapArea
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: root.sortRequested(root.sortKey)
    }

    ToolTip {
        visible: tapArea.containsMouse
        delay: 400
        text: root.toolTipText
    }
}
