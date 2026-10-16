// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// A track being dragged out of Browse or the Matching panel (playlist
// editing, Experimental): a card with the track's cover, title, artist,
// key, BPM and length in Browse's row styling, under the pointer. One per
// page, placed over the page's content: a row's own delegate is clipped
// by its list and can be recycled while the pointer is far away, so the
// card is never part of it. TrackDragArea starts it from a row.
//
// Dropped on a playlist, it does nothing yet. It stays where it landed and
// asks: "Add to <playlist>", and "Move to <playlist>" when Browse shows a
// playlist the track is in. Only a button stages anything, and Save
// writes it, as for every other playlist edit: the user knows what will
// happen before it does. Escape or a click anywhere else lets it go.
//
// The drag contract, which every drop target relies on (the playlist rows
// here, and reordering within a playlist): Drag.keys is
// ["seabass/track"], and the drag source is this item, carrying the
// properties below from sourceId to playlistNames.
Rectangle {
    id: card

    // ---- The dragged track (the contract) ----
    property string sourceId: ""
    property string filePath: ""
    property string title: ""
    property string artist: ""
    property double durationSeconds: 0
    property double bpm: 0
    property string key: ""
    property string artworkPath: ""
    property string fallbackArtworkPath: ""
    property string streamingSource: ""
    property var playlistNames: []

    property string keyNotation: "camelot"

    // What letting go here would do, said on the card while it is dragged
    // ("Hold to move it here" over the track list); empty, nothing said.
    // And whether the card stands back: once a gap in the list is open,
    // the faded row in it is where the track goes, and the card is only
    // the pointer.
    property string hint: ""
    property bool dimmed: false

    // "idle" (not shown), "dragging", "confirming" (dropped, asking) or
    // "closing" (fading out).
    readonly property string phase: internal.phase
    readonly property bool dragging: internal.phase === "dragging"
    readonly property bool confirming: internal.phase === "confirming"
    // The card carries its own drag once it started (DragCarrier): the row
    // it came from may be destroyed while it goes on.
    readonly property bool carried: carrier.carrying
    // The playlist it was dropped on, while it asks.
    readonly property string targetPlaylist: internal.targetPlaylist
    // The playlist a Move takes the track out of; empty, and no Move
    // offered, unless Browse shows a playlist the track is in.
    readonly property string moveFrom: internal.moveFrom
    // Where in the target playlist the track lands, when it was let go at
    // a place in the list (from 0); -1, at the end, as for a playlist row.
    readonly property int targetIndex: internal.targetIndex

    signal addChosen(string playlist)
    signal moveChosen(string playlist, string fromPlaylist)

    // A track with a file on this stick: a streaming track has none, and
    // nothing can be put into a playlist without one.
    function canDrag(track) {
        return track !== null && track !== undefined && (track.filePath || "").length > 0
            && (track.streamingSource || "").length === 0;
    }

    // The track as one map, the contract's fields only: what the page's
    // draggedTrack holds.
    function asMap() {
        return {sourceId: card.sourceId, filePath: card.filePath, title: card.title, artist: card.artist,
                durationSeconds: card.durationSeconds, bpm: card.bpm, key: card.key,
                artworkPath: card.artworkPath, fallbackArtworkPath: card.fallbackArtworkPath,
                streamingSource: card.streamingSource, playlistNames: card.playlistNames};
    }

    // A press on a row that may turn into a drag: take the row's track and
    // stand under the pointer, held by the cover, at `x`, `y` of `from`.
    // Not shown yet: most presses are clicks.
    function arm(track, from, x, y) {
        if (internal.phase === "dragging") {
            return;
        }
        fadeOut.stop();
        internal.phase = "idle";
        card.sourceId = track.sourceId || "";
        card.filePath = track.filePath || "";
        card.title = track.title || "";
        card.artist = track.artist || "";
        card.durationSeconds = track.durationSeconds || 0;
        card.bpm = track.bpm || 0;
        card.key = track.key || "";
        card.artworkPath = track.artworkPath || "";
        card.fallbackArtworkPath = track.fallbackArtworkPath || "";
        card.streamingSource = track.streamingSource || "";
        card.playlistNames = track.playlistNames || [];
        internal.targetPlaylist = "";
        internal.moveFrom = "";
        internal.targetIndex = -1;
        const at = from.mapToItem(card.parent, x, y);
        card.x = at.x - card.Drag.hotSpot.x;
        card.y = at.y - card.Drag.hotSpot.y;
    }

    // The press became a drag: the card plops into existence.
    function start() {
        internal.droppedOn = "";
        internal.phase = "dragging";
        card.opacity = 0;
        plopIn.restart();
        card.Drag.active = true;
        carrier.carry();
    }

    // A playlist row took the drop (its DropArea, through the page), or a
    // place in the playlist shown did: `moveFrom` as the moveFrom above,
    // `atIndex` as targetIndex.
    function dropOn(playlist, moveFrom, atIndex) {
        internal.droppedOn = playlist;
        internal.pendingMoveFrom = moveFrom || "";
        internal.pendingIndex = atIndex === undefined ? -1 : atIndex;
    }

    // The button was let go. Over a playlist the card stays and asks;
    // anywhere else it goes.
    function release() {
        if (internal.phase !== "dragging") {
            return;
        }
        internal.droppedOn = "";
        carrier.putDown();
        card.Drag.drop();
        if (internal.droppedOn.length === 0) {
            card.close();
            return;
        }
        internal.targetPlaylist = internal.droppedOn;
        internal.moveFrom = internal.pendingMoveFrom;
        internal.targetIndex = internal.pendingIndex;
        internal.phase = "confirming";
        // Grown by its buttons, it may reach past the page's edge.
        Qt.callLater(card.keepInside);
        card.forceActiveFocus();
    }

    // Lets it go: Escape, a click elsewhere, a choice made, a drag
    // abandoned. Fades out where it is.
    function close() {
        if (internal.phase === "idle" || internal.phase === "closing") {
            return;
        }
        carrier.putDown();
        card.Drag.active = false;
        internal.phase = "closing";
        plopIn.stop();
        fadeOut.restart();
    }

    function keepInside() {
        if (!card.parent) {
            return;
        }
        const margin = Theme.tightSpacing;
        card.x = Math.max(margin, Math.min(card.x, card.parent.width - card.width - margin));
        card.y = Math.max(margin, Math.min(card.y, card.parent.height - card.height - margin));
    }

    QtObject {
        id: internal
        property string phase: "idle"
        property string targetPlaylist: ""
        property string moveFrom: ""
        // Set by dropOn() while Drag.drop() is delivered.
        property string droppedOn: ""
        property string pendingMoveFrom: ""
        property int targetIndex: -1
        property int pendingIndex: -1
    }

    visible: internal.phase !== "idle"
    // Above the floating Save button (EditSessionHost's 900), below a
    // scan's scrim.
    z: 950

    width: content.implicitWidth + 2 * Theme.tightSpacing
    height: content.implicitHeight + 2 * Theme.tightSpacing
    color: Theme.rowEven
    radius: Theme.cornerRadius
    // The accent outline a current or playing row has: this is the one
    // being acted on.
    border.color: Theme.accent
    border.width: 2

    Drag.keys: ["seabass/track"]
    Drag.supportedActions: Qt.CopyAction | Qt.MoveAction
    Drag.proposedAction: Qt.CopyAction
    // The pointer holds the card by its cover.
    Drag.hotSpot.x: Theme.tightSpacing + Theme.iconSizeNormal / 2
    Drag.hotSpot.y: Theme.tightSpacing + Theme.iconSizeNormal / 2

    // Scaled about the pointer, so the card grows out from under it and
    // what the drop targets see (the hot spot) never moves.
    // Standing back, it also shrinks towards the pointer, off the faded
    // row it would otherwise cover.
    transform: [
        Scale {
            id: plopScale
            origin.x: card.Drag.hotSpot.x
            origin.y: card.Drag.hotSpot.y
            xScale: card.plop
            yScale: card.plop
        },
        Scale {
            origin.x: card.Drag.hotSpot.x
            origin.y: card.Drag.hotSpot.y
            xScale: card.standBack
            yScale: card.standBack
        }
    ]
    readonly property real standBackScale: 0.6
    property real standBack: card.dragging && card.dimmed ? card.standBackScale : 1
    Behavior on standBack {
        NumberAnimation { duration: Theme.shortTransitionDuration }
    }
    property real plop: 1

    ParallelAnimation {
        id: plopIn
        NumberAnimation {
            target: card
            property: "plop"
            from: 0.6
            to: 1
            duration: Theme.shortTransitionDuration
            easing.type: Easing.OutBack
        }
        NumberAnimation {
            target: card
            property: "opacity"
            from: 0
            to: 1
            duration: Theme.shortTransitionDuration
        }
    }
    SequentialAnimation {
        id: fadeOut
        ParallelAnimation {
            NumberAnimation {
                target: card
                property: "opacity"
                to: 0
                duration: Theme.shortTransitionDuration
            }
            NumberAnimation {
                target: card
                property: "plop"
                to: 0.9
                duration: Theme.shortTransitionDuration
            }
        }
        ScriptAction {
            script: {
                internal.phase = "idle";
                card.plop = 1;
                card.opacity = 1;
            }
        }
    }

    Keys.onEscapePressed: card.close()

    // Follows the pointer from the row's MouseArea on, and lets go where
    // the button is let go.
    DragCarrier {
        id: carrier
        objectName: "dragCarrier"
        onMoved: (sceneX, sceneY) => {
            if (!card.parent) {
                return;
            }
            const at = card.parent.mapFromItem(null, sceneX, sceneY);
            card.x = at.x - card.Drag.hotSpot.x;
            card.y = at.y - card.Drag.hotSpot.y;
        }
        onReleased: card.release()
        onCanceled: {
            if (card.dragging) {
                card.close();
            }
        }
    }

    // Clicks on the card itself stay on it: the page lets the card go on a
    // click anywhere else.
    MouseArea {
        anchors.fill: parent
        enabled: card.confirming
        acceptedButtons: Qt.AllButtons
    }

    ColumnLayout {
        id: content
        x: Theme.tightSpacing
        y: Theme.tightSpacing
        spacing: Theme.tightSpacing
        opacity: card.dragging && card.dimmed ? 0.5 : 1
        Behavior on opacity {
            NumberAnimation { duration: Theme.shortTransitionDuration }
        }

        // Browse's row, column for column, at its widths.
        RowLayout {
            spacing: Theme.rowSpacing
            Rectangle {
                Layout.preferredWidth: Theme.iconSizeNormal
                Layout.preferredHeight: Theme.iconSizeNormal
                color: Theme.surface
                ArtworkImage {
                    objectName: "dragArtwork"
                    anchors.fill: parent
                    source: card.artworkPath
                    fallbackSource: card.fallbackArtworkPath
                }
            }
            ColumnLayout {
                spacing: 1
                Layout.preferredWidth: Theme.scaled(200)
                Label {
                    objectName: "dragTitle"
                    text: card.title
                    font.bold: true
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }
                Label {
                    objectName: "dragArtist"
                    text: card.artist
                    color: Theme.textMuted
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }
            }
            KeyBadge {
                keyName: card.key
                notation: card.keyNotation
            }
            Label {
                objectName: "dragBpm"
                text: card.bpm > 0 ? card.formatBpm(card.bpm) : ""
            }
            Label {
                objectName: "dragDuration"
                text: card.formatDuration(card.durationSeconds)
            }
        }

        Label {
            objectName: "dragHint"
            visible: card.dragging && card.hint.length > 0
            text: card.hint
            color: Theme.textMuted
            font.pointSize: Theme.fontSmall
            Layout.alignment: Qt.AlignRight
        }

        // Once dropped: what the drop would do, as a choice.
        RowLayout {
            objectName: "dropChoices"
            visible: card.confirming
            spacing: Theme.tightSpacing
            Layout.alignment: Qt.AlignRight
            Button {
                objectName: "dropAddButton"
                text: qsTr("Add to \"%1\"").arg(card.targetPlaylist)
                highlighted: true
                onClicked: {
                    // Asked for before the card lets go, so the page can
                    // hold the place open until the add is shown there.
                    const playlist = card.targetPlaylist;
                    card.addChosen(playlist);
                    card.close();
                }
            }
            Button {
                objectName: "dropMoveButton"
                visible: card.moveFrom.length > 0
                text: qsTr("Move to \"%1\"").arg(card.targetPlaylist)
                ToolTip.visible: hovered
                ToolTip.delay: 400
                ToolTip.text: qsTr("Add to \"%1\" and take it out of \"%2\"").arg(card.targetPlaylist).arg(card.moveFrom)
                onClicked: {
                    const playlist = card.targetPlaylist;
                    const from = card.moveFrom;
                    card.moveChosen(playlist, from);
                    card.close();
                }
            }
        }
    }

    // Browse's own spellings (ScanPage's formatBpm and formatDuration).
    function formatBpm(value) {
        const oneDecimal = value.toFixed(1);
        return oneDecimal.endsWith(".0") ? oneDecimal.slice(0, -2) : oneDecimal;
    }
    function formatDuration(seconds) {
        const total = Math.round(seconds);
        const s = total % 60;
        return Math.floor(total / 60) + ":" + (s < 10 ? "0" : "") + s;
    }
}
