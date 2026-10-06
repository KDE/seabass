// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// The selected stick at the top of the home screen's pane: its icon,
// name, path and size, which catalogs are on it, the eject or mount
// button, and the line that says what to do with a stick that has no
// library. This is the row every stick in the old home list carried,
// moved out whole so the rail layout can show it once, for the stick the
// rail has selected; what it shows and does has not changed.
Rectangle {
    id: root
    // The stick model's row: label, mountPoint, devicePath, mounted,
    // hasRekordbox, hasEngine, hasOneLibrary, rekordboxPath, enginePath,
    // isSdCard, isFolder, isBrowsedBackup, libraryId, safeToUnplug,
    // readOnly and, when the locator could read it, capacityBytes.
    required property var row
    required property var mediaController
    required property var playbackController
    // What the backup advisor found for this stick, for the no-library
    // line: the advice object, its state, and the other mounted stick
    // whose library could be copied onto this one (null when none).
    property var advice: null
    property var cloneSource: null
    property string adviceState: ""
    // In flight (mount, unmount, or an automatic mount) via this row's
    // own devicePath, not the app-wide busy flag, which disabled every
    // other row's button too. Worked out here unless the page says.
    property bool thisRowBusy: root.mediaController.busy
        && root.mediaController.busyDevicePath === root.devicePath

    // A folder row cannot be closed from here alone: the page first has
    // to let go of the folder's edit session, and refuses while that has
    // unsaved changes (StickListPage.releaseOpenedFolder).
    signal closeFolderRequested(string mountPoint)
    // The "Sync Needed" badge: the player will offer to import the
    // rekordbox library over the Engine one, and Sync Cue Points is where
    // a save settles that.
    signal syncRequested(string stickLabel, string rekordboxPath, string enginePath)

    // How far the stick's name sits from this row's left edge, so the
    // pane can put its group heading and cards on the same line: read off
    // the margin, the icon and the gap that place the name.
    readonly property real textInset: contentColumn.anchors.leftMargin + stickIcon.size + rowContent.spacing

    // ---- The row, read defensively: a fake row may lack a role, and the
    // pane has no row at all while no stick is selected.
    readonly property bool hasRow: root.row !== null && root.row !== undefined
    readonly property string label: root.hasRow ? String(root.row.label || "") : ""
    readonly property string mountPoint: root.hasRow ? String(root.row.mountPoint || "") : ""
    readonly property string devicePath: root.hasRow ? String(root.row.devicePath || "") : ""
    readonly property bool mounted: root.hasRow && root.row.mounted === true
    readonly property bool hasRekordbox: root.hasRow && root.row.hasRekordbox === true
    readonly property bool hasEngine: root.hasRow && root.row.hasEngine === true
    readonly property bool hasOneLibrary: root.hasRow && root.row.hasOneLibrary === true
    readonly property bool syncNeeded: root.hasRow && root.row.syncNeeded === true
    // #60: "3 cue lists disagree" once the cue pass has examined every
    // analysis file, "2 analysis files could not be read" when it could
    // not; empty otherwise. The backup advisor counts it from the tracks
    // the cue pass read (BackupAdvisorController, advice.cueListsText).
    readonly property string cueListsText: root.advice && root.advice.cueListsText
        ? String(root.advice.cueListsText) : ""
    readonly property bool isSdCard: root.hasRow && root.row.isSdCard === true
    readonly property bool isFolder: root.hasRow && root.row.isFolder === true
    readonly property bool safeToUnplug: root.hasRow && root.row.safeToUnplug === true
    // Hidden rather than shown as "0 B" when the locator could not read
    // a capacity, which happens for a drive with no partition table.
    readonly property real capacityBytes: root.hasRow && root.row.capacityBytes ? Number(root.row.capacityBytes) : 0
    readonly property bool hasKnownLibrary: root.hasRekordbox || root.hasEngine

    // One size for the icons in this row, the header's size.
    readonly property int buttonIconSize: Math.round(Theme.scaled(22))

    implicitHeight: contentColumn.implicitHeight + 2 * Theme.cardPadding
    color: Theme.surface
    border.color: Theme.border
    radius: Theme.cornerRadius

    ColumnLayout {
        id: contentColumn
        anchors.fill: parent
        anchors.margins: Theme.cardPadding
        spacing: Theme.tightSpacing

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.rowSpacing

            // A plain Item + explicit MouseArea, not an ItemDelegate:
            // ItemDelegate's own hover/press background isn't reliably
            // gated by `enabled` in this KDE-Breeze/Material style mashup
            // (`enabled: !mounted` still left the row hover-highlighting
            // and accepting clicks once mounted). ScanPage.qml's track
            // rows settled on this same Rectangle+MouseArea sidestep.
            Item {
                Layout.fillWidth: true
                implicitHeight: rowContent.implicitHeight

                Rectangle {
                    anchors.fill: parent
                    anchors.margins: -Theme.tightSpacing
                    radius: Theme.cornerRadius
                    visible: !root.mounted
                    color: rowMouseArea.pressed ? Theme.rowPressed
                        : rowMouseArea.containsMouse ? Theme.rowHover
                        : "transparent"
                }

                MouseArea {
                    id: rowMouseArea
                    objectName: "mountRowArea"
                    anchors.fill: parent
                    // Not gated on the whole app being busy: mountStick()
                    // queues behind whatever else is running, so only this
                    // row's own task disables it.
                    enabled: !root.mounted && !root.thisRowBusy
                    hoverEnabled: !root.mounted && !root.thisRowBusy
                    ToolTip.visible: containsMouse
                    ToolTip.text: "Click to mount " + root.label
                    onClicked: root.mediaController.mountStick(root.devicePath)
                }

                RowLayout {
                    id: rowContent
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: Theme.rowSpacing

                    UsbStickIcon {
                        id: stickIcon
                        objectName: "stickIcon"
                        size: Theme.iconSizeNormal
                        Layout.alignment: Qt.AlignVCenter
                        isSdCard: root.isSdCard
                        isFolder: root.isFolder
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        RowLayout {
                            Layout.fillWidth: true
                            Subtitle {
                                objectName: "stickLabel"
                                text: root.label
                                color: Theme.text
                            }
                            Item { Layout.fillWidth: true }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.tightSpacing
                            Label {
                                objectName: "stickPathLabel"
                                text: root.mounted ? root.mountPoint : root.devicePath
                                color: Theme.textMuted
                                font.pointSize: Theme.baseFontPointSize * 0.9
                                elide: Text.ElideMiddle
                                // A Text's Layout.minimumWidth defaults to
                                // its implicit width, so without this the
                                // elide could never fire and a long mount
                                // point pushed the size beside it off the
                                // row. The maximum keeps a short path from
                                // stretching; the minimum lets a long one
                                // shorten.
                                Layout.minimumWidth: 0
                                // Whole pixels, with one to spare: a layout
                                // hands out whole pixels, and a Text given a
                                // fraction less than its natural width
                                // elides (/media/sebas/WHALESHARK2 measures
                                // 208.03 and got 208). Same fix as
                                // BackBreadcrumb's crumbs.
                                Layout.preferredWidth: Math.ceil(implicitWidth) + 1
                                Layout.maximumWidth: Math.ceil(implicitWidth) + 1
                                Layout.fillWidth: true
                            }
                            Label {
                                objectName: "stickCapacityLabel"
                                visible: root.capacityBytes > 0
                                text: Theme.humanBytes(root.capacityBytes)
                                color: Theme.textMuted
                                font.pointSize: Theme.baseFontPointSize * 0.9
                            }
                            Item { Layout.fillWidth: true }
                        }
                        // What is on the stick or, once it is unmounted,
                        // whether it may be pulled. One row for both, at
                        // one height, so pressing eject changes what the
                        // row says without moving it under the pointer.
                        RowLayout {
                            objectName: "stickStateRow"
                            spacing: Theme.tightSpacing
                            Layout.preferredHeight: Math.max(unmountedLabel.implicitHeight,
                                                             deviceLibraryBadge.implicitHeight)
                            // "OK to unplug" where that is provably true,
                            // the old description where it is not: the
                            // reader asks this right after pressing eject.
                            StatusBadge {
                                objectName: "okToUnplugBadge"
                                visible: !root.mounted && root.safeToUnplug
                                label: "OK to unplug"
                                badgeColor: Theme.good
                            }
                            Label {
                                id: unmountedLabel
                                objectName: "unmountedLabel"
                                visible: !root.mounted && !root.safeToUnplug
                                text: "(not mounted)"
                                color: Theme.textMuted
                            }
                            // Said, not left as an empty line, for a
                            // mounted stick with no catalog on it.
                            Label {
                                objectName: "noLibraryLabel"
                                visible: root.mounted && !root.hasRekordbox
                                         && !root.hasEngine && !root.hasOneLibrary
                                text: "No library"
                                color: Theme.textMuted
                            }
                            StatusBadge {
                                id: deviceLibraryBadge
                                objectName: "deviceLibraryBadge"
                                visible: root.mounted && root.hasRekordbox
                                label: "DeviceLibrary"
                                badgeColor: Theme.accent
                            }
                            StatusBadge {
                                objectName: "oneLibraryBadge"
                                visible: root.mounted && root.hasOneLibrary
                                label: "OneLibrary"
                                badgeColor: Theme.accent
                            }
                            StatusBadge {
                                objectName: "engineBadge"
                                visible: root.mounted && root.hasEngine
                                label: "Engine"
                                badgeColor: Theme.accent
                            }
                            StatusBadge {
                                objectName: "cueListsBadge"
                                visible: root.mounted && root.cueListsText.length > 0
                                label: root.cueListsText
                                badgeColor: Theme.warnIcon
                                tooltipText: root.cueListsText.indexOf("could not be read") >= 0
                                    ? "Analysis files that are missing, or whose cue lists could not be read, so "
                                      + "whether their two generations of cue list agree is not known."
                                    : "Analysis files whose legacy cue lists, the ones an XDJ-RX2 or a "
                                      + "CDJ-3000X shows, differ from the modern lists Seabass reads. A sync would "
                                      + "write what Seabass shows."
                            }
                            StatusBadge {
                                objectName: "syncNeededBadge"
                                visible: root.mounted && root.syncNeeded
                                label: "Sync Needed"
                                badgeColor: Theme.warnIcon
                                clickable: true
                                tooltipText: "The rekordbox library changed since the player last imported it, so "
                                    + "the player will offer the import, which replaces the Engine side, cues "
                                    + "included. A save on Sync Cue Points settles it."
                                onClicked: root.syncRequested(root.label,
                                                              root.hasRow ? String(root.row.rekordboxPath || "") : "",
                                                              root.hasRow ? String(root.row.enginePath || "") : "")
                            }
                        }
                    }
                }
            }

            // Mount and unmount run on a background thread and can take
            // a moment: while this row's own operation is in flight, a
            // spinner stands in the eject button's place.
            BusyIndicator {
                objectName: "stickBusyIndicator"
                visible: root.thisRowBusy
                running: visible
                Layout.preferredWidth: Theme.iconSizeLarge
                Layout.preferredHeight: Theme.iconSizeLarge
                Layout.alignment: Qt.AlignVCenter
            }

            // A folder was never mounted, so there is nothing to eject:
            // the equivalent is dropping it from the list, which touches
            // nothing on disk.
            IconToolButton {
                visible: root.isFolder
                objectName: "closeFolderButton"
                // The text is what an assistive reader announces.
                text: "Remove from list"
                iconName: "window-close"
                iconSize: root.buttonIconSize
                Layout.preferredWidth: Theme.iconSizeLarge
                Layout.preferredHeight: Theme.iconSizeLarge
                Layout.alignment: Qt.AlignVCenter
                ToolTip.visible: hovered
                ToolTip.text: "Remove " + root.label + " from this list (nothing on disk is changed)"
                onClicked: root.closeFolderRequested(root.mountPoint)
            }

            IconToolButton {
                // Not gated on the app-wide busy flag: that disabled every
                // other row's button while any one stick's task ran, which
                // read as "eject does nothing". A click queues instead.
                visible: !root.thisRowBusy && !root.isFolder
                objectName: "ejectButton"
                enabled: true
                text: root.mounted ? "Eject" : "Mount"
                iconName: "media-eject"
                iconSize: root.buttonIconSize
                // Kept upright always: an eject icon turned over to mean
                // "mount" reads as a broken icon. The tooltip and the
                // click-anywhere row carry the "mount" meaning instead.
                Layout.preferredWidth: Theme.iconSizeLarge
                Layout.preferredHeight: Theme.iconSizeLarge
                Layout.alignment: Qt.AlignVCenter
                ToolTip.visible: hovered
                ToolTip.text: root.mounted ? "Eject " + root.label : "Mount " + root.label
                onClicked: {
                    if (root.mounted) {
                        // Stop first: unmounting out from under an open
                        // file handle on the playing track would be bad.
                        root.playbackController.stop();
                        root.mediaController.unmountStick(root.devicePath);
                    } else {
                        root.mediaController.mountStick(root.devicePath);
                    }
                }
            }
        }

        // No wall of disabled cards for a stick that is not mounted yet
        // (click the row to mount it) or that has no library on it: this
        // line says what there is to do instead.
        Label {
            objectName: "noKnownLibraryLabel"
            Layout.fillWidth: true
            Layout.topMargin: Theme.tightSpacing
            visible: !root.hasKnownLibrary
            wrapMode: Text.WordWrap
            color: Theme.textMuted
            text: root.mounted
                ? "No DeviceLibrary or Engine library detected on this stick. "
                  + (root.cloneSource !== null && root.cloneSource !== undefined
                     ? root.cloneSource.detail + " "
                     : "")
                  + (root.adviceState === "restore" && root.advice
                     ? root.advice.detail + " (" + root.advice.backupLabel + ")"
                     : "Restore a backup onto it, or format it.")
                : "Click to mount, then Seabass will show what's available here."
        }
    }
}
