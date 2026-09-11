// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// A big icon+title+subtitle menu button -- the tappable tile used on
// StickListPage and both hub pages (DuplicatesHubPage, BackupsHubPage) to
// navigate to a specific feature page. Extracted from StickListPage.qml
// (where it originated as an inline `component`) once a second page needed
// the exact same tile.
Button {
    id: card
    property string cardTitle
    property string cardSubtitle
    property string cardIcon
    property string cardIconFont: "Noto Sans Symbols2"
    // See docs/experimental-features.md. A card marked experimental stays
    // hidden until experimentalFeaturesEnabled is on -- set both from the
    // page embedding this card (experimental: true, experimentalFeaturesEnabled:
    // root.appSettingsController.experimentalFeaturesEnabled), not just
    // the first; leaving the second at its false default would hide the
    // card unconditionally regardless of the user's own setting.
    property bool experimental: false
    property bool experimentalFeaturesEnabled: false
    // A feature that still works but is slated for rework or removal:
    // stays reachable, wears a muted DEPRECATED badge whose tooltip says
    // what is wrong with it.
    property bool deprecated: false
    property string deprecatedNote: "Needs rework"
    // Another Seabass instance is editing the library this card would
    // change (see docs/edit-mode-and-cancel.md): the card stays visible,
    // wears a READ ONLY badge, and a click asks the page to explain
    // (readOnlyClicked) instead of opening the feature. Cards that only
    // read (Browse, Statistics) never set this.
    property bool readOnly: false
    property string readOnlyReason: "Another Seabass instance is editing this library"
    signal readOnlyClicked()
    visible: !experimental || experimentalFeaturesEnabled
    Layout.fillWidth: true
    // A minimum, not a fixed height: a long title next to a badge (a
    // narrow GridLayout column, e.g. StickListPage's 3-column grid,
    // leaves too little room for both on one line) wraps to a second
    // line instead of eliding, and GridLayout equalizes every card in
    // that row to match, so the row stays aligned rather than only the
    // wrapped card growing on its own.
    Layout.minimumHeight: 68

    contentItem: RowLayout {
        spacing: 10
        Label {
            text: card.cardIcon
            font.family: card.cardIconFont
            font.pointSize: Theme.fontHuge
            color: card.enabled && !card.readOnly ? Theme.textMuted : Qt.darker(Theme.textMuted, 1.6)
            Layout.preferredWidth: 30
            horizontalAlignment: Text.AlignHCenter
        }
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2
            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                Label {
                    text: card.cardTitle
                    font.family: Theme.titleFamily
                    font.weight: Theme.cardTitleWeight
                    font.pointSize: Theme.cardTitleSize
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                }
                // Warning-toned (not the muted/neutral badge idiom used
                // elsewhere, e.g. the streaming-source badge) -- this one's
                // meant to read as a caution, not just a label.
                Rectangle {
                    visible: card.experimental
                    radius: 3
                    color: Theme.warnBg
                    border.color: Theme.warnBorder
                    implicitWidth: experimentalBadgeText.implicitWidth + 8
                    implicitHeight: experimentalBadgeText.implicitHeight + 4
                    Label {
                        id: experimentalBadgeText
                        anchors.centerIn: parent
                        text: "EXPERIMENTAL"
                        font.pointSize: Theme.fontTiny
                        font.bold: true
                        color: Theme.warnText
                    }
                }
                Rectangle {
                    objectName: "readOnlyBadge"
                    visible: card.readOnly
                    radius: 3
                    color: Theme.warnBg
                    border.color: Theme.warnBorder
                    implicitWidth: readOnlyBadgeText.implicitWidth + 8
                    implicitHeight: readOnlyBadgeText.implicitHeight + 4
                    Label {
                        id: readOnlyBadgeText
                        anchors.centerIn: parent
                        text: "READ ONLY"
                        font.pointSize: Theme.fontTiny
                        font.bold: true
                        color: Theme.warnText
                    }
                }
                Rectangle {
                    visible: card.deprecated
                    radius: 3
                    color: "transparent"
                    border.color: Theme.textMuted
                    implicitWidth: deprecatedBadgeText.implicitWidth + 8
                    implicitHeight: deprecatedBadgeText.implicitHeight + 4
                    Label {
                        id: deprecatedBadgeText
                        anchors.centerIn: parent
                        text: "DEPRECATED"
                        font.pointSize: Theme.fontTiny
                        font.bold: true
                        color: Theme.textMuted
                    }
                    MouseArea {
                        id: deprecatedHover
                        anchors.fill: parent
                        hoverEnabled: true
                        acceptedButtons: Qt.NoButton
                    }
                    ToolTip.visible: deprecatedHover.containsMouse && card.deprecatedNote.length > 0
                    ToolTip.text: card.deprecatedNote
                }
            }
            Label {
                text: card.cardSubtitle
                color: card.enabled && !card.readOnly ? Theme.textMuted : Qt.darker(Theme.textMuted, 1.6)
                font.pointSize: Theme.baseFontPointSize * 0.9
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
        }
    }

    // Swallows the click while read-only so the page's onClicked never
    // fires; the page hears readOnlyClicked instead.
    MouseArea {
        objectName: "readOnlyGuard"
        anchors.fill: parent
        visible: card.readOnly
        hoverEnabled: true
        cursorShape: Qt.ForbiddenCursor
        onClicked: card.readOnlyClicked()
        ToolTip.visible: containsMouse && card.readOnlyReason.length > 0
        ToolTip.text: card.readOnlyReason
    }
}
