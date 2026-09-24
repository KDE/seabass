// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// A write that takes over a page -- a restore onto a stick -- shown on top
// of the page rather than inside it. The progress and the result report
// used to sit at the foot of a long scrolling form, below the fold of any
// ordinary window: pressing Restore opened a dialog, the dialog closed,
// and nothing on screen changed. People waited on a page that looked
// finished, or pressed the button again.
//
// Same scrim as BusyOverlay (the page dims, stays visible, and takes no
// clicks), with a card in the middle holding whatever the page puts in it:
// the progress frame while it runs, the result frame when it is done.
// Covers only the content area it is placed in, not the header, for the
// reason BusyOverlay gives.
Item {
    id: root
    property bool active: false
    property string title: ""
    // The body, stacked in the card under the title.
    default property alias content: body.data

    visible: root.active
    z: 1000

    Rectangle {
        anchors.fill: parent
        color: Theme.background
        opacity: 0.72
    }

    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        preventStealing: true
        onClicked: {}
        onWheel: (wheel) => wheel.accepted = true
    }

    Pane {
        objectName: "transferOverlayCard"
        anchors.centerIn: parent
        width: Math.min(parent.width - 2 * Theme.pageMargin, Theme.scaled(560))
        // A long list of problems scrolls inside the card rather than
        // pushing it past the window edge, which is the fault this
        // overlay exists to fix.
        height: Math.min(parent.height - 2 * Theme.pageMargin, implicitHeight)
        padding: Theme.pageMargin
        background: Rectangle {
            color: Theme.surface
            border.color: Theme.border
            radius: 4
        }

        contentItem: ColumnLayout {
            spacing: Theme.sectionSpacing
            Label {
                Layout.fillWidth: true
                visible: root.title.length > 0
                wrapMode: Text.WordWrap
                text: root.title
                font.bold: true
                font.pointSize: Theme.fontLarge
            }
            ScrollView {
                id: scroller
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.preferredHeight: body.implicitHeight
                contentWidth: availableWidth
                clip: true
                ColumnLayout {
                    id: body
                    width: scroller.availableWidth
                    spacing: Theme.sectionSpacing
                }
            }
        }
    }
}
