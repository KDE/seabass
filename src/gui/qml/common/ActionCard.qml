// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// A big icon+title+subtitle menu button -- the tappable tile used on
// the home screen (StickToolCards, StickListPage) to navigate to a
// specific feature page. Extracted from StickListPage.qml, where it
// originated as an inline `component`.
Button {
    id: card
    property string cardTitle
    property string cardSubtitle
    // A bundled Breeze icon's name (see SeabassIcon), drawn flat in the
    // subtitle's colour and dimmed with it.
    property string cardIcon
    // A mark in front of the subtitle -- "dialog-warning" when the
    // subtitle is a caution -- or none.
    property string cardSubtitleIcon: ""
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
    // The home screen's bigger tile, two to a row beside the rail: a
    // bigger icon and title, more room around them and a taller floor.
    // Off everywhere else, where the card is exactly what it was.
    property bool large: false
    visible: !experimental || experimentalFeaturesEnabled
    Layout.fillWidth: true
    // A minimum, not a fixed height: a long title next to a badge (a
    // narrow GridLayout column, e.g. StickListPage's 3-column grid,
    // leaves too little room for both on one line) wraps to a second
    // line instead of eliding, and GridLayout equalizes every card in
    // that row to match, so the row stays aligned rather than only the
    // wrapped card growing on its own.
    Layout.minimumHeight: card.large ? 84 : 68
    // Bindings rather than `padding: large ? ... : ...`: the style sets
    // the button's padding, and an ordinary card has to keep the style's
    // value, which only restoring the original binding gives back. All
    // four sides, because a style that sets a side of its own (KDE's
    // does) wins over `padding`, and the large card's text has to sit
    // exactly cardPadding plus its icon in from the edge for the home
    // pane's one left line.
    // The card's own ground, rather than whatever the active style draws
    // there. A card had no background of its own, so its shape was the
    // style's: Material rounded it, Basic draws it square, and on macOS
    // the switch between them turned every card on the home and library
    // pages square while the badges this file draws stayed rounded at 4.
    // One box, two corner radii, and nothing in the app had said either.
    //
    // Same three states the style gave it, taken from Theme so they hold
    // under any style: pressed, hovered, at rest.
    background: Rectangle {
        radius: Theme.cornerRadius
        color: card.down ? Theme.rowPressed : (card.hovered ? Theme.rowHover : Theme.surface)
        border.color: Theme.border
        border.width: 1
    }

    Binding { target: card; property: "leftPadding"; value: Theme.cardPadding; when: card.large }
    Binding { target: card; property: "rightPadding"; value: Theme.cardPadding; when: card.large }
    Binding { target: card; property: "topPadding"; value: Theme.cardPadding; when: card.large }
    Binding { target: card; property: "bottomPadding"; value: Theme.cardPadding; when: card.large }

    // How far the title sits from the card's left edge: the padding, the
    // icon and the gap after it, read off the very values that place it,
    // so a page lining other text up with the cards' titles follows
    // whatever the card does (the home pane's one left line).
    readonly property real textInset: card.leftPadding + cardIconItem.size + cardRow.spacing

    contentItem: RowLayout {
        id: cardRow
        spacing: Theme.rowSpacing
        SeabassIcon {
            id: cardIconItem
            objectName: "cardIcon"
            iconName: card.cardIcon
            size: card.large ? Theme.iconSizeNormal : Theme.iconSizeSmall
            color: card.enabled && !card.readOnly ? Theme.textMuted : Qt.darker(Theme.textMuted, 1.6)
            Layout.alignment: Qt.AlignVCenter
        }
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2
            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                Label {
                    objectName: "cardTitleLabel"
                    text: card.cardTitle
                    font.family: Theme.titleFamily
                    font.weight: Theme.cardTitleWeight
                    font.pointSize: card.large ? Theme.cardTitleSize * 1.1 : Theme.cardTitleSize
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
            RowLayout {
                Layout.fillWidth: true
                spacing: 4
                SeabassIcon {
                    objectName: "cardSubtitleIcon"
                    visible: card.cardSubtitleIcon.length > 0
                    iconName: card.cardSubtitleIcon
                    // One line of the subtitle tall, on its first line.
                    size: subtitleLabel.fontInfo.pixelSize * 1.3
                    color: card.enabled && !card.readOnly ? Theme.warnIcon : Qt.darker(Theme.textMuted, 1.6)
                    Layout.alignment: Qt.AlignTop
                }
                Label {
                    id: subtitleLabel
                    text: card.cardSubtitle
                    color: card.enabled && !card.readOnly ? Theme.textMuted : Qt.darker(Theme.textMuted, 1.6)
                    font.pointSize: Theme.baseFontPointSize * 0.9
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }
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
