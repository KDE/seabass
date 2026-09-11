// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// A playlist-picker dropdown: name (elided) + right-aligned track count per
// row, same alternating-shading/hover/press/current-border idiom the rest
// of this app's row delegates use. Model is a plain array of {name, count}
// objects (index 0 is conventionally "All tracks", same as every other
// playlist picker here -- see e.g. ScanController's own playlistNames), the
// component doesn't care what index 0 means, that's a caller decision.
//
// Self-contained (its row delegate is inlined, not a separate shared
// PlaylistRowDelegate.qml) -- there isn't one in this branch to share yet.
// If a shared row delegate exists by the time this merges with other
// in-flight playlist-picker UI work, folding this one into it is a
// reasonable follow-up cleanup, not a redesign.
ComboBox {
    id: root
    // No local `model` property here -- ComboBox already declares one
    // (FINAL in Qt 6, so redeclaring it is a hard QML error, not just
    // shadowing); callers set the inherited one directly, same as they
    // would on a plain ComboBox.
    signal playlistPicked(int index, var modelData)

    textRole: "name"

    delegate: Rectangle {
        id: rowRoot
        required property int index
        required property var modelData

        width: ListView.view ? ListView.view.width : implicitWidth
        height: 32

        color: rowMouseArea.pressed ? Theme.rowPressed
            : rowMouseArea.containsMouse ? Theme.rowHover
            : (rowRoot.index % 2 === 0 ? Theme.rowEven : Theme.rowOdd)
        border.color: rowRoot.index === root.currentIndex ? Theme.accent : "transparent"
        border.width: rowRoot.index === root.currentIndex ? 2 : 0
        radius: rowRoot.index === root.currentIndex ? 4 : 0

        MouseArea {
            id: rowMouseArea
            anchors.fill: parent
            hoverEnabled: true
            onClicked: {
                root.playlistPicked(rowRoot.index, rowRoot.modelData);
                root.popup.close();
            }
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 8
            anchors.rightMargin: 8
            spacing: 6
            Label {
                text: rowRoot.modelData.name
                Layout.fillWidth: true
                elide: Text.ElideRight
            }
            Label {
                text: rowRoot.modelData.count
                color: Qt.darker(Theme.textMuted, 1.3)
                font.pointSize: Theme.fontSmall
                horizontalAlignment: Text.AlignRight
            }
        }
    }

    // The default popup doesn't size itself correctly against a custom
    // item delegate (a plain Rectangle, not an ItemDelegate the style
    // already knows how to measure) -- came out oversized with rows barely
    // visible inside it. Sizing it explicitly, the documented way to
    // customize a ComboBox popup: width matches the combo, height matches
    // the real row count up to a cap, with its own scrollbar past that.
    popup: Popup {
        y: root.height
        width: root.width
        implicitHeight: Math.min(contentItem.implicitHeight, 320)
        padding: 1

        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: root.popup.visible ? root.delegateModel : null
            // A custom popup replaces the default wiring that would
            // otherwise apply the ComboBox's own `delegate:` automatically
            // -- without this, the popup sizes correctly (real rows, real
            // height) but renders nothing into any of them.
            delegate: root.delegate
            currentIndex: root.highlightedIndex
            ScrollBar.vertical: BigScrollBar {}
        }
        background: Rectangle {
            color: Theme.surface
            border.color: Theme.border
            radius: 4
        }
    }
}
