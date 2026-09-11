import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// A dedicated, discoverable entry point for LibraryConsistencyController's
// junk-cue cleanup (see domain::JunkCueFinder's own doc comment: a memory
// cue sitting at 0:00 is almost always an accidental leftover from
// analysis or import). This already existed as a secondary section on
// Library Health, whose own top-level billing is about missing-file
// repair -- easy to miss if you're looking for "clean up stray cues"
// specifically, per real user feedback. That section stays exactly where
// it was on Library Health too (a legitimate contextual shortcut, not a
// replaced one) -- this page is the "reachable through the main nav
// structure" home for the same feature, filed under Clean-up and
// Housekeeping where someone would actually think to look for it.
// Reuses LibraryConsistencyController wholesale rather than a new
// controller: scan() computes both checks together already (see that
// class's own comment on why that's cheap), this page just never renders
// the missing-file half of its result.
Page {
    id: root
    required property string stickLabel
    required property string rekordboxPath
    required property string enginePath

    LibraryConsistencyController {
        id: consistencyController
    }

    // Edit mode for this library: session, floating Save, leave guard.
    EditSessionHost {
        id: editHost
        feature: "library-health"
        anchors.fill: parent
        libraryId: typeof EditSessionRegistry !== "undefined"
            ? EditSessionRegistry.libraryIdForPath(root.rekordboxPath.length > 0 ? root.rekordboxPath : root.enginePath) : ""
        stickLabel: root.stickLabel
        rekordboxPath: root.rekordboxPath
        enginePath: root.enginePath
    }

    // "" scopes the scan to the whole library (every catalog present),
    // same empty-means-all convention every other picker in this app
    // uses (see SyncPage.qml's own selectedPlaylistName).
    property string selectedPlaylistName: ""

    // "All tracks" first (no meaningful single count across up to three
    // independent catalogs, so left blank rather than showing a
    // misleading sum), then the union of playlist names across whichever
    // catalogs are present -- built from consistencyController's own
    // unfiltered scan, so this list doesn't shrink once a playlist is
    // selected.
    readonly property var playlistPickerModel: [{name: "All tracks", count: ""}].concat(
        consistencyController.playlistNames.map((n) =>
            ({name: n, count: consistencyController.playlistTrackCounts[n] ?? 0})))

    function formatLabel(format) {
        if (format === "engine") return "Engine OS";
        if (format === "onelibrary") return "OneLibrary";
        return "DeviceLibrary";
    }

    Component.onCompleted: consistencyController.scan(root.rekordboxPath, root.enginePath, root.selectedPlaylistName)

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
        // Opaque background override, see AppSettingsPage.qml's header
        // for why (KDE's Breeze style bleeds the window behind Seabass
        // through an unstyled ToolBar).
        background: Rectangle { color: Theme.surface }

        RowLayout {
            anchors.fill: parent
            anchors.margins: Theme.pageMargin
            spacing: 12
            BackBreadcrumb {
                stack: root.StackView.view
                middleLabel: "Housekeeping"
                title: "Clean Up Stray Cues"
                backEnabled: !consistencyController.writing
                onHomeRequested: editHost.requestLeave(() => root.StackView.view.pop(null))
                onBackRequested: editHost.requestLeave(() => root.StackView.view.pop())
            }
            Item { Layout.fillWidth: true }
            Label {
                text: "Playlist:"
                color: Theme.textMuted
            }
            PlaylistPickerCombo {
                Layout.minimumWidth: 140
                model: root.playlistPickerModel
                currentIndex: {
                    if (root.selectedPlaylistName.length === 0) {
                        return 0;
                    }
                    for (var i = 1; i < root.playlistPickerModel.length; i++) {
                        if (root.playlistPickerModel[i].name === root.selectedPlaylistName) {
                            return i;
                        }
                    }
                    return 0;
                }
                ToolTip.visible: hovered
                ToolTip.text: "Scope Clean Up Stray Cues to one playlist instead of the whole library"
                onPlaylistPicked: (index, modelData) => {
                    root.selectedPlaylistName = index === 0 ? "" : modelData.name;
                    consistencyController.scan(root.rekordboxPath, root.enginePath, root.selectedPlaylistName);
                }
            }
            RowLayout {
                visible: consistencyController.busy
                spacing: 8
                BusyIndicator { running: true; implicitWidth: 20; implicitHeight: 20 }
                Label {
                    text: consistencyController.scanningFormat.length > 0
                        ? "Scanning " + root.formatLabel(consistencyController.scanningFormat) + "..."
                        : "Scanning..."
                    color: Theme.textMuted
                }
            }
        }
    }

    MessageDialog {
        id: confirmRemoveJunkCueDialog
        property int pendingIndex: -1
        severity: SeabassDialog.Question
        title: "Remove This Cue?"
        headline: "Stages removing this memory cue sitting at 0:00 from the track; Save writes it."
        detailText: "Backed up first."
        acceptText: "Stage Removal"
        onAccepted: if (pendingIndex >= 0) consistencyController.removeJunkCue(pendingIndex)
    }

    MessageDialog {
        id: confirmRemoveAllJunkCuesDialog
        severity: SeabassDialog.Warning
        destructive: true
        title: "Remove All " + junkCueListView.count + " Memory Cue(s) at 0:00?"
        headline: "This permanently removes every memory cue at 0:00 currently listed, across every "
            + "catalog on this stick -- a real write, not just dismissing them from view."
        detailText: "Everything is backed up first, but make sure this is really what you want before "
            + "continuing."
        acceptText: "Stage Removal"
        onAccepted: consistencyController.removeAllJunkCues()
    }

    MessageDialog {
        id: confirmIgnoreAllJunkCuesDialog
        severity: SeabassDialog.Question
        title: "Ignore all memory cues at 0:00"
        headline: "Dismisses every memory cue at 0:00 currently listed, just for this view."
        detailText: "Nothing is written, they'll show up again the next time you scan."
        acceptText: "Ignore All"
        onAccepted: consistencyController.ignoreAllJunkCues()
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 8

        Label {
            visible: consistencyController.errorMessage.length > 0
            text: consistencyController.errorMessage
            color: Theme.danger
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
        Label {
            visible: consistencyController.statusMessage.length > 0
            text: consistencyController.statusMessage
            color: Theme.good
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        Rectangle {
            visible: junkCueListView.count > 0
            Layout.fillWidth: true
            Layout.bottomMargin: 12
            radius: 6
            color: Theme.groupBackground
            border.color: Theme.borderSubtle
            implicitHeight: summaryRow.implicitHeight + 20

            RowLayout {
                id: summaryRow
                anchors.fill: parent
                anchors.margins: 10
                spacing: 12
                Label {
                    text: junkCueListView.count + " memory cue(s) sitting at 0:00, likely accidental"
                    font.bold: true
                }
                Item { Layout.fillWidth: true }
                Button {
                    text: "Stage Removing All"
                    enabled: !consistencyController.busy && !consistencyController.writing
                    onClicked: confirmRemoveAllJunkCuesDialog.open()
                }
                Button {
                    text: "Ignore All"
                    enabled: !consistencyController.busy
                    onClicked: confirmIgnoreAllJunkCuesDialog.open()
                }
            }
        }

        ListView {
            // Room to scroll the last row clear of the Save overlay (bottom right).
            bottomMargin: 80
            id: junkCueListView
            // Not draggable when everything already fits.
            interactive: contentHeight > height
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: consistencyController.junkCues
            spacing: 4
            ScrollBar.vertical: BigScrollBar {}

            delegate: ItemDelegate {
                id: junkDelegate
                width: ListView.view.width
                hoverEnabled: false

                required property int index
                required property string format
                required property string title
                required property string artist
                required property bool staged

                contentItem: RowLayout {
                    spacing: 8
                    Label {
                        text: junkDelegate.title + " - " + junkDelegate.artist
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                    Rectangle {
                        radius: 3
                        color: Theme.groupBackground
                        border.color: Theme.borderSubtle
                        implicitWidth: junkFormatLabelText.implicitWidth + 8
                        implicitHeight: junkFormatLabelText.implicitHeight + 4
                        Label {
                            id: junkFormatLabelText
                            anchors.centerIn: parent
                            text: root.formatLabel(junkDelegate.format)
                            font.pointSize: Theme.fontTiny
                            font.bold: true
                            color: Theme.textMuted
                        }
                    }
                    StatusBadge {
                        visible: junkDelegate.staged
                        label: "Staged"
                        badgeColor: Theme.warnText
                        tooltipText: "Not on the stick yet: press Save."
                    }
                    Button {
                        text: junkDelegate.staged ? "Unstage" : "Remove"
                        enabled: !consistencyController.busy && !consistencyController.writing
                        onClicked: {
                            if (junkDelegate.staged) {
                                consistencyController.unstageJunkCue(junkDelegate.index);
                            } else {
                                confirmRemoveJunkCueDialog.pendingIndex = junkDelegate.index;
                                confirmRemoveJunkCueDialog.open();
                            }
                        }
                    }
                    Button {
                        text: "Ignore"
                        enabled: !consistencyController.busy
                        onClicked: consistencyController.ignoreJunkCue(junkDelegate.index)
                    }
                }
            }
        }

        Label {
            Layout.alignment: Qt.AlignHCenter
            Layout.topMargin: 24
            Layout.bottomMargin: 12
            visible: junkCueListView.count === 0 && !consistencyController.busy
            text: "No memory cues are sitting at 0:00."
            color: Theme.textMuted
        }
    }
}
