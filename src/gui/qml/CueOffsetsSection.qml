// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// Sync Cue Points' "Cues a few milliseconds off" section (#121): tracks
// whose cues sit a few tens of milliseconds from the sound on one side,
// left so by older Seabass builds (copied without the MP3 lead-in shift) or
// by a Denon player's first load. Sync calls such a pair the same cue (it is
// well within half a beat), so this section, not the list below, owns them.
//
// A row whose origin is known (the stick's log, or the player's own shift)
// has one button, and "Move All Known" stages every such row. A row nothing
// explains shows both positions and two buttons, one a side: it is moved
// only by the DJ's pick. Everything stages; Save writes it, backed up first,
// and Undo Last Save puts it back.
Rectangle {
    id: root
    required property var controller
    property bool expanded: false

    objectName: "cueOffsetsSection"
    visible: controller.cueOffsets.count > 0
    Layout.fillWidth: true
    implicitHeight: visible ? column.implicitHeight + 2 * Theme.tightSpacing : 0
    color: Theme.warnBg
    border.color: Theme.warnBorder
    radius: Theme.cornerRadius

    ColumnLayout {
        id: column
        anchors.fill: parent
        anchors.margins: Theme.tightSpacing
        anchors.leftMargin: Theme.rowSpacing
        spacing: Theme.tightSpacing

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.rowSpacing
            SeabassIcon {
                iconName: "dialog-warning"
                size: Theme.scaled(24)
                color: Theme.warnIcon
                Layout.alignment: Qt.AlignVCenter
            }
            Label {
                objectName: "cueOffsetsSummary"
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Theme.warnText
                text: {
                    const count = root.controller.cueOffsets.count;
                    return (count === 1 ? "1 track has" : count + " tracks have")
                        + " cues a few milliseconds off on one side.";
                }
            }
            Label {
                visible: root.controller.cueOffsetStagedCount > 0
                text: root.controller.cueOffsetStagedCount + " staged, not saved yet"
                color: Theme.warnText
                font.pointSize: Theme.fontSmall
            }
            Button {
                objectName: "moveAllKnownButton"
                text: "Move All Known"
                visible: root.controller.cueOffsetKnownCount > 0
                enabled: !root.controller.busy && !root.controller.writing
                ToolTip.visible: hovered
                ToolTip.text: "Stage moving every track whose wrong side the stick's log or the player's own shift "
                    + "names. Tracks nothing explains are left for you to pick. Save writes it."
                onClicked: root.controller.stageKnownCueOffsets()
            }
            Button {
                objectName: "cueOffsetsToggle"
                text: root.expanded ? "Hide" : "Show"
                onClicked: root.expanded = !root.expanded
            }
        }

        ListView {
            id: offsetList
            objectName: "cueOffsetsList"
            visible: root.expanded
            Layout.fillWidth: true
            Layout.preferredHeight: visible ? Math.min(contentHeight, Theme.scaled(320)) : 0
            interactive: contentHeight > height
            clip: true
            spacing: Theme.tightSpacing
            model: root.controller.cueOffsets
            ScrollBar.vertical: BigScrollBar {}

            delegate: ColumnLayout {
                id: offsetRow
                required property int index
                required property string title
                required property string artist
                required property string side
                required property string reason
                required property string cues
                required property string rekordboxName
                required property string stagedSide
                required property bool hasKnown
                required property bool hasUnknown

                // Clear of the scroll bar, which overlays the list.
                width: ListView.view.width - Theme.pageMargin
                spacing: Theme.tightSpacing / 2

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.rowSpacing
                    Label {
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                        font.bold: true
                        text: offsetRow.artist.length > 0 ? offsetRow.title + ", " + offsetRow.artist : offsetRow.title
                    }
                    Label {
                        visible: offsetRow.stagedSide.length > 0
                        text: "staged"
                        color: Theme.warnText
                        font.pointSize: Theme.fontSmall
                    }
                    // The cues whose side is known: one button, each cue
                    // moved on the side its evidence names.
                    Button {
                        visible: offsetRow.hasKnown && offsetRow.stagedSide.length === 0
                        text: offsetRow.side === "engine" ? "Move Engine's"
                            : offsetRow.side === "rekordbox" ? "Move " + offsetRow.rekordboxName + "'s"
                            : "Move Known"
                        enabled: !root.controller.busy && !root.controller.writing
                        ToolTip.visible: hovered
                        ToolTip.text: offsetRow.hasUnknown
                            ? "Move the cues whose wrong side is known; the others stay until you pick a side"
                            : "Move the cues on the side that is off"
                        onClicked: root.controller.stageCueOffset(offsetRow.index, "")
                    }
                    // The cues nothing explains: the DJ picks the side that
                    // is right. The known ones still move on their own side.
                    Button {
                        visible: offsetRow.hasUnknown && offsetRow.stagedSide.length === 0
                        text: offsetRow.rekordboxName + " Is Right"
                        enabled: !root.controller.busy && !root.controller.writing
                        ToolTip.visible: hovered
                        ToolTip.text: "Move Engine's cues that nothing explains to where " + offsetRow.rekordboxName
                            + "'s say"
                        onClicked: root.controller.stageCueOffset(offsetRow.index, "engine")
                    }
                    Button {
                        visible: offsetRow.hasUnknown && offsetRow.stagedSide.length === 0
                        text: "Engine Is Right"
                        enabled: !root.controller.busy && !root.controller.writing
                        ToolTip.visible: hovered
                        ToolTip.text: "Move " + offsetRow.rekordboxName + "'s cues that nothing explains to where "
                            + "Engine's say"
                        onClicked: root.controller.stageCueOffset(offsetRow.index, "rekordbox")
                    }
                    Button {
                        visible: offsetRow.stagedSide.length > 0
                        text: "Unstage"
                        enabled: !root.controller.writing
                        onClicked: root.controller.unstageCueOffset(offsetRow.index)
                    }
                }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: Theme.textMuted
                    font.pointSize: Theme.fontSmall
                    text: offsetRow.reason
                }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: Theme.textMuted
                    font.pointSize: Theme.fontSmall
                    text: offsetRow.cues
                }
            }
        }
    }
}
