// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// The buttons at the top right of the window: the home's menu of
// backups and folders on this computer, then About, Preferences and
// Support. One row, owned by the window and laid over the page stack
// rather than by each page: as part of a page they slid in and out with
// the page's transition, and Sebastian wanted them still (2026-09-28).
//
// A page that wants the row under its header says so with two
// properties: `appHeaderPlace` ("home", "about", "preferences" or
// "support"; the button for that place is off, and the menu shows on
// the home alone) and `appHeaderReserve`, which this row sets to its own
// width so the page's header can leave that much room at its right. A
// page without them (a stick's tool pages) gets no row.
Item {
    id: root
    // An Item, not a StackView: Main.qml imports the Material style, so
    // its StackView is Material's type and this file's is the running
    // style's, and a StackView-typed property refused the window's own
    // ("Unable to assign"), leaving the row with no stack at all.
    required property Item stackView
    readonly property var page: root.stackView ? root.stackView.currentItem : null
    readonly property string place: root.page && root.page.appHeaderPlace !== undefined ? root.page.appHeaderPlace : ""
    property int iconSize: Math.round(Theme.scaled(22))

    signal browseFullBackupRequested()
    signal openFolderRequested()
    signal aboutRequested()
    signal preferencesRequested()
    signal supportRequested()

    visible: root.place.length > 0
    implicitWidth: row.implicitWidth
    implicitHeight: row.implicitHeight

    // A page that guards leaving (Preferences asks about changed
    // settings) exposes leaveTo(fn); the others just go.
    function leave(fn) {
        if (root.page && typeof root.page.leaveTo === "function") {
            root.page.leaveTo(fn);
        } else {
            fn();
        }
    }

    // The page keeps that much of its header clear.
    Binding {
        target: root.page
        property: "appHeaderReserve"
        value: root.visible ? row.width : 0
        when: root.page !== null && root.page.appHeaderReserve !== undefined
        restoreMode: Binding.RestoreBindingOrValue
    }

    RowLayout {
        id: row
        anchors.fill: parent
        spacing: 0

        // What opens a library from this computer rather than from a
        // stick, behind one button: each is used now and then, and as
        // header buttons plus a row under the list they crowded a page
        // whose subject is the sticks. Home only.
        IconToolButton {
            id: homeMenuButton
            objectName: "homeMenuButton"
            visible: root.place === "home"
            iconName: "application-menu"
            iconSize: root.iconSize
            text: "Backups and folders"
            ToolTip.visible: hovered && !homeMenu.visible
            ToolTip.text: "Backups and folders on this computer"
            onClicked: homeMenu.visible ? homeMenu.close() : homeMenu.open()

            Menu {
                id: homeMenu
                objectName: "homeMenu"
                // A press on the button itself does not close the menu,
                // so its click can: otherwise the press closed it and the
                // click opened it again, under every style but KDE's.
                closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
                // Opens down and to the left, so it stays in the window
                // from a button near the right edge.
                x: homeMenuButton.width - width
                y: homeMenuButton.height
                // As wide as its longest entry. A Menu keeps the style's
                // own width, 200 px, whatever its items hold, and elided
                // "Browse a Full Stick Backup…" with the window half
                // empty. Rounded up for the reason BackBreadcrumb's crumbs
                // are: a fraction short of the text elides it.
                width: {
                    let widest = 0;
                    for (let i = 0; i < count; ++i) {
                        const item = itemAt(i);
                        if (item) widest = Math.max(widest, item.implicitWidth);
                    }
                    return Math.max(implicitWidth, Math.ceil(widest) + 1 + leftPadding + rightPadding);
                }
                MenuItem {
                    objectName: "browseFullBackupItem"
                    text: "Browse a Full Stick Backup…"
                    icon.source: Theme.iconUrl("backup")
                    icon.color: enabled ? Theme.text : Theme.textMuted
                    onTriggered: root.browseFullBackupRequested()
                }
                MenuItem {
                    objectName: "openFolderItem"
                    text: "Open a Library From a Folder…"
                    icon.source: Theme.iconUrl("folder-open")
                    icon.color: enabled ? Theme.text : Theme.textMuted
                    onTriggered: root.openFolderRequested()
                }
            }
        }

        AppHeaderButtons {
            objectName: "appHeaderButtons"
            current: root.place === "home" ? "" : root.place
            iconSize: root.iconSize
            onAboutRequested: root.leave(() => root.aboutRequested())
            onPreferencesRequested: root.leave(() => root.preferencesRequested())
            onSupportRequested: root.leave(() => root.supportRequested())
        }
    }
}
