// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui
import "HomeModel.js" as HomeModel

// The home screen's rail: which stick, and which kind of tool. Two
// sections, "Sticks" (one line per row of the stick model) and "Tools"
// (Explore, Sync, Backup, Maintain), down the left of the page, or, in
// a narrow window (compact), wrapped rows of chips above the pane.
//
// It shows the selection and reports clicks; it does not keep the
// selection itself. The page owns selectedStickKey and selectedGroup and
// sets them in answer to stickActivated and groupActivated, so the rules
// for what gets selected (on load, on arrival, on removal, remembered
// across runs) live in one place, and nothing here breaks a binding the
// page made.
//
// Every entry's text starts one pill padding in from the rail's left
// edge, the section labels' included, so the rail has one text line of
// its own; the pills themselves sit on the page's left line.
FocusScope {
    id: root
    objectName: "homeRail"

    // The stick model: MediaController's DetectedStickListModel, or a
    // plain array of stick objects in the tests. Rows carry label,
    // mountPoint, devicePath, isSdCard and isFolder at least.
    required property var sticks
    // mountPoint when non-empty, else devicePath: the same key the
    // page's stick rows are named by (stickRow:<key>).
    property string selectedStickKey: ""
    // "explore", "sync", "backup" or "maintain".
    property string selectedGroup: "explore"
    // A narrow window: wrapped rows of chips rather than a column.
    property bool compact: false

    signal stickActivated(string key)
    signal groupActivated(string group)

    // The tool groups, shared with the page (HomeModel.js).
    readonly property var groups: HomeModel.groups()

    implicitWidth: Theme.scaled(200)
    implicitHeight: flow.implicitHeight

    // ---- the model, whichever shape it comes in ---------------------
    //
    // The Repeater below copes with an array and a model on its own;
    // HomeModel reads rows directly for what needs them, the same way
    // the page does.
    function hasKey(key) {
        const count = HomeModel.rowCount(root.sticks);
        for (let i = 0; i < count; ++i) {
            if (HomeModel.keyOf(HomeModel.rowAt(root.sticks, i)) === key) {
                return true;
            }
        }
        return false;
    }
    function groupIndex(key) {
        for (let i = 0; i < root.groups.length; ++i) {
            if (root.groups[i].key === key) {
                return i;
            }
        }
        return -1;
    }
    function stickIndex(key) {
        for (let i = 0; i < stickRepeater.count; ++i) {
            const item = stickRepeater.itemAt(i);
            if (item && item.key === key) {
                return i;
            }
        }
        return -1;
    }

    // ---- arrivals ---------------------------------------------------
    //
    // A stick arriving is animated in, the way the stick list did it: a
    // fade and a small overshooting grow. "Arriving" means a key this
    // rail has not shown before, not a delegate being created: the rows
    // present when the rail is built do not bounce, and a model that
    // replaces itself wholesale (a plain array reassigned) rebuilds its
    // delegates without every stick looking new.
    //
    // A stick goes by two keys over its life, its device path until it
    // is mounted and its mount point after, and each row remembers every
    // key it has had (ownKeys), so a stick that leaves is forgotten under
    // all of them: plugged in again, under either, it arrives again.
    property bool ready: false
    property var seenKeys: ({})
    function noteKey(key, entry) {
        const isNew = root.ready && key.length > 0 && root.seenKeys[key] !== true;
        root.seenKeys[key] = true;
        if (key.length > 0 && entry.ownKeys.indexOf(key) < 0) {
            entry.ownKeys.push(key);
        }
        return isNew;
    }
    Component.onCompleted: root.ready = true

    // The other rows slide to make room, or to close the gap, only while
    // a stick is coming or going. Any other relayout (the window resized,
    // the compact form switched on) is the rail being redrawn, not
    // something moving, and animating it would only make it lag.
    property bool sticksMoving: false
    function noteMovement() {
        root.sticksMoving = true;
        settle.restart();
    }
    Timer {
        id: settle
        interval: Theme.arrivalTransitionDuration
        onTriggered: root.sticksMoving = false
    }

    Flow {
        id: flow
        width: root.width
        spacing: root.compact ? Theme.tightSpacing : 0

        // The rows below the one that went (or above the one that came)
        // slide into place rather than jump, softer than the arrival
        // itself so the eye stays on the stick.
        move: root.sticksMoving ? slide : null
        Transition {
            id: slide
            NumberAnimation {
                properties: "x,y"
                duration: Theme.arrivalTransitionDuration
                easing.type: Easing.OutQuint
            }
        }

        TableHeaderLabel {
            objectName: "railSticksLabel"
            label: "Sticks"
            width: flow.width
            leftPadding: Theme.crumbTextInset
            bottomPadding: Theme.tightSpacing
        }

        Repeater {
            id: stickRepeater
            model: root.sticks !== null && root.sticks !== undefined ? root.sticks : []

            delegate: RailEntry {
                id: stickEntry
                required property int index
                required property string label
                required property string mountPoint
                required property string devicePath
                required property bool isSdCard
                required property bool isFolder
                readonly property string key: mountPoint.length > 0 ? mountPoint : devicePath
                // Every key this row has gone by (see noteKey).
                property var ownKeys: []

                objectName: "railStick:" + key
                isStick: true
                compact: root.compact
                columnWidth: flow.width
                text: label
                stickIsSdCard: isSdCard
                stickIsFolder: isFolder
                fontSize: Theme.fontSmall
                verticalPadding: Theme.tightSpacing
                selected: key === root.selectedStickKey
                hasCursor: stickKeys.activeFocus && stickKeys.cursor === index
                onActivated: root.stickActivated(stickEntry.key)

                Component.onCompleted: {
                    if (root.noteKey(key, stickEntry)) {
                        // Set before the first frame, not on the
                        // animation's first tick: otherwise the row is
                        // drawn once at full size before it arrives.
                        stickEntry.opacity = 0;
                        stickEntry.scale = 0.85;
                        root.noteMovement();
                        arrival.start();
                    }
                    stickEntry.created = true;
                }
                // Mounting turns the device key into the mount point:
                // the same stick, not a new one. Only once created: the
                // key's first value arrives as a change too, and noting
                // it then would make every new stick look seen already.
                property bool created: false
                onKeyChanged: {
                    if (stickEntry.created) {
                        root.noteKey(key, stickEntry);
                    }
                }

                ParallelAnimation {
                    id: arrival
                    NumberAnimation {
                        target: stickEntry; property: "opacity"; from: 0; to: 1
                        duration: Theme.arrivalTransitionDuration; easing.type: Easing.OutCubic
                    }
                    NumberAnimation {
                        target: stickEntry; property: "scale"; from: 0.85; to: 1
                        duration: Theme.arrivalTransitionDuration
                        easing.type: Easing.OutBack; easing.overshoot: 1.8
                    }
                }
            }

            // A stick leaving shrinks away rather than vanishing: a stand-in
            // with its look takes its place and animates out, since the
            // Repeater destroys the real one straight away.
            onItemRemoved: (index, item) => {
                if (!root.ready || root.hasKey(item.key)) {
                    return;
                }
                for (let i = 0; i < item.ownKeys.length; ++i) {
                    delete root.seenKeys[item.ownKeys[i]];
                }
                root.noteMovement();
                ghostComponent.createObject(ghostLayer, {
                    objectName: "railStickLeaving:" + item.key,
                    x: item.x, y: item.y, width: item.width, height: item.height,
                    text: item.text, stickIsSdCard: item.stickIsSdCard, stickIsFolder: item.stickIsFolder,
                    selected: item.selected,
                });
            }
        }

        Label {
            objectName: "railNoSticks"
            visible: stickRepeater.count === 0
            // Its own width in the compact form (undefined resets it to
            // the text's); binding it to implicitWidth instead loops, as
            // an elided Text re-measures itself when its width is set.
            width: root.compact ? undefined : flow.width
            text: "No USB sticks detected"
            color: Theme.textMuted
            font.pointSize: Theme.fontSmall
            leftPadding: Theme.crumbTextInset
            rightPadding: Theme.crumbTextInset
            topPadding: Theme.tightSpacing
            bottomPadding: Theme.tightSpacing
            elide: Text.ElideRight
        }

        TableHeaderLabel {
            objectName: "railToolsLabel"
            label: "Tools"
            width: flow.width
            leftPadding: Theme.crumbTextInset
            topPadding: Theme.sectionSpacing
            bottomPadding: Theme.tightSpacing
        }

        Repeater {
            id: groupRepeater
            model: root.groups

            delegate: RailEntry {
                id: groupEntry
                required property int index
                required property var modelData
                objectName: "railGroup:" + modelData.key
                compact: root.compact
                columnWidth: flow.width
                text: modelData.name
                iconName: modelData.icon
                fontSize: Theme.fontNormal
                verticalPadding: Theme.crumbTextInset
                selected: modelData.key === root.selectedGroup
                hasCursor: toolKeys.activeFocus && toolKeys.cursor === index
                onActivated: root.groupActivated(groupEntry.modelData.key)
            }
        }
    }

    // Above the flow, at its origin, so a stand-in lands exactly where the
    // row it replaces was drawn.
    Item {
        id: ghostLayer
        x: flow.x
        y: flow.y
        width: flow.width
        height: flow.height
    }

    Component {
        id: ghostComponent
        RailEntry {
            id: ghost
            isStick: true
            interactive: false
            compact: root.compact
            fontSize: Theme.fontSmall
            verticalPadding: Theme.tightSpacing
            ParallelAnimation {
                running: true
                NumberAnimation {
                    target: ghost; property: "opacity"; to: 0
                    duration: Theme.departureTransitionDuration; easing.type: Easing.InCubic
                }
                NumberAnimation {
                    target: ghost; property: "scale"; to: 0.82
                    duration: Theme.departureTransitionDuration
                    easing.type: Easing.InBack; easing.overshoot: 1.4
                }
                onFinished: ghost.destroy()
            }
        }
    }

    // ---- keyboard ---------------------------------------------------
    //
    // Each section is one tab stop: Tab reaches the sticks, then the
    // tools, then whatever follows the rail (the cards). Up and Down
    // move a cursor within the focused section, and Enter or Space
    // activates the entry under it, exactly as a click would. In the
    // compact form, where the entries run across, Left and Right do the
    // same. The cursor starts on the selected entry.
    component SectionKeys: Item {
        id: keys
        property int cursor: 0
        property int count: 0
        // Entries run across (the compact form): Left and Right move too.
        property bool across: false
        signal activate(int index)
        signal focusEntered()
        onActiveFocusChanged: {
            if (activeFocus) {
                keys.focusEntered();
            }
        }
        onCountChanged: keys.cursor = Math.max(0, Math.min(keys.cursor, keys.count - 1))
        Keys.onPressed: (event) => {
            const back = event.key === Qt.Key_Up || (keys.across && event.key === Qt.Key_Left);
            const forward = event.key === Qt.Key_Down || (keys.across && event.key === Qt.Key_Right);
            if (back || forward) {
                keys.cursor = Math.max(0, Math.min(keys.count - 1, keys.cursor + (forward ? 1 : -1)));
                event.accepted = true;
            } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter
                       || event.key === Qt.Key_Space) {
                if (keys.count > 0) {
                    keys.activate(keys.cursor);
                }
                event.accepted = true;
            }
        }
    }

    SectionKeys {
        id: stickKeys
        objectName: "railStickKeys"
        activeFocusOnTab: stickRepeater.count > 0
        across: root.compact
        count: stickRepeater.count
        onFocusEntered: cursor = Math.max(0, root.stickIndex(root.selectedStickKey))
        onActivate: (index) => {
            const item = stickRepeater.itemAt(index);
            if (item) {
                root.stickActivated(item.key);
            }
        }
    }

    SectionKeys {
        id: toolKeys
        objectName: "railToolKeys"
        activeFocusOnTab: true
        across: root.compact
        count: root.groups.length
        onFocusEntered: cursor = Math.max(0, root.groupIndex(root.selectedGroup))
        onActivate: (index) => root.groupActivated(root.groups[index].key)
    }

    // ---- one entry --------------------------------------------------
    //
    // A pill, full width in the column and as wide as its text in the
    // compact form. Selected: the group background, full-strength
    // DemiBold text; a tool also gets the accent bar at its left edge
    // and an accent icon. The rest are muted and shade on hover.
    component RailEntry: Item {
        id: entry
        property string text
        property bool isStick: false
        property bool stickIsSdCard: false
        property bool stickIsFolder: false
        property string iconName: ""
        property bool selected: false
        property bool hasCursor: false
        property bool interactive: true
        property real fontSize: Theme.fontNormal
        property real verticalPadding: Theme.tightSpacing
        // An inline component sees none of this file's ids, so what it
        // needs from the rail is handed in.
        property bool compact: false
        property real columnWidth: 0
        signal activated()

        width: entry.compact ? implicitWidth : entry.columnWidth
        implicitWidth: content.implicitWidth + 2 * Theme.crumbTextInset
        implicitHeight: content.implicitHeight + 2 * entry.verticalPadding

        Accessible.role: Accessible.Button
        Accessible.name: entry.text
        Accessible.onPressAction: entry.activated()

        Rectangle {
            objectName: "railPill"
            anchors.fill: parent
            radius: 4
            color: entry.selected ? Theme.groupBackground
                : pointer.pressed ? Theme.rowPressed
                : pointer.containsMouse ? Theme.rowHover
                : "transparent"
            // Unselected chips need an edge to read as chips; in the
            // column the row itself is the shape.
            border.width: entry.compact && !entry.selected ? 1 : 0
            border.color: Theme.border
        }

        Rectangle {
            objectName: "railAccentBar"
            visible: entry.selected && !entry.isStick
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: 3
            color: Theme.accent
        }

        RowLayout {
            id: content
            x: Theme.crumbTextInset
            width: entry.width - 2 * Theme.crumbTextInset
            anchors.verticalCenter: parent.verticalCenter
            spacing: entry.isStick ? Theme.tightSpacing : Theme.rowSpacing

            UsbStickIcon {
                objectName: "railIcon"
                visible: entry.isStick
                isSdCard: entry.stickIsSdCard
                isFolder: entry.stickIsFolder
                size: Math.round(name.fontInfo.pixelSize * 1.4)
                color: name.color
                Layout.alignment: Qt.AlignVCenter
            }
            SeabassIcon {
                objectName: "railIcon"
                visible: !entry.isStick
                iconName: entry.iconName
                size: Math.round(name.fontInfo.pixelSize * 1.4)
                color: entry.selected ? Theme.accent : Theme.textMuted
                Layout.alignment: Qt.AlignVCenter
            }
            Label {
                id: name
                objectName: "railText"
                text: entry.text
                color: entry.selected ? Theme.text : Theme.textMuted
                font.pointSize: entry.fontSize
                font.weight: entry.selected ? Font.DemiBold : Font.Normal
                elide: Text.ElideRight
                // Whole pixels with one to spare, and no floor at the
                // natural width: a long label elides in the column, and a
                // short one is not abbreviated for a fraction of a pixel
                // (see the stick path label on StickListPage).
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                Layout.preferredWidth: Math.ceil(implicitWidth) + 1
                Layout.alignment: Qt.AlignVCenter
            }
        }

        // Where the keyboard cursor is, while its section has focus.
        Rectangle {
            objectName: "railCursor"
            anchors.fill: parent
            visible: entry.hasCursor
            radius: 4
            color: "transparent"
            border.width: 2
            border.color: Theme.accent
        }

        MouseArea {
            id: pointer
            anchors.fill: parent
            enabled: entry.interactive
            hoverEnabled: entry.interactive
            cursorShape: Qt.PointingHandCursor
            onClicked: entry.activated()
        }
    }
}
