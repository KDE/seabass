import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// One track row, shared by Metadata Backup's stored list and Restore
// Metadata's proposal list.
//
// The two pages ask different questions of the same object -- "what have
// I got backed up" and "what would go back on the stick" -- and a row
// answering either has the same anatomy: a tick box, a cover, a name, a
// length, a rating, a cue count, and a page-specific action or two on
// the end. They had drifted into two delegates that agreed on none of
// it: one showed a rating and the other did not, one put the stick label
// in the collapsed row where it competed with the title, and only one
// could be expanded at all.
//
// Page-specific controls are passed as a list and land at the right-hand
// end of the row:
//
//     MetadataTrackDelegate {
//         title: model.title
//         actionItems: [
//             Button { text: "Restore"; onClicked: ... }
//         ]
//     }
//
// A list property rather than the default one: a component whose own
// body is a layout cannot also have a default property alias, because
// the layout would be reparented into the alias along with the caller's
// children.
//
// Everything the expanded half shows is a property here rather than a
// lookup: a delegate is built for every row the list scrolls past, so a
// component that queried a database to build itself would turn a paged
// list back into a full one. The one exception is cueTooltip, which a
// page is free to bind against `hovered` so it costs only the row the
// pointer is over.
Rectangle {
    id: delegate

    // ---- identity ----------------------------------------------------
    required property int index
    property string title: ""
    property string artist: ""
    property string filename: ""
    property string relativePath: ""
    property string durationText: ""
    property string artworkUrl: ""

    // ---- what the DJ put on it ---------------------------------------
    // -1 means unrated, which is a different fact from zero stars.
    property int rating: -1
    property string comment: ""
    property int cueCount: 0
    // Every cue on its own line.
    property string cueTooltip: ""
    property string cueBadgeLabel: delegate.cueCount + (delegate.cueCount === 1 ? " cue" : " cues")
    property color cueBadgeColor: Theme.good
    property string playlistNames: ""

    // ---- where it came from ------------------------------------------
    // Shown in the expanded half only. It used to sit in the collapsed
    // row, where a long stick label elided the thing next to it and
    // nobody was reading it anyway: "which stick was this last seen on"
    // is a question you ask about one track, having already found it.
    property string storedFrom: ""
    property string storedAt: ""

    // ---- state -------------------------------------------------------
    property bool selectable: true
    property bool selected: false
    // What ticking this row does, which is not the same on both pages:
    // on one it stages a restore and on the other it marks an entry to
    // be forgotten. A tick box that does something destructive has to
    // say so before it is clicked, not after.
    property string selectTooltip: "Select this track"
    property bool expanded: false
    // Struck through and dimmed: this row is marked for something
    // destructive that has not happened yet.
    property bool markedForRemoval: false
    // An extra line at the top of the detail, for whatever the page
    // needs to say about this particular row.
    property string detailNote: ""

    property alias actionItems: actionRow.data

    readonly property bool hovered: rowMouse.containsMouse

    signal selectionToggled(bool selected)
    signal expandToggled()

    component DetailLabel: Label {
        color: Theme.textMuted
        font.pointSize: Theme.fontTiny
        Layout.alignment: Qt.AlignTop | Qt.AlignLeft
    }
    component DetailValue: Label {
        color: Theme.text
        font.pointSize: Theme.fontSmall
        wrapMode: Text.WordWrap
        Layout.fillWidth: true
    }

    width: ListView.view ? ListView.view.width : implicitWidth
    implicitHeight: layout.implicitHeight + 2 * Theme.tightSpacing
    color: rowMouse.containsMouse ? Theme.rowHover
         : (delegate.index % 2 === 0 ? Theme.rowEven : Theme.rowOdd)
    radius: 4
    opacity: delegate.markedForRemoval ? 0.55 : 1.0

    MouseArea {
        id: rowMouse
        anchors.fill: parent
        hoverEnabled: true
        // Clicks that land on a control inside the row belong to that
        // control; this only catches the gaps between them.
        onClicked: delegate.expandToggled()
    }

    ColumnLayout {
        id: layout
        anchors.fill: parent
        anchors.margins: Theme.tightSpacing
        // The tick box's left edge is the row's left edge, and the list
        // is already on the page's left line.
        anchors.leftMargin: 0
        // The right-hand end had nothing between the last button and the
        // scrollbar, so every row's action sat hard against it.
        anchors.rightMargin: Theme.rowSpacing
        spacing: Theme.tightSpacing

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.rowSpacing

            CheckBox {
                id: selectBox
                objectName: "selectCheckBox"
                visible: delegate.selectable
                checked: delegate.selected
                // The selection is the model's to own: the page writes
                // it back and the delegate re-reads it, so a recycled
                // delegate cannot carry one row's tick to the next.
                onToggled: delegate.selectionToggled(checked)
                ToolTip.visible: hovered
                ToolTip.delay: 400
                ToolTip.text: delegate.selectTooltip
            }

            Rectangle {
                id: artworkTile
                Layout.preferredWidth: Theme.iconSizeNormal
                Layout.preferredHeight: Theme.iconSizeNormal
                radius: 3
                color: Theme.groupBackground
                Image {
                    anchors.fill: parent
                    source: delegate.artworkUrl
                    visible: delegate.artworkUrl.length > 0
                    fillMode: Image.PreserveAspectCrop
                    asynchronous: true
                    cache: true
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                spacing: 0
                Label {
                    objectName: "rowTitle"
                    Layout.fillWidth: true
                    text: delegate.title.length > 0 ? delegate.title : delegate.filename
                    color: Theme.text
                    // Explicit, and the line under it is the reason.
                    // Theme.fontSmall has a floor of the system's
                    // smallest readable size, so on a machine whose
                    // default QML font is smaller than that floor an
                    // unsized title came out SMALLER than the artist
                    // beneath it -- the row's own heading set in the
                    // smallest type on it.
                    font.pointSize: Theme.fontNormal
                    elide: Text.ElideRight
                    font.strikeout: delegate.markedForRemoval
                }
                Label {
                    Layout.fillWidth: true
                    text: delegate.artist.length > 0 ? delegate.artist : delegate.relativePath
                    color: Theme.textMuted
                    font.pointSize: Theme.fontSmall
                    elide: Text.ElideRight
                    font.strikeout: delegate.markedForRemoval
                }
            }

            Label {
                visible: delegate.durationText.length > 0
                text: delegate.durationText
                color: Theme.textMuted
                font.pointSize: Theme.fontSmall
            }

            StarRating {
                objectName: "rowRating"
                visible: delegate.rating >= 0
                value: Math.max(0, delegate.rating)
                editable: false
                // StarRating only tracks hover while it is editable, and
                // this one is not, so the hover comes from a handler of
                // its own.
                ToolTip.visible: ratingHover.hovered
                ToolTip.delay: 400
                ToolTip.text: delegate.rating === 0
                    ? "Rated zero stars"
                    : "Rated " + delegate.rating + (delegate.rating === 1 ? " star" : " stars")
                HoverHandler { id: ratingHover }
            }

            StatusBadge {
                objectName: "cueBadge"
                visible: delegate.cueCount > 0
                label: delegate.cueBadgeLabel
                badgeColor: delegate.cueBadgeColor
                // Where the cue list lives now. It used to be visible
                // only in the expanded half, which meant opening a row
                // to answer "which cues?" about the number already on
                // screen.
                tooltipText: delegate.cueTooltip
            }

            Label {
                objectName: "commentMarker"
                visible: delegate.comment.length > 0
                text: "comment"
                color: Theme.textMuted
                font.pointSize: Theme.fontSmall
                // A Label is a Text and has no `hovered`, so the hover
                // comes from a handler.
                ToolTip.visible: commentHover.hovered
                ToolTip.delay: 400
                ToolTip.text: delegate.comment
                HoverHandler { id: commentHover }
            }

            RowLayout {
                id: actionRow
                spacing: Theme.tightSpacing
            }
        }

        // ---- the expanded half -------------------------------------
        //
        // A two-column grid rather than a stack of prefixed sentences.
        // "Comment: ..." and "Playlists: ..." read as prose when what
        // they are is a short list of labelled facts, and the labels
        // could not line up because each one was baked into its string.
        GridLayout {
            objectName: "expandedDetail"
            Layout.fillWidth: true
            // Lines the detail up under the title rather than under the
            // tick box, measured off the controls themselves so it stays
            // right at any font size.
            Layout.leftMargin: (selectBox.visible ? selectBox.width + Theme.rowSpacing : 0)
                + artworkTile.width + Theme.rowSpacing
            Layout.bottomMargin: Theme.tightSpacing
            visible: delegate.expanded
            columns: 2
            columnSpacing: Theme.rowSpacing
            rowSpacing: 2

            DetailLabel { visible: delegate.detailNote.length > 0; text: "" }
            DetailValue {
                visible: delegate.detailNote.length > 0
                text: delegate.detailNote
                color: Theme.textMuted
            }

            DetailLabel { visible: delegate.relativePath.length > 0; text: "File" }
            DetailValue {
                visible: delegate.relativePath.length > 0
                text: delegate.relativePath
                color: Theme.textMuted
                elide: Text.ElideMiddle
                wrapMode: Text.NoWrap
            }

            DetailLabel { visible: delegate.comment.length > 0; text: "Comment" }
            DetailValue { visible: delegate.comment.length > 0; text: delegate.comment }

            DetailLabel { visible: delegate.cueTooltip.length > 0; text: "Cues" }
            DetailValue {
                visible: delegate.cueTooltip.length > 0
                text: delegate.cueTooltip
                color: Theme.textMuted
            }

            DetailLabel { visible: delegate.playlistNames.length > 0; text: "Playlists" }
            DetailValue {
                visible: delegate.playlistNames.length > 0
                text: delegate.playlistNames
                color: Theme.textMuted
            }

            // Last, because it is the least of them: where a copy was
            // last seen matters once you have already decided this is
            // the track you were looking for.
            DetailLabel { visible: delegate.storedFrom.length > 0; text: "Backed up from" }
            DetailValue {
                visible: delegate.storedFrom.length > 0
                text: delegate.storedFrom + (delegate.storedAt.length > 0 ? " on " + delegate.storedAt : "")
                color: Theme.textMuted
            }
        }
    }
}
