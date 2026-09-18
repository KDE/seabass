// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// Opened from the heart button in StickListPage.qml's header.
//
// Sebastian's own words, kept as he wrote them: this page asks for
// something, and asking in someone else's voice reads as a form letter.
// The links open in the browser rather than being spelled out, except
// where the address is the point.
Page {
    id: root

    header: ToolBar {
        // Every side zeroed so the header's inset is Theme.pageMargin
        // and nothing else. `padding` alone does not do it: styles set
        // horizontalPadding or leftPadding of their own on top of it,
        // 4px under Breeze and 6 under the default style, and that is
        // exactly how far right of the body the breadcrumb used to sit.
        leftPadding: 0
        rightPadding: 0
        topPadding: 0
        bottomPadding: Theme.headerBottomPadding
        // Opaque background override, see AppSettingsPage.qml's header
        // for why (KDE's Breeze style bleeds the window behind Seabass
        // through an unstyled ToolBar).
        background: Rectangle { color: Theme.surface }

        RowLayout {
            anchors.fill: parent
            anchors.margins: Theme.pageMargin
            BackBreadcrumb {
                title: "Support Seabass"
                onHomeRequested: root.StackView.view.pop(null)
            }
            Item { Layout.fillWidth: true }
        }
    }

    Flickable {
        anchors.fill: parent
        contentWidth: width
        contentHeight: content.implicitHeight + 64
        clip: true
        // See AboutPage: a Flickable with nothing to scroll still drags
        // into an elastic overshoot, which reads as a bug.
        interactive: contentHeight > height

        ColumnLayout {
            id: content
            x: Math.max(32, (parent.width - width) / 2)
            width: Math.min(parent.width - 64, 640)
            y: 32
            spacing: 20

            HeartIcon {
                iconSize: Theme.iconSizeLarge
                color: "#aa0000"
                Layout.alignment: Qt.AlignHCenter
            }

            Label {
                objectName: "supportTitle"
                text: "Supporting Seabass"
                font.family: Theme.titleFamily
                font.weight: Theme.titleWeight
                font.pointSize: Theme.titleLarge
                Layout.alignment: Qt.AlignHCenter
            }

            Label {
                objectName: "supportByline"
                // The heart is the page's own mark, in the sentence it
                // belongs to rather than as an emoji the font may not
                // have.
                text: "Seabass is created with love by Sebastian K\u00fcgler (a.k.a. Whaleshark) and friends."
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
                font.pointSize: Theme.baseFontPointSize * 1.1
                Layout.fillWidth: true
            }

            Label {
                objectName: "supportKindWords"
                textFormat: Text.StyledText
                linkColor: Theme.accent
                text: "Kind words mean a lot to me. If you like Seabass, let me know! An endorsement from a "
                    + "fellow DJ goes a long way in making my day a bit brighter. Send an email to "
                    + "<a href=\"mailto:sebas@kde.org\">sebas@kde.org</a>"
                wrapMode: Text.WordWrap
                font.pointSize: Theme.baseFontPointSize * 1.1
                Layout.fillWidth: true
                onLinkActivated: link => Qt.openUrlExternally(link)
            }

            Label {
                objectName: "supportCosts"
                text: "With that said, making Seabass available for free isn't free for me. Aside from my time, "
                    + "hardware to test with I also have to pay for various services that keep this project "
                    + "going. Chipping in is hugely welcome."
                wrapMode: Text.WordWrap
                font.pointSize: Theme.baseFontPointSize * 1.1
                Layout.fillWidth: true
            }

            Label {
                objectName: "supportDonate"
                textFormat: Text.StyledText
                linkColor: Theme.accent
                text: "You can either send a one-time donation or become a regular supporter of Seabass over "
                    + "on <a href=\"https://kde.org/donate/\">Patreon</a>."
                wrapMode: Text.WordWrap
                font.pointSize: Theme.baseFontPointSize * 1.1
                Layout.fillWidth: true
                onLinkActivated: link => Qt.openUrlExternally(link)
            }

            Label {
                objectName: "supportKde"
                textFormat: Text.StyledText
                linkColor: Theme.accent
                text: "If you'd rather donate money to the KDE community, this is of great value for Seabass "
                    + "as well, you can do so at <a href=\"https://kde.org/donate\">kde.org/donate</a>."
                wrapMode: Text.WordWrap
                font.pointSize: Theme.baseFontPointSize * 1.1
                Layout.fillWidth: true
                onLinkActivated: link => Qt.openUrlExternally(link)
            }

            Label {
                objectName: "supportThanks"
                text: "Thank you."
                font.pointSize: Theme.baseFontPointSize * 1.1
                Layout.fillWidth: true
            }
        }
    }
}
