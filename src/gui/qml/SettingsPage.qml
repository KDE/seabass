import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// Device Profile: rekordbox's player/mixer preference files on the
// stick. Picking a value stages it (UNSAVED, with an Undo); the floating
// Save writes every staged field, each backed up first.
Page {
    id: root
    required property string stickLabel
    required property string pioneerRoot

    SettingsController {
        id: settingsController
    }

    // Edit mode for this library: session, Save button, leave guard.
    EditSessionHost {
        id: editHost
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
            BackBreadcrumb {
                stack: root.StackView.view
                middleLabel: root.stickLabel
                title: "Device Profile"
                backEnabled: !settingsController.busy && !editHost.writing
                onHomeRequested: editHost.requestLeave(() => root.StackView.view.pop(null))
                onBackRequested: editHost.requestLeave(() => root.StackView.view.pop())
            }
            ToolButton {
                text: "ⓘ"
                font.pointSize: Theme.baseFontPointSize * 1.2
                ToolTip.visible: hovered
                // The Denon caveat is real but unverifiable and not
                // something a reader acts on while hovering; the file
                // list is what they came for.
                ToolTip.text: "Player and mixer preferences rekordbox writes to the stick: tempo range, "
                    + "quantize, auto cue level and similar."
            }
            Item { Layout.fillWidth: true }
            BusyIndicator {
                visible: settingsController.busy
                running: settingsController.busy
                implicitWidth: 24
                implicitHeight: 24
            }
        }
    }

    PageScrollView {
        anchors.fill: parent
        anchors.margins: 16

        ColumnLayout {
            width: parent.width
            spacing: 16

            Label {
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
                delegate: GroupBox {
                    id: groupBox
                    Layout.fillWidth: true
                    required property var modelData
                    title: modelData.title + " (" + modelData.fileName + ")"

                    ColumnLayout {
                        anchors.fill: parent
                        spacing: 4

                        Repeater {
                            model: groupBox.modelData.fields
                            delegate: RowLayout {
                                id: fieldRow
                                required property var modelData
                                readonly property string shownValue: modelData.unsaved ? modelData.pendingValue : modelData.value
                                Layout.fillWidth: true
                                Label { text: fieldRow.modelData.label + ":"; color: Theme.textMuted; Layout.preferredWidth: 180 }
                                ComboBox {
                                    id: valueCombo
                                    Layout.preferredWidth: 220
                                    model: fieldRow.modelData.options
                                    // Falls back to -1 (nothing selected) for a byte value outside
                                    // every known option -- e.g. "unknown (0x..)" -- rather than
                                    // guess, since that value isn't one setField() could write back.
                                    currentIndex: fieldRow.modelData.options.indexOf(fieldRow.shownValue)
                                    enabled: fieldRow.modelData.options.length > 0 && !settingsController.busy && !editHost.writing
                                    onActivated: (index) => {
                                        // Staged, not written: the floating Save does that.
                                        settingsController.setField(groupBox.modelData.fileName, fieldRow.modelData.label,
                                                                    valueCombo.textAt(index));
                                    }
                                }
                                Rectangle {
                                    visible: fieldRow.modelData.unsaved === true
                                    radius: 3
                                    color: Theme.warnBg
                                    border.color: Theme.warnBorder
                                    implicitWidth: unsavedText.implicitWidth + 8
                                    implicitHeight: unsavedText.implicitHeight + 4
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
                                    onClicked: settingsController.unstageField(groupBox.modelData.fileName, fieldRow.modelData.label)
                                }
                                Item { Layout.fillWidth: true }
                            }
                        }
                    }
                }
            }
        }
    }
}
