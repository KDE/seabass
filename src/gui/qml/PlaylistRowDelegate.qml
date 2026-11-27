// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// One playlist row: name (elided) + track count, right-aligned in a
// slightly darker tone than the name -- same alternating-shading/hover/
// press/accent-border-when-current idiom trackListView's own delegate
// uses. Shared by PlaylistListView.qml (the left Pane / off-canvas
// Drawer) and the "This Playlist" ComboBox's own popup delegate in
// MatchingPage.qml, so both render identically instead of
// drifting apart.
Rectangle {
    id: root
    required property int index
    required property string name
    property int count: 0
    property bool isCurrent: false
    // Staged for deletion (Browse's playlist editing): the
    // name is struck through until Save deletes it or Keep takes it back.
    property bool pendingDelete: false
    // Staged to be created: in italics, "new" for a count, until Save
    // makes it.
    property bool pendingNew: false
    // Tracks staged into or out of it: bold italics, and the count it
    // will have after Save in parentheses.
    property bool edited: false
    property int countChange: 0
    // A dragged track (TrackDragItem) may be dropped here. While one
    // hovers, and while the drag card asks about the drop made here
    // (heldDropTarget), the row wears the current row's accent outline.
    property bool acceptsTrackDrop: false
    property bool heldDropTarget: false
    readonly property bool dropTarget: (trackDropArea.containsDrag && root.acceptsTrackDrop) || root.heldDropTarget
    signal trackDropped()
    // Audio files dragged in from the computer (Browse's track import)
    // may be dropped here; the row wears the same outline while they
    // hover.
    property bool acceptsFileDrop: false
    readonly property bool fileDropTarget: fileDropArea.containsDrag
    signal filesDropped(var urls)
    // Staged to be renamed, listed under the new name: in italics like a
    // new one, its count as it is (a rename alone changes no count, so it
    // shows none in parentheses).
    property bool pendingRename: false
    readonly property bool tracksEdited: root.edited && !(root.pendingRename && root.countChange === 0)
    // The playlist's small cover (ScanController.playlistThumbnails), left
    // of its name, where the host shows them (showsThumbnail); "" draws
    // the neutral tile, a playlist with no covers.
    property bool showsThumbnail: false
    property string thumbnailSource: ""
    // An icon in the cover's place, for a row that is no playlist and has
    // no covers to show (Browse's Own Recordings).
    property string iconName: ""
    signal picked()
    // A tick box before the name, where the host picks playlists to act
    // on (Beat Grids & Waveforms): checkState is the host's (Qt.Unchecked,
    // Qt.PartiallyChecked, Qt.Checked), and a click asks for the whole
    // playlist ticked, or for none of it once all of it is.
    property bool checkable: false
    property int checkState: Qt.Unchecked
    property bool checkEnabled: true
    property string checkTooltip: ""
    signal checkToggled(bool checked)
    // Right click, or a long press on touch: Browse offers its playlist
    // menu here. The Matching page's combo ignores it.
    signal contextRequested()

    // What a playlist list needs for its summary (PlaylistListView):
    // whether the pointer is on this row, and a press or the wheel in it,
    // which put the summary away. The row has no popup of its own.
    readonly property bool rowHovered: rowHover.hovered
    signal summaryDismissed()

    // The same fallback the other two pickers needed. This delegate is
    // used inside a ComboBox popup on the Matching page as well as in a
    // plain list, and in the popup the rows are built through a
    // DelegateModel, where the ListView.view attachment comes back null.
    // implicitWidth for a row like this is nearly nothing, so the row
    // paints and cannot be clicked -- which is what the playlist picker
    // was doing under org.kde.desktop until tonight.
    width: ListView.view ? ListView.view.width
                         : (parent ? parent.width : implicitWidth)
    height: 32

    color: mouseArea.pressed ? Theme.rowPressed
        : rowHover.hovered ? Theme.rowHover
        : (root.index % 2 === 0 ? Theme.rowEven : Theme.rowOdd)
    border.color: root.isCurrent || root.dropTarget || root.fileDropTarget ? Theme.accent : "transparent"
    border.width: root.isCurrent || root.dropTarget || root.fileDropTarget ? 2 : 0
    radius: Theme.cornerRadius

    MouseArea {
        id: mouseArea
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        onClicked: mouse => mouse.button === Qt.RightButton ? root.contextRequested() : root.picked()
        onPressAndHold: root.contextRequested()
        onPressed: root.summaryDismissed()
        // Passed on, so the list still scrolls.
        onWheel: wheel => {
            root.summaryDismissed();
            wheel.accepted = false;
        }
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 8
        // Clear of the list's scrollbar (BigScrollBar, 10 px, drawn over
        // the content): the track counts are right-aligned, so without
        // this the bar sat on top of the numbers.
        anchors.rightMargin: 8 + Theme.scaled(10)
        spacing: 6
        SeabassCheckBox {
            id: rowCheck
            objectName: "playlistRowCheck"
            visible: root.checkable
            Layout.alignment: Qt.AlignVCenter
            // Every side, not `padding`: a style that sets a side itself
            // (FluentWinUI3 does) wins over it.
            leftPadding: 0
            rightPadding: 0
            topPadding: 0
            bottomPadding: 0
            enabled: root.checkEnabled
            checkState: root.checkState
            ToolTip.visible: hovered && root.checkTooltip.length > 0
            ToolTip.delay: 400
            ToolTip.text: root.checkTooltip
            onClicked: {
                // The host's state decides; the click only asks.
                const wanted = root.checkState !== Qt.Checked;
                rowCheck.checkState = Qt.binding(() => root.checkState);
                root.checkToggled(wanted);
            }
        }
        PlaylistThumbnail {
            id: thumbnailTile
            objectName: "playlistThumbnailTile"
            visible: root.showsThumbnail && root.iconName.length === 0
            Layout.preferredWidth: implicitWidth
            Layout.preferredHeight: implicitHeight
            Layout.alignment: Qt.AlignVCenter
            source: root.showsThumbnail ? root.thumbnailSource : ""
        }
        // As wide as the tile it stands for, so the names stay on one line.
        Item {
            objectName: "playlistRowIcon"
            visible: root.showsThumbnail && root.iconName.length > 0
            Layout.preferredWidth: thumbnailTile.implicitWidth
            Layout.preferredHeight: thumbnailTile.implicitHeight
            Layout.alignment: Qt.AlignVCenter
            SeabassIcon {
                anchors.centerIn: parent
                iconName: root.iconName.length > 0 ? root.iconName : "media-record"
                size: Math.min(parent.width, parent.height) * 0.75
                color: Theme.textMuted
            }
        }
        Label {
            objectName: "playlistRowName"
            text: root.name
            Layout.fillWidth: true
            elide: Text.ElideRight
            font.strikeout: root.pendingDelete
            font.italic: root.pendingNew || root.edited || root.pendingRename
            font.bold: root.tracksEdited
            color: root.pendingDelete || root.pendingNew || root.pendingRename ? Theme.textMuted : Theme.text
        }
        Label {
            objectName: "playlistRowCount"
            text: root.pendingNew ? (root.countChange > 0 ? qsTr("new (%1)").arg(root.countChange) : qsTr("new"))
                : root.tracksEdited ? qsTr("%1 (%2)").arg(root.count).arg(root.count + root.countChange)
                : root.count
            color: Qt.darker(Theme.textMuted, 1.3)
            font.pointSize: Theme.fontSmall
            horizontalAlignment: Text.AlignRight
        }
    }

    DropArea {
        id: trackDropArea
        objectName: "playlistTrackDropArea"
        anchors.fill: parent
        enabled: root.acceptsTrackDrop
        keys: ["seabass/track"]
        onDropped: drop => {
            drop.accept(Qt.CopyAction);
            root.trackDropped();
        }
    }

    DropArea {
        id: fileDropArea
        objectName: "playlistFileDropArea"
        anchors.fill: parent
        enabled: root.acceptsFileDrop
        keys: ["text/uri-list"]
        onDropped: drop => {
            if (drop.hasUrls) {
                drop.accept(Qt.CopyAction);
                root.filesDropped(drop.urls);
            }
        }
    }

    // The row's hover, for its shading and its summary. A handler, not
    // the MouseArea's own hover: under org.kde.desktop a Label carries a
    // HoverHandler of its own, and the MouseArea under the row's labels
    // never hears the pointer arrive, so the row shaded only in its
    // margins.
    HoverHandler { id: rowHover }
}
