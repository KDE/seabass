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
    // A model whose entries carry a `thumbnail` (a playlist's small cover,
    // as ScanController.playlistThumbnails gives it) shows it on every
    // row, left of the name; one without shows none.
    readonly property bool showsThumbnails: {
        const entries = root.model;
        if (!entries || entries.length === undefined) {
            return false;
        }
        for (let i = 0; i < entries.length; ++i) {
            if (entries[i] && entries[i].thumbnail !== undefined) {
                return true;
            }
        }
        return false;
    }

    // Typing while the popup is open (Sebastian's long lists): keys that
    // follow each other within typeAheadInterval build a filter, the list
    // narrowed to the names holding the typed text (case folded) with the
    // first of them current; Backspace takes a letter off, Escape clears
    // the filter (a second Escape closes), Enter picks the current row.
    // After a pause the typing starts over, and a letter typed alone
    // jumps to the next name starting with it, as type-ahead does,
    // without narrowing anything. The filter goes when the popup closes.
    property int typeAheadInterval: 600
    // What has been typed, while it filters the list; "" when it does not.
    readonly property string typedFilter: typeAhead.filtering ? typeAhead.typed : ""
    // The rows the popup lists, as indices into the model, in its order.
    readonly property var shownRows: {
        const rows = [];
        const wanted = root.typedFilter.toLocaleLowerCase();
        for (let i = 0; i < root.count; ++i) {
            if (wanted.length === 0 || root.textAt(i).toLocaleLowerCase().includes(wanted)) {
                rows.push(i);
            }
        }
        return rows;
    }

    QtObject {
        id: typeAhead
        property string typed: ""
        // The typed text narrows the list: from its second letter on, and
        // still after Backspace takes it back to one.
        property bool filtering: false
        // The row current in the popup, as a place in shownRows.
        property int current: -1
        // A key moved the current row since the popup opened: it is shaded.
        property bool keyed: false

        function reset() {
            keyed = false;
            typed = "";
            filtering = false;
            pause.stop();
        }
        function key(text) {
            keyed = true;
            const fresh = !pause.running;
            pause.restart();
            if (fresh) {
                typed = text;
                filtering = false;
                jumpToInitial(text);
            } else {
                typed += text;
                filtering = true;
                current = root.shownRows.length > 0 ? 0 : -1;
            }
        }
        // The next name after the current row starting with `letter`,
        // round the list.
        function jumpToInitial(letter) {
            const wanted = letter.toLocaleLowerCase();
            const n = root.shownRows.length;
            for (let step = 1; step <= n; ++step) {
                const at = (current + step + n) % n;
                if (root.textAt(root.shownRows[at]).toLocaleLowerCase().startsWith(wanted)) {
                    current = at;
                    return;
                }
            }
        }
        function backspace() {
            if (typed.length === 0) {
                return;
            }
            pause.restart();
            typed = typed.slice(0, -1);
            if (typed.length === 0) {
                filtering = false;
            }
            current = root.shownRows.length > 0 ? 0 : -1;
        }
    }
    Timer {
        id: pause
        interval: root.typeAheadInterval
    }

    // The combo's field says what is typed while it filters the list,
    // above the popup, where the name picked shows otherwise.
    displayText: root.typedFilter.length > 0 ? qsTr("Filter: %1").arg(root.typedFilter) : root.currentText

    function pickShown(place) {
        if (place < 0 || place >= root.shownRows.length) {
            return;
        }
        const index = root.shownRows[place];
        root.playlistPicked(index, root.model[index]);
        root.popup.close();
    }

    // Before the ComboBox's own handling, which would search by initial
    // and move its current index under the popup.
    Keys.priority: Keys.BeforeItem
    Keys.onPressed: event => {
        if (!root.popup.visible) {
            return;
        }
        if (event.key === Qt.Key_Escape) {
            if (typeAhead.typed.length > 0) {
                typeAhead.reset();
                typeAhead.current = Math.max(0, root.shownRows.indexOf(root.currentIndex));
                event.accepted = true;
            }
            return;  // a second Escape closes, as before
        }
        if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
            root.pickShown(typeAhead.current);
            event.accepted = true;
        } else if (event.key === Qt.Key_Backspace) {
            typeAhead.backspace();
            event.accepted = true;
        } else if (event.key === Qt.Key_Down || event.key === Qt.Key_Up) {
            typeAhead.keyed = true;
            const step = event.key === Qt.Key_Down ? 1 : -1;
            typeAhead.current = Math.max(0, Math.min(root.shownRows.length - 1, typeAhead.current + step));
            event.accepted = true;
        } else if (event.text.length > 0 && event.text.charCodeAt(0) >= 32
                   && !(event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier))) {
            typeAhead.key(event.text);
            event.accepted = true;
        }
    }

    // The popup's rows, its own: they list shownRows, indices into the
    // model, so the ComboBox's delegateModel (one row per model entry) is
    // not what they come from.
    Component {
        id: rowComponent
    Rectangle {
        id: rowRoot
        // The row's place in the popup's list, and the model entry it
        // shows (shownRows[place]).
        required property int index
        required property var modelData
        readonly property int sourceIndex: rowRoot.modelData
        readonly property var entry: root.model[rowRoot.sourceIndex]
        // Where the keys have put the current row since the popup opened.
        readonly property bool keyCurrent: typeAhead.keyed && rowRoot.index === typeAhead.current

        // ListView.view first, then the item it was parented into, then
        // its own implicit width. The attached property is not always
        // there: these rows are built through a DelegateModel and handed
        // to a ListView this file owns, and under org.kde.desktop the
        // attachment came back null, which left every row 0 wide. A row
        // with no width still paints -- it has height and a background --
        // and simply cannot be clicked, which is how it was mistaken for
        // a popup in a window of its own (seabass#18).
        width: ListView.view ? ListView.view.width
                             : (parent ? parent.width : implicitWidth)
        height: 32

        color: rowMouseArea.pressed ? Theme.rowPressed
            : rowHover.hovered || rowRoot.keyCurrent ? Theme.rowHover
            : (rowRoot.index % 2 === 0 ? Theme.rowEven : Theme.rowOdd)
        border.color: rowRoot.sourceIndex === root.currentIndex ? Theme.accent : "transparent"
        border.width: rowRoot.sourceIndex === root.currentIndex ? 2 : 0
        radius: rowRoot.sourceIndex === root.currentIndex ? 4 : 0

        // Not rowMouseArea's own hover: under org.kde.desktop a Label
        // carries a HoverHandler of its own, and the MouseArea under the
        // row's name never hears the pointer arrive.
        HoverHandler { id: rowHover }

        MouseArea {
            id: rowMouseArea
            anchors.fill: parent
            hoverEnabled: true
            onClicked: root.pickShown(rowRoot.index)
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 8
            anchors.rightMargin: 8
            spacing: 6
            PlaylistThumbnail {
                visible: root.showsThumbnails
                Layout.preferredWidth: implicitWidth
                Layout.preferredHeight: implicitHeight
                Layout.alignment: Qt.AlignVCenter
                source: rowRoot.entry && rowRoot.entry.thumbnail ? rowRoot.entry.thumbnail : ""
            }
            Label {
                objectName: "pickerRowName"
                text: root.textAt(rowRoot.sourceIndex)
                // Theme's, like the count beside it: the style's palette
                // text is a light theme's on a Mac, dark on this popup.
                color: Theme.text
                Layout.fillWidth: true
                elide: Text.ElideRight
            }
            Label {
                text: rowRoot.entry && rowRoot.entry.count !== undefined ? rowRoot.entry.count : ""
                color: Qt.darker(Theme.textMuted, 1.3)
                font.pointSize: Theme.fontSmall
                horizontalAlignment: Text.AlignRight
            }
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
        id: popupRoot
        y: root.height
        width: root.width
        // + topPadding/bottomPadding: without them the popup's own
        // bounding box comes out `padding`-times-2 short of what the
        // ListView actually needs, so the last row's real geometry
        // (inside a ListView positioned at the padding-inset content
        // area) ends up partly below the popup's OWN outer bounds --
        // and a click there reads as "outside the popup" (closing it)
        // rather than reaching the row's MouseArea. Confirmed directly:
        // clicking the last row of a 3-row list closed the popup
        // instead of picking it, every time, before this.
        implicitHeight: Math.min(contentItem.implicitHeight, 320) + topPadding + bottomPadding
        padding: 1
        // Each opening starts on the playlist picked, unfiltered: the
        // filter went when the popup last closed.
        onAboutToShow: typeAhead.current = root.shownRows.indexOf(root.currentIndex)
        onClosed: typeAhead.reset()

        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            // Width named rather than inherited. A Popup's contentItem is
            // given its width by the style, and the style Linux actually
            // ships does not give it one: under org.kde.desktop this
            // ListView came out 0 wide, so every row delegate bound to
            // ListView.view.width was 0 wide too. The rows were visible --
            // they have height and they paint -- and clicking one did
            // nothing at all, because there was nothing under the pointer
            // to click. That is the whole of what seabass#18 recorded as
            // "the popup must be a separate window": it is not, the rows
            // simply had no width.
            width: popupRoot.availableWidth
            // The rows shown (every one, or the typed filter's), built
            // from rowComponent; nothing while the popup is shut.
            model: root.popup.visible ? root.shownRows : null
            delegate: rowComponent
            currentIndex: typeAhead.current
            onCurrentIndexChanged: {
                if (currentIndex >= 0) {
                    positionViewAtIndex(currentIndex, ListView.Contain);
                }
            }
            ScrollBar.vertical: BigScrollBar {}
        }
        background: Rectangle {
            color: Theme.surface
            border.color: Theme.border
            radius: Theme.cornerRadius
        }
    }
}
