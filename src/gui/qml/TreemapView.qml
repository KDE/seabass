// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import SeabassGui

// A simple slice-and-dice treemap (not a true "squarified" layout --
// good enough for a stick's own top-level categories/artists/playlists,
// not worth the extra complexity here), one level at a time: click a box
// with children to drill into it, drillUp() to go back. Filelight itself
// renders every depth at once as nested rings; this shows one level and
// lets the caller keep the navigation stack, a simpler, still useful
// take on the same "where did my space go" idea.
Item {
    id: root
    // {label, sizeBytes, children: [...]} -- see domain::DiskUsageNode's
    // own shape, this is exactly that converted to QVariantMap.
    property var node: null
    signal boxClicked(var childNode)

    readonly property var _palette: [
        Theme.accent, Theme.good, Theme.conflictText, Theme.warnBorder, Theme.danger,
        Qt.lighter(Theme.accent, 1.4), Qt.lighter(Theme.good, 1.4), Qt.lighter(Theme.conflictText, 1.4),
    ]

    function humanBytes(bytes) {
        if (!bytes || bytes <= 0) return "0 B";
        var units = ["B", "KiB", "MiB", "GiB", "TiB"];
        var value = bytes;
        var unitIndex = 0;
        while (value >= 1024 && unitIndex < units.length - 1) {
            value /= 1024;
            unitIndex++;
        }
        return value.toFixed(unitIndex === 0 ? 0 : 1) + " " + units[unitIndex];
    }

    // Slice-and-dice, choosing split orientation by the current rect's
    // own aspect ratio each time (wide rect -> vertical slices, tall
    // rect -> horizontal slices) -- a common, simple approximation of a
    // true squarified treemap that avoids ever-thinner slivers on a
    // list with many children.
    function layoutRects(nodes, x, y, w, h) {
        var total = 0;
        for (var i = 0; i < nodes.length; i++) total += nodes[i].sizeBytes;
        var rects = [];
        if (total <= 0 || nodes.length === 0) return rects;

        var horizontal = w >= h;
        var offset = 0;
        for (i = 0; i < nodes.length; i++) {
            var fraction = nodes[i].sizeBytes / total;
            if (horizontal) {
                var boxW = w * fraction;
                rects.push({node: nodes[i], x: x + offset, y: y, width: boxW, height: h});
                offset += boxW;
            } else {
                var boxH = h * fraction;
                rects.push({node: nodes[i], x: x, y: y + offset, width: w, height: boxH});
                offset += boxH;
            }
        }
        return rects;
    }

    readonly property var _rects: (root.node && root.node.children && root.width > 0 && root.height > 0)
        ? layoutRects(root.node.children, 0, 0, root.width, root.height) : []

    Repeater {
        model: root._rects

        delegate: Rectangle {
            id: box
            required property var modelData
            required property int index

            x: modelData.x
            y: modelData.y
            width: Math.max(0, modelData.width - 2)
            height: Math.max(0, modelData.height - 2)
            color: root._palette[box.index % root._palette.length]
            // Only a box with children can actually be drilled into (see
            // mouseArea.cursorShape below) -- highlighting a leaf box
            // (e.g. "Free space") on hover the same way suggested it was
            // clickable when it wasn't.
            readonly property bool clickable: box.modelData.node.children && box.modelData.node.children.length > 0
            opacity: (box.clickable && mouseArea.containsMouse) ? 1.0 : 0.85
            border.color: Theme.background
            border.width: 1
            radius: 2

            Behavior on opacity { NumberAnimation { duration: 100 } }

            MouseArea {
                id: mouseArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: (box.modelData.node.children && box.modelData.node.children.length > 0)
                    ? Qt.PointingHandCursor : Qt.ArrowCursor
                onClicked: {
                    if (box.modelData.node.children && box.modelData.node.children.length > 0) {
                        root.boxClicked(box.modelData.node);
                    }
                }
                ToolTip.visible: containsMouse
                ToolTip.text: box.modelData.node.label + ": " + root.humanBytes(box.modelData.node.sizeBytes)
            }

            // Label pill in the corner, only when there's room -- a
            // centered label reads poorly on a long thin slice, and text
            // directly on the box color has no guaranteed contrast.
            Rectangle {
                visible: box.width > 50 && box.height > 20
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.margins: 4
                width: Math.min(labelText.implicitWidth + 8, box.width - 8)
                height: labelText.implicitHeight + 4
                radius: 2
                color: Qt.rgba(0, 0, 0, 0.55)

                Label {
                    id: labelText
                    anchors.centerIn: parent
                    width: parent.width - 4
                    elide: Text.ElideRight
                    color: "white"
                    font.pointSize: Theme.fontTiny
                    font.bold: true
                    text: box.modelData.node.label + " (" + root.humanBytes(box.modelData.node.sizeBytes) + ")"
                }
            }
        }
    }

    Label {
        anchors.centerIn: parent
        visible: root._rects.length === 0
        text: "Nothing to show yet."
        color: Theme.textMuted
    }
}
