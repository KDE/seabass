// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// Device Profile: rekordbox's player/mixer preference files on the
// stick. Picking a value stages it (UNSAVED, with an Undo); the floating
// Save writes every staged field, each backed up first.
//
// Laid out by what a setting is about -- the player's DJ settings, its
// lights, the mixer's faders -- rather than by the three .DAT files the
// bytes happen to live in. Every setting says what it does behind its
// own (i): "Needle lock", "Slip flashing" and "Waveform divisions" are
// the hardware's words, and the only way to find out what they meant was
// to go and find the player's manual.
Page {
    id: root
    required property string stickLabel
    required property string pioneerRoot

    // The width of the name column, so every control on the page starts
    // on one line. Scaled like AppSettingsPage's settingIndent rather
    // than measured off the longest label: a German or Japanese label is
    // not what this column is sized for, and it elides instead of pushing
    // the controls out of line.
    readonly property real labelColumnWidth: Theme.scaled(150)
    // The width every control gets, switch or drop-down, so the (i)
    // after it lands on one line down the whole page. Sized to the
    // drop-down; a switch sits at the start of the same slot. Letting
    // each control take its own width put the (i) of every off/on
    // setting a couple of hundred pixels left of its neighbours'.
    readonly property real controlColumnWidth: Theme.scaled(200)

    SettingsController {
        id: settingsController
    }

    // Edit mode for this library: session, Save button, leave guard.
    EditSessionHost {
        id: editHost
        // Cancel on the low-space question leaves, as Back does -- see
        // EditSessionHost's backupLocationDeclined for why it must.
        onBackupLocationDeclined: editHost.requestLeave(() => root.StackView.view.pop())
        feature: "settings"
        anchors.fill: parent
        libraryId: root.registryLibraryId()
        stickLabel: root.stickLabel
        rekordboxPath: root.pioneerRoot
    }
    function registryLibraryId() {
        return typeof EditSessionRegistry !== "undefined" ? EditSessionRegistry.libraryIdForPath(root.pioneerRoot) : "";
    }

    Component.onCompleted: settingsController.load(root.pioneerRoot)

    header: ToolBar {
        // Every side zeroed so the header's inset is Theme.pageMargin
        // and nothing else. `padding` alone does not do it: styles set
        // horizontalPadding or leftPadding of their own on top of it,
        // 4px under Breeze and 6 under the default style, and that is
        // exactly how far right of the body the breadcrumb used to sit.
        leftPadding: 0
        rightPadding: 0
        topPadding: 0
        bottomPadding: Theme.headerBottomPadding
        // Opaque background override -- see AppSettingsPage.qml's header
        // for why (KDE's Breeze style bleeds the window behind Seabass
        // through an unstyled ToolBar).
        background: Rectangle { color: Theme.surface }

        RowLayout {
            anchors.fill: parent
            anchors.margins: Theme.pageMargin
            spacing: Theme.rowSpacing
            BackBreadcrumb {
                stack: root.StackView.view
                middleLabel: root.stickLabel
                title: "Device Profile"
                backEnabled: !settingsController.busy && !editHost.writing
                onHomeRequested: editHost.requestLeave(() => root.StackView.view.pop(null))
                onBackRequested: editHost.requestLeave(() => root.StackView.view.pop())
            }
            InfoButton {
                objectName: "pageInfoButton"
                explanationTitle: "Device Profile"
                summaryText: "The player and mixer preferences rekordbox keeps on this stick: tempo range, quantize, "
                    + "auto cue level, fader curves and the rest."
                explanationText: "A Pioneer player picks these up when you load My Settings from the stick, so a "
                    + "stick set up here behaves the same on every CDJ and DJM you plug it into.\n\n"
                    + "## Where they live\n\n"
                    + "Three files in the PIONEER folder: MYSETTING.DAT and MYSETTING2.DAT for the player, "
                    + "DJMMYSETTING.DAT for the mixer. A fourth, DEVSETTING.DAT, is on the stick too, but its "
                    + "layout is not known well enough to change safely, so it is not shown.\n\n"
                    + "## Nothing is written until you press Save\n\n"
                    + "Every change is staged first, and each file is backed up before it is written."
            }
            Item { Layout.fillWidth: true }
            BusyIndicator {
                visible: settingsController.busy
                running: settingsController.busy
                implicitWidth: Theme.iconSizeSmall
                implicitHeight: Theme.iconSizeSmall
            }
        }
    }

    PageScrollView {
        id: scroller
        objectName: "settingsScroll"
        anchors.fill: parent
        anchors.margins: Theme.pageMargin

        ColumnLayout {
            objectName: "settingsColumn"
            // The Flickable's content container already takes the page's
            // width minus the scrollbar gutter. PageScrollView is not a
            // ScrollView and has no availableWidth; binding to that left
            // the column at its implicit width, where no label could elide.
            //
            // Capped and centred the way Preferences is (AppSettingsPage's
            // settingsColumn, the same 640): on a wide window the page
            // used to put every control far left under the breadcrumb and
            // leave the rest of the window empty, one click away from a
            // Preferences page that stands in the middle. The cap is on
            // the column, not the scroll view, so the scroll bar stays on
            // the window's edge.
            readonly property int maxWidth: 640
            width: Math.min(parent.width, maxWidth)
            x: Math.round(Math.max(0, (parent.width - width) / 2))
            spacing: Theme.sectionSpacing

            Label {
                objectName: "errorLabel"
                visible: settingsController.errorMessage.length > 0
                text: settingsController.errorMessage
                color: Theme.danger
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
            Label {
                visible: settingsController.pendingCount > 0
                text: settingsController.pendingCount + " setting(s) changed and not saved yet. Nothing is written "
                    + "to the stick until you press Save; each file is backed up first."
                color: Theme.warnText
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }

            Repeater {
                model: settingsController.groups
                delegate: ColumnLayout {
                    id: group
                    objectName: "settingsGroup"
                    required property var modelData
                    Layout.fillWidth: true
                    spacing: Theme.tightSpacing

                    // A heading, not a box. A GroupBox drew a border and
                    // indented everything inside it by its own padding,
                    // which put the controls on a different left line in
                    // every group -- and its title was a file name.
                    Subtitle {
                        objectName: "groupHeading"
                        text: group.modelData.title
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                    }

                    Repeater {
                        model: group.modelData.fields
                        delegate: RowLayout {
                            id: fieldRow
                            objectName: "settingRow"
                            required property var modelData
                            readonly property string shownValue: modelData.unsaved ? modelData.pendingValue : modelData.value
                            readonly property bool editable: modelData.options.length > 0
                                && !settingsController.busy && !editHost.writing
                            Layout.fillWidth: true
                            spacing: Theme.rowSpacing

                            Label {
                                objectName: "settingLabel"
                                text: fieldRow.modelData.label
                                color: Theme.text
                                elide: Text.ElideRight
                                Layout.preferredWidth: root.labelColumnWidth
                                Layout.maximumWidth: root.labelColumnWidth
                            }

                            // Off/on settings get a switch, because that is
                            // the one question they ask. Everything else
                            // keeps its drop-down. Both sit in one slot of a
                            // fixed width and height, so a switch row is as
                            // tall as a drop-down row and every (i) lines up.
                            Item {
                                Layout.preferredWidth: root.controlColumnWidth
                                Layout.preferredHeight: valueCombo.implicitHeight

                            Switch {
                                id: valueSwitch
                                objectName: "settingSwitch"
                                anchors.left: parent.left
                                anchors.verticalCenter: parent.verticalCenter
                                visible: fieldRow.modelData.isSwitch === true
                                checked: fieldRow.shownValue === "on"
                                enabled: fieldRow.editable
                                onToggled: {
                                    // Staged, not written: the floating Save does that.
                                    settingsController.setField(fieldRow.modelData.fileName, fieldRow.modelData.label,
                                                                valueSwitch.checked ? "on" : "off");
                                    // A refused change (no session, a lock held by
                                    // another page) leaves groups untouched, so the
                                    // delegate is not rebuilt -- and a Switch clicked
                                    // by hand has dropped its binding. Put it back, so
                                    // it shows the stick, not the click.
                                    valueSwitch.checked = Qt.binding(() => fieldRow.shownValue === "on");
                                }
                                ToolTip.visible: hovered
                                ToolTip.delay: 400
                                ToolTip.text: fieldRow.modelData.label + ": " + fieldRow.shownValue
                            }
                            ComboBox {
                                id: valueCombo
                                objectName: "settingCombo"
                                visible: fieldRow.modelData.isSwitch !== true
                                anchors.fill: parent
                                model: fieldRow.modelData.options
                                // Falls back to -1 (nothing selected) for a byte value outside
                                // every known option -- e.g. "unknown (0x..)" -- rather than
                                // guess, since that value isn't one setField() could write back.
                                currentIndex: fieldRow.modelData.options.indexOf(fieldRow.shownValue)
                                enabled: fieldRow.editable
                                onActivated: (index) => {
                                    settingsController.setField(fieldRow.modelData.fileName, fieldRow.modelData.label,
                                                                valueCombo.textAt(index));
                                    // Same reason as the switch: a refusal must not
                                    // leave the combo showing a value that was never
                                    // staged.
                                    valueCombo.currentIndex = Qt.binding(
                                        () => fieldRow.modelData.options.indexOf(fieldRow.shownValue));
                                }
                            }
                            }

                            InfoButton {
                                objectName: "settingInfoButton"
                                explanationTitle: fieldRow.modelData.label
                                summaryText: fieldRow.modelData.explanation
                                explanationText: "Values: " + fieldRow.modelData.options.join(", ") + ".\n\n"
                                    + "Stored in " + fieldRow.modelData.fileName + " on this stick."
                            }

                            Rectangle {
                                visible: fieldRow.modelData.unsaved === true
                                radius: 3
                                color: Theme.warnBg
                                border.color: Theme.warnBorder
                                implicitWidth: unsavedText.implicitWidth + 2 * Theme.tightSpacing
                                implicitHeight: unsavedText.implicitHeight + Theme.tightSpacing
                                Label {
                                    id: unsavedText
                                    anchors.centerIn: parent
                                    text: "UNSAVED"
                                    font.pointSize: Theme.fontTiny
                                    font.bold: true
                                    color: Theme.warnText
                                }
                            }
                            Label {
                                visible: fieldRow.modelData.unsaved === true
                                text: "was " + fieldRow.modelData.value
                                color: Theme.textMuted
                                font.pointSize: Theme.fontSmall
                            }
                            ToolButton {
                                visible: fieldRow.modelData.unsaved === true
                                text: "Undo"
                                enabled: !editHost.writing
                                onClicked: settingsController.unstageField(fieldRow.modelData.fileName,
                                                                           fieldRow.modelData.label)
                            }
                            Item { Layout.fillWidth: true }
                        }
                    }
                }
            }
        }
    }
}
