// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Shapes
import SeabassGui

// Flat, monochrome device pictogram -- drawn with plain Rectangles/Shape
// fills rather than a font glyph or emoji, so it's guaranteed to render
// as a single flat color with no font-fallback surprises (see the
// mount/unmount ToolButton's font.family fix for what that risk looks
// like in practice).
Item {
    id: root
    property color color: Theme.textMuted
    // BRAINSTORM.md: "show different icon for an SD card for improved
    // clarity" -- best-effort, see DetectedStick::isSdCard's own comment
    // for what "best-effort" means per platform.
    property bool isSdCard: false
    // A library opened from an ordinary directory rather than found on
    // removable media (see MediaController::openFolder): drawn as a
    // folder so the list says at a glance which rows are real hardware.
    // Wins over isSdCard -- a folder is not on a card even when the file
    // it was restored from was.
    property bool isFolder: false
    implicitWidth: Theme.iconSizeSmall
    implicitHeight: Theme.iconSizeSmall

    // USB-stick pictogram: a rounded body plus a small connector prong.
    Item {
        anchors.fill: parent
        visible: !root.isSdCard && !root.isFolder
        Rectangle {
            width: parent.width * 0.30
            height: parent.height * 0.22
            radius: width * 0.2
            color: root.color
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: parent.top
        }
        Rectangle {
            width: parent.width * 0.64
            height: parent.height * 0.7
            radius: width * 0.18
            color: root.color
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
        }
    }

    // SD-card pictogram: the standard card silhouette (a cut top-right
    // corner, held contacts-down) is what actually reads as "SD card"
    // at a glance rather than just "a smaller rectangle" -- not
    // achievable with axis-aligned Rectangles alone, hence the one
    // Shape in this app's whole icon set.
    // Folder pictogram: a body plus the raised tab on the left, which is
    // what separates it from a plain rounded rectangle at this size.
    Item {
        anchors.fill: parent
        visible: root.isFolder
        Rectangle {
            width: parent.width * 0.40
            height: parent.height * 0.16
            radius: height * 0.35
            color: root.color
            anchors.left: parent.left
            anchors.leftMargin: parent.width * 0.07
            anchors.top: parent.top
            anchors.topMargin: parent.height * 0.10
        }
        Rectangle {
            width: parent.width * 0.86
            height: parent.height * 0.60
            radius: width * 0.09
            color: root.color
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: parent.height * 0.14
        }
    }

    Shape {
        anchors.fill: parent
        visible: root.isSdCard
        ShapePath {
            fillColor: root.color
            strokeColor: "transparent"
            startX: root.width * 0.14; startY: root.height * 0.08
            PathLine { x: root.width * 0.72; y: root.height * 0.08 }
            PathLine { x: root.width * 0.86; y: root.height * 0.24 }
            PathLine { x: root.width * 0.86; y: root.height * 0.92 }
            PathLine { x: root.width * 0.14; y: root.height * 0.92 }
            PathLine { x: root.width * 0.14; y: root.height * 0.08 }
        }
    }
}
