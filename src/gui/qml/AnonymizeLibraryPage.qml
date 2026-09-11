import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import SeabassGui

// Writes a de-identified, structurally-real copy of one or both real
// libraries as a single zip file: for regenerating this project's own
// committed test fixture, or for submitting a library to help test
// hardware/library shapes the maintainer doesn't have. Never sends
// anything anywhere itself -- only ever writes that one zip file, next
// to the location chosen below. Reachable from Preferences ->
// Experimental features, not an ActionCard: this is a maintainer/
// power-user tool, not a per-stick everyday action.
Page {
    id: root
    required property var mediaController
    required property var appSettingsController

    AnonymizeLibraryController {
        id: controller
    }

    // Populated from mediaController.sticks -- only sticks actually
    // mounted with a rekordbox or Engine catalog Seabass could read are
    // candidates; nothing here is ever a manually-typed path.
    property var candidateSticks: []
    property int selectedStickIndex: -1
    readonly property var selectedStick: root.selectedStickIndex >= 0 && root.selectedStickIndex < root.candidateSticks.length
        ? root.candidateSticks[root.selectedStickIndex] : null

    // The full path of the zip to write, proposed rather than demanded:
    // an export is named after the stick it came from and the day it was
    // taken, which is what anyone filing one would have typed anyway.
    // Editable, because the proposal is a guess about someone else's
    // filing system.
    property string outputPath: ""

    function proposedOutputPath() {
        const label = root.selectedStick ? String(root.selectedStick.label || "library") : "library";
        const safe = label.replace(/[^A-Za-z0-9._-]+/g, "-");
        const now = new Date();
        const pad = n => String(n).padStart(2, "0");
        const stamp = pad(now.getDate()) + "-" + pad(now.getMonth() + 1) + "-" + now.getFullYear();
        return appSettingsController.anonymizedExportDirectory() + "/" + safe + "-" + stamp + ".zip";
    }

    // Re-proposed whenever the chosen stick changes, but never over
    // something the user typed.
    property bool outputPathEdited: false
    onSelectedStickIndexChanged: {
        if (!root.outputPathEdited) {
            root.outputPath = root.proposedOutputPath();
        }
    }
    property int maxTracks: 0  // 0 = unlimited, see controller.run()'s own doc comment
    property var selectedHardware: ({})  // label -> true, for checked entries
    property string otherHardware: ""

    readonly property string hardwareText: {
        var parts = [];
        for (var key in root.selectedHardware) {
            if (root.selectedHardware[key]) {
                parts.push(key);
            }
        }
        if (root.otherHardware.trim().length > 0) {
            parts.push(root.otherHardware.trim());
        }
        return parts.join(", ");
    }

    // Grouped by vendor for display only -- a checkbox's selection key
    // (see selectedHardware/hardwareText below) is still just the bare
    // model name, so grouping never changes what ends up in MANIFEST.txt.
    readonly property var hardwareGroups: [
        { vendor: "Pioneer DJ / AlphaTheta", items: [
            "CDJ-3000", "CDJ-2000NXS2", "XDJ-RX3", "XDJ-RX2", "XDJ-XZ", "DJM-900NXS2", "DJM-750MK2", "DJM-A9",
        ] },
        { vendor: "Denon DJ / inMusic", items: [
            "Prime 4", "Prime 4+", "Prime GO(+)", "SC5000", "SC6000", "SC-Live 4",
        ] },
    ]

    function refreshCandidates() {
        var list = [];
        var count = mediaController.sticks.rowCount();
        for (var i = 0; i < count; i++) {
            var row = mediaController.sticks.get(i);
            if (row.mounted && (row.hasRekordbox || row.hasEngine)) {
                list.push(row);
            }
        }
        root.candidateSticks = list;
        root.selectedStickIndex = list.length > 0 ? 0 : -1;
    }

    Component.onCompleted: {
        mediaController.detect();
        root.refreshCandidates();
        if (root.outputPath.length === 0) {
            root.outputPath = root.proposedOutputPath();
        }
    }

    Connections {
        target: mediaController.sticks
        function onModelReset() { root.refreshCandidates() }
    }

    FileDialog {
        id: outputFileDialog
        title: "Where to write the anonymized export"
        fileMode: FileDialog.SaveFile
        defaultSuffix: "zip"
        nameFilters: ["Zip archive (*.zip)"]
        onAccepted: {
            root.outputPath = appSettingsController.localPathFromUrl(selectedFile.toString());
            root.outputPathEdited = true;
        }
    }

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
        background: Rectangle { color: Theme.surface }
        RowLayout {
            anchors.fill: parent
            anchors.margins: Theme.pageMargin
            spacing: 12
            BackBreadcrumb {
                stack: root.StackView.view
                middleLabel: "Preferences"
                title: "Export Anonymized Library"
                backEnabled: !controller.busy
                backDisabledTooltip: "Wait for it to finish before leaving this page"
                onHomeRequested: root.StackView.view.pop(null)
                onBackRequested: root.StackView.view.pop()
            }
            Item { Layout.fillWidth: true }
            BusyIndicator { running: controller.busy; visible: controller.busy; implicitWidth: 20; implicitHeight: 20 }
        }
    }

    PageScrollView {
        anchors.fill: parent
        anchors.margins: 16

        ColumnLayout {
            width: parent.width
            spacing: 16

            // What goes in the zip and why, on the page rather than behind
            // the (?): someone is being asked to hand over a copy of their
            // library, and "click here to find out what you are sending"
            // is the wrong shape for that question.
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: exportSummary.implicitHeight + 24
                color: Theme.groupBackground
                border.color: Theme.borderSubtle
                border.width: 1
                radius: 4

                ColumnLayout {
                    id: exportSummary
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 6

                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        font.bold: true
                        text: "The zip holds your library's catalogs and analysis files. No music."
                    }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: Theme.textMuted
                        text: "Every name is replaced with a placeholder: track titles, artists, "
                            + "comments, filenames, playlist and folder names, in all three catalogs "
                            + "and inside the analysis files, which embed the file path each track came "
                            + "from. The same real track gets the same placeholder everywhere, so the "
                            + "catalogs still agree with each other."
                    }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: Theme.textMuted
                        text: "What stays is the shape of the library: cue positions and colours, BPM, "
                            + "key, durations, file sizes, ratings, play counts, playlist order, and the "
                            + "waveform preview. That is where the bugs are: a cue landing two "
                            + "milliseconds out, a playlist that reorders itself, three catalogs "
                            + "disagreeing about one file. None of it can be reproduced from a "
                            + "description. Because it is metadata only, the zip is tens of megabytes, "
                            + "not the size of your music."
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: "Nothing is sent anywhere automatically; this only writes a single zip file "
                        + "next to the location you choose below. Review it yourself before sending anything."
                }
                InfoButton {
                    explanationTitle: "What gets sent, and to whom?"
                    summaryText: "Nothing is sent automatically. This writes one zip file to a "
                        + "location you pick, and nothing leaves your machine unless you email it."
                    explanationText:
                          "## If you want to help test Seabass\n"
                        + "1. Review the zip's contents yourself\n"
                        + "2. Attach it to an email to sebas@kde.org\n\n"
                        + "## Before you send it\n"
                        + "This data **may be published** as part of the project's test suite. "
                        + "Anything you would not want public should not go in the hardware or "
                        + "notes fields below -- mention it in the email instead.\n"
                }
            }

            GroupBox {
                label: Subtitle { text: "Source library" }
                Layout.fillWidth: true
                ColumnLayout {
                    anchors.fill: parent
                    spacing: 8

                    Label {
                        visible: root.candidateSticks.length === 0
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: Theme.textMuted
                        text: "No DeviceLibrary or Engine library detected on a connected, mounted stick. "
                            + "Insert one and reopen this page."
                    }

                    RowLayout {
                        visible: root.candidateSticks.length > 1
                        spacing: 8
                        Label { text: "Stick:" }
                        ComboBox {
                            Layout.fillWidth: true
                            model: root.candidateSticks.map((s) => s.label)
                            currentIndex: root.selectedStickIndex
                            onActivated: (index) => root.selectedStickIndex = index
                            enabled: !controller.busy
                        }
                    }

                    Label {
                        visible: root.selectedStick !== null
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: root.selectedStick
                            ? (root.selectedStick.label
                                + (root.selectedStick.hasRekordbox ? " · DeviceLibrary" : "")
                                + (root.selectedStick.hasEngine ? " · Engine" : ""))
                            : ""
                    }
                }
            }

            GroupBox {
                label: Subtitle { text: "Output location" }
                Layout.fillWidth: true
                ColumnLayout {
                    anchors.fill: parent
                    spacing: 8
                    Label {
                        text: "Must not already exist and be non-empty. A zip file with this name is "
                            + "created next to it."
                        wrapMode: Text.WordWrap
                        color: Theme.textMuted
                        font.pointSize: Theme.fontSmall
                    }
                    RowLayout {
                        spacing: 8
                        TextField {
                            objectName: "outputPathField"
                            Layout.fillWidth: true
                            enabled: !controller.busy
                            text: root.outputPath
                            placeholderText: "No location chosen yet"
                            onTextEdited: {
                                root.outputPath = text;
                                root.outputPathEdited = true;
                            }
                        }
                        Button {
                            text: "Choose…"
                            enabled: !controller.busy
                            onClicked: {
                                outputFileDialog.currentFolder =
                                    appSettingsController.toLocalFileUrl(appSettingsController.anonymizedExportDirectory());
                                outputFileDialog.open();
                            }
                        }
                    }

                    RowLayout {
                        spacing: 8
                        Label { text: "Max tracks:" }
                        SpinBox {
                            id: maxTracksSpin
                            from: 0
                            to: 999900
                            stepSize: 100
                            value: root.maxTracks
                            enabled: !controller.busy
                            onValueModified: root.maxTracks = value
                            textFromValue: (value) => value === 0 ? "Unlimited" : value.toString()
                            valueFromText: (text) => text === "Unlimited" ? 0 : parseInt(text)
                        }
                        Label {
                            text: "0 means every real track is included"
                            color: Theme.textMuted
                            font.pointSize: Theme.fontSmall
                        }
                        InfoButton {
                            explanationTitle: "What's kept, replaced, and removed"
                            summaryText: "Everything that identifies your music is replaced or "
                                + "removed. What stays is the shape of the library: timings, cues, "
                                + "and structure."
                            explanationText:
                                  "## Kept as-is\n"
                                + "Format, file size, bitrate, duration, BPM, key, hot and memory cue "
                                + "positions and colours, rating, play count, last-played date, "
                                + "whether a track is from a streaming service, playlist membership "
                                + "and position, and the low-resolution waveform this app uses.\n\n"
                                + "## Replaced with placeholder text\n"
                                + "Titles, artists, comments, cue comments, filenames, and "
                                + "playlist and folder names.\n\n"
                                + "## Removed entirely\n"
                                + "Artwork images, the detailed colour and scrolling waveform data "
                                + "rekordbox's own player uses (this app does not read it), and "
                                + "original file paths.\n"
                        }
                    }
                }
            }

            GroupBox {
                id: submissionBox
                label: Subtitle { text: "For the submission (optional; saved into MANIFEST.txt as entered)" }
                Layout.fillWidth: true
                ColumnLayout {
                    anchors.fill: parent
                    spacing: 8

                    Label { text: "Hardware you use:" }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 10
                        Repeater {
                            model: root.hardwareGroups
                            delegate: ColumnLayout {
                                id: groupDelegate
                                required property var modelData
                                Layout.fillWidth: true
                                spacing: 4

                                TableHeaderLabel { label: groupDelegate.modelData.vendor }
                                Flow {
                                    Layout.fillWidth: true
                                    spacing: 12
                                    Repeater {
                                        model: groupDelegate.modelData.items
                                        delegate: CheckBox {
                                            required property string modelData
                                            text: modelData
                                            checked: !!root.selectedHardware[modelData]
                                            enabled: !controller.busy
                                            onToggled: {
                                                var updated = Object.assign({}, root.selectedHardware);
                                                updated[modelData] = checked;
                                                root.selectedHardware = updated;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                    Label { text: "Other hardware (not listed above):" }
                    TextField {
                        Layout.fillWidth: true
                        placeholderText: "e.g. a controller or mixer not in the list"
                        enabled: !controller.busy
                        onTextEdited: root.otherHardware = text
                    }

                    Label { text: "Anything you'd like tested:" }
                    TextArea {
                        id: notesField
                        Layout.fillWidth: true
                        Layout.preferredHeight: 60
                        wrapMode: Text.WordWrap
                        enabled: !controller.busy
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                // Every field above lives inside a GroupBox, so the right
                // edge a reader lines things up against is the box's
                // CONTENT edge -- where the "Choose..." button and the two
                // text fields end -- not the frame around it. This row is
                // a direct child of the page column, so without the same
                // inset it overhangs them by the box's own padding: six
                // pixels in the desktop style, which is exactly enough to
                // look wrong. Borrowed from a real GroupBox rather than
                // hardcoded, because that padding is the style's to pick
                // and it differs between them.
                spacing: 12
                // Right-aligned, the same way SeabassDialog places its
                // accept button: the action that commits the page sits at
                // the trailing edge, where KDE's own dialogs put it.
                Item { Layout.fillWidth: true }
                Button {
                    text: "Export Library"
                    enabled: !controller.busy && root.selectedStick !== null && root.outputPath.length > 0
                    onClicked: controller.run(
                        root.selectedStick.hasRekordbox ? root.selectedStick.rekordboxPath : "",
                        root.selectedStick.hasEngine ? root.selectedStick.enginePath : "",
                        root.outputPath, root.maxTracks, root.hardwareText, notesField.text)
                }
            }

            // Selectable: this is the text a person is being asked to
            // send to someone, so it has to be copyable rather than
            // retyped off the screen.
            SelectableText {
                objectName: "errorText"
                visible: controller.errorMessage.length > 0
                text: controller.errorMessage
                color: Theme.danger
            }

            // The result is a confirmation, not page furniture: it says
            // where the file went and what the person still has to do
            // with it, and both are easy to scroll past when they sit
            // inline under a long form. A dialog makes the reader
            // acknowledge it, and gives the "show me the file" action
            // somewhere to live that is not competing with the form.
            MessageDialog {
                id: exportedDialog
                severity: SeabassDialog.Info
                title: "Anonymized library exported"
                headline: "Written to " + controller.outputZipPath
                detailText: controller.summaryText
                acceptText: "Show in Folder"
                rejectText: "Close"
                onAccepted: {
                    const lastSlash = controller.outputZipPath.lastIndexOf("/");
                    const folder = lastSlash >= 0
                        ? controller.outputZipPath.substring(0, lastSlash) : controller.outputZipPath;
                    Qt.openUrlExternally(appSettingsController.toLocalFileUrl(folder));
                }
            }

            Connections {
                target: controller
                function onResultChanged() {
                    if (controller.summaryText.length > 0 && controller.outputZipPath.length > 0) {
                        exportedDialog.open();
                    }
                }
            }
        }
    }

    BusyOverlay {
        anchors.fill: parent
        busy: controller.busy
        current: controller.progressCurrent
        total: controller.progressTotal
        label: controller.currentPhase.length > 0 ? controller.currentPhase + "..." : "Working..."
        // An export walks thousands of analysis files, so a bare "1874 /
        // 5976" leaves the reader guessing what is being counted.
        unitName: "files"
    }
}
