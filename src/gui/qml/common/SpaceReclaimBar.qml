// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Layouts
import SeabassGui

// The stick's capacity drawn to scale, with the space a cleanup would
// give back marked inside it.
//
// The point is proportion, not another number: "7.8 GB" says nothing
// about whether that matters, while the same figure shown as a block
// against a nearly-full 30 GB stick says the whole story at a glance.
// The counts stay in the legend beneath, so nothing is only conveyed by
// the drawing.
//
// Renders nothing at all when totalBytes is 0 -- that means the capacity
// could not be read (stick unplugged mid-scan, a path not on a stick),
// and an empty bar would read as "this disk is empty", which is a
// different and wrong claim.
Item {
    id: root

    property real totalBytes: 0
    property real freeBytes: 0
    // What the current selection would give back. Drawn inside the used
    // portion, because that is where it is being taken from.
    property real reclaimBytes: 0
    // What every group on the page adds up to, whether ticked or not.
    // Shown as an outline beyond the selection so the difference between
    // "what I picked" and "what is available" is visible.
    property real reclaimableBytes: 0

    readonly property bool known: totalBytes > 0
    readonly property real usedBytes: Math.max(0, totalBytes - freeBytes)

    // The four segments, as bytes. They are carved OUT of usedBytes
    // rather than added beside it: everything reclaimable is space that
    // is in use right now, which is the whole reason reclaiming it
    // helps. Adding it alongside "in use" would draw a bar longer than
    // the disk -- and a bar whose parts do not sum to its capacity is
    // worse than no bar, because it invites exactly the proportional
    // reading it then gets wrong. tst_SpaceReclaimBar guards the sum.
    readonly property real reclaimableClamped: Math.min(reclaimableBytes, usedBytes)
    readonly property real reclaimClamped: Math.min(reclaimBytes, reclaimableClamped)
    readonly property real untouchedBytes: Math.max(0, usedBytes - reclaimableClamped)
    readonly property real notTickedBytes: Math.max(0, reclaimableClamped - reclaimClamped)

    visible: known
    implicitHeight: known ? column.implicitHeight : 0

    function human(bytes) {
        if (bytes <= 0)
            return "0 B"
        var units = ["B", "KB", "MB", "GB", "TB"]
        var v = bytes
        var u = 0
        while (v >= 1024 && u < units.length - 1) {
            v /= 1024
            u++
        }
        return v.toFixed(1) + " " + units[u]
    }

    ColumnLayout {
        id: column
        width: parent.width
        spacing: 10

        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            Text {
                text: root.human(root.reclaimBytes > 0 ? root.reclaimBytes : root.reclaimableBytes)
                font.pixelSize: 30
                font.bold: true
                color: Theme.good
            }
            Text {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                font.pixelSize: 13
                color: Theme.textMuted
                text: root.reclaimBytes > 0
                      ? "would be freed by the " + "groups you've ticked, on a stick with "
                        + root.human(root.freeBytes) + " free"
                      : "available to free across every group below, on a stick with "
                        + root.human(root.freeBytes) + " free"
            }
        }

        // The bar itself. Widths are fractions of the real capacity, so
        // a sliver on screen is a sliver on the stick.
        Item {
            Layout.fillWidth: true
            implicitHeight: 26

            Rectangle {
                anchors.fill: parent
                radius: 3
                color: Theme.groupBackground
                border.color: Theme.borderSubtle
                border.width: 1
            }

            Row {
                id: segments
                anchors.fill: parent
                anchors.margins: 1
                spacing: 2

                // Ordered so the bar reads left to right as "staying put
                // -> could go -> going -> already free": blue is what
                // stays, green is what this page gives back, grey is
                // what was already free. The two greens sit next to the
                // grey they are about to join.
                function span(bytes) {
                    return bytes > 0 ? Math.max(2, width * (bytes / root.totalBytes) - 2) : 0
                }

                // Used space no group on this page touches.
                Rectangle {
                    width: segments.span(root.untouchedBytes)
                    height: parent.height
                    radius: 2
                    color: Theme.info
                    visible: width > 0
                }
                // Reclaimable, but not ticked. A washed-out version of the
                // "freed" green rather than a bare outline: at this height
                // an unfilled rectangle reads as another dark gap, which
                // is what "Free" at the far end already looks like. Same
                // hue, less of it, says "could join that" at a glance.
                Rectangle {
                    width: segments.span(root.notTickedBytes)
                    height: parent.height
                    radius: 2
                    color: Qt.rgba(Theme.good.r, Theme.good.g, Theme.good.b, 0.3)
                    border.color: Theme.good
                    border.width: 1
                    visible: width > 0
                }
                // What the ticked groups give back.
                Rectangle {
                    width: segments.span(root.reclaimClamped)
                    height: parent.height
                    radius: 2
                    color: Theme.good
                    visible: width > 0
                }
                // Already free. Grey rather than transparent: an empty
                // segment reads as a gap in the bar rather than as a
                // quantity, which is misleading when it is often the
                // smallest part of a nearly full stick.
                Rectangle {
                    width: segments.span(root.freeBytes)
                    height: parent.height
                    radius: 2
                    color: Theme.mix(Theme.surface, Theme.text, 0.28)
                    visible: width > 0
                }
            }
        }

        // Legend: identity never rests on colour alone.
        Flow {
            Layout.fillWidth: true
            spacing: 18

            Repeater {
                model: [
                    { label: "In use", value: root.human(root.untouchedBytes),
                      fill: Theme.info, outline: false, show: true },
                    { label: "Reclaimable, not ticked", value: root.human(root.notTickedBytes),
                      fill: Qt.rgba(Theme.good.r, Theme.good.g, Theme.good.b, 0.3), outline: true,
                      show: root.notTickedBytes > 0 },
                    { label: "Freed by your selection", value: root.human(root.reclaimClamped),
                      fill: Theme.good, outline: false, show: root.reclaimClamped > 0 },
                    { label: "Free", value: root.human(root.freeBytes),
                      fill: Theme.mix(Theme.surface, Theme.text, 0.28), outline: false, show: true }
                ]

                delegate: Row {
                    visible: modelData.show
                    spacing: 7

                    Rectangle {
                        width: 10
                        height: 10
                        radius: 2
                        anchors.verticalCenter: parent.verticalCenter
                        color: modelData.fill
                        border.color: modelData.outline ? Theme.good : Theme.borderSubtle
                        border.width: 1
                    }
                    Text {
                        text: modelData.label + " " + modelData.value
                        font.pixelSize: 12
                        color: Theme.textMuted
                    }
                }
            }
        }
    }
}
