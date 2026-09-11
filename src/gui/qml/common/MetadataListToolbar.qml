import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// The row above a metadata list: select all, select none, a search box,
// and whatever the page wants to say about the counts.
//
// Shared for the same reason the delegate is. Both pages grew a list
// long enough to need filtering and a selection, and a "select all" that
// sits somewhere different on each page is a worse answer than one that
// does not.
//
// The two buttons are on the left, ahead of the field, because they act
// on the list the field filters: read left to right it is "all of these,
// none of these, or the ones matching this".
RowLayout {
    id: root

    property alias searchText: field.text
    property string placeholder: "Search title, artist or filename"
    // What the count label on the right says. A page's own sentence; it
    // knows what it is counting.
    property string summary: ""
    // Off while a list is empty or a run is in progress: a select-all
    // that selects nothing teaches that the button does nothing.
    property bool selectionEnabled: true
    // Selecting everything means everything that has been loaded, and a
    // paged list may not have loaded all of it yet. Pages that page say
    // so in the tooltip rather than quietly meaning something narrower
    // than the words.
    property string selectAllTooltip: "Select every track in the list"

    signal selectAllRequested()
    signal selectNoneRequested()
    signal searchChanged(string text)

    spacing: Theme.rowSpacing

    Button {
        objectName: "selectAllButton"
        text: "Select All"
        enabled: root.selectionEnabled
        onClicked: root.selectAllRequested()
        ToolTip.visible: hovered
        ToolTip.delay: 400
        ToolTip.text: root.selectAllTooltip
    }
    Button {
        objectName: "selectNoneButton"
        text: "Select None"
        enabled: root.selectionEnabled
        onClicked: root.selectNoneRequested()
        ToolTip.visible: hovered
        ToolTip.delay: 400
        ToolTip.text: "Clear the selection"
    }

    TextField {
        id: field
        objectName: "searchField"
        Layout.fillWidth: true
        Layout.minimumWidth: 110
        placeholderText: root.placeholder
        // Room for the clear button to sit inside the field rather than
        // beside it, where it would read as a third button in the row.
        rightPadding: clearButton.visible ? clearButton.width + 8 : undefined
        onTextChanged: root.searchChanged(text)

        ToolButton {
            id: clearButton
            objectName: "clearSearchButton"
            anchors.right: parent.right
            anchors.rightMargin: 4
            anchors.verticalCenter: parent.verticalCenter
            visible: field.text.length > 0
            width: Theme.iconSizeSmall
            height: Theme.iconSizeSmall
            text: "✕"
            font.pointSize: Theme.fontSmall
            onClicked: {
                field.clear();
                // Back to the field, not to whatever the click left
                // focused: clearing a search is almost always the start
                // of typing a different one.
                field.forceActiveFocus();
            }
            ToolTip.visible: hovered
            ToolTip.delay: 400
            ToolTip.text: "Clear the search"
        }
    }

    Label {
        objectName: "listSummary"
        Layout.maximumWidth: 320
        text: root.summary
        color: Theme.textMuted
        font.pointSize: Theme.fontSmall
        elide: Text.ElideRight
    }
}
