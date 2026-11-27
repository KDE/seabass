// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import SeabassGui
import "RecordingSave.js" as RecordingSave

// Save as FLAC (experimental): the form that makes a recording a FLAC
// with tags and a cover in a folder on this computer, through the
// window's KeepRecordingController, whose checks it shows as they are
// typed and asks again on start. The Recordings page and Browse's Own
// Recordings both open it: one form, so that every rule holds on both.
// It fills its host, over which the form opens.
Item {
    id: root
    anchors.fill: parent

    // A KeepRecordingController; a QtObject with the same shape in the
    // tests.
    property var keepController: null
    // The root of the stick the recordings are on, handed to the
    // controller as the form opens; "" for a host whose own controller
    // hands it on (the Recordings page's RecordingsController).
    property string stickRoot: ""
    readonly property int keepRevision: root.keepController !== null ? root.keepController.revision : 0

    // This build can save at all, and this recording: a WAV (the encoder
    // reads only those) with audio in it.
    readonly property bool available: root.keepController !== null && root.keepController.available === true
    function offers(path, holdsNoAudio) {
        return root.available && root.keepController.canKeep(path) && holdsNoAudio !== true;
    }

    // Opens the form for a recording: its path, file name, when it was
    // recorded, and what its WAV header says (unfinished, the seconds it
    // holds and declares, whether that is usable, the bytes missing).
    function openFor(path, name, recorded, unfinished, heldSeconds, declaredSeconds, declaredUnusable,
                     missingBytes) {
        keepDialog.openFor(path, name, recorded, unfinished, heldSeconds, declaredSeconds, declaredUnusable,
                           missingBytes);
    }

    SeabassDialog {
        id: keepDialog
        objectName: "keepDialog"
        severity: SeabassDialog.Question
        showBadge: false
        width: Math.min(root.width - 2 * Theme.pageMargin, Theme.scaled(640))
        title: qsTr("Save Recording as FLAC")
        headline: qsTr("%1 becomes a FLAC file with tags and a cover on this computer.").arg(keepDialog.recording)
        detailText: qsTr("The recording on the stick stays as it is.")

        property string wavPath: ""
        property string recording: ""
        // The recording's header was not completed: the audio the file
        // holds is kept only when the person ticks the box for it. The
        // header's own length is -1 when it declares none.
        property bool unfinished: false
        property double heldSeconds: -1
        property double declaredSeconds: -1
        // The header declares a length no recording has; how many bytes
        // less the file holds than the header declares (-1 when not cut
        // short).
        property bool declaredUnusable: false
        property double missingBytes: -1
        // Shown once the person has tried to start, or has typed.
        property bool tried: false
        readonly property var keep: root.keepController
        readonly property string fileName: keep ? keep.fileNameFor(artistField.text, titleField.text, folderField.text) : ""
        readonly property string titleProblem: titleField.text.trim().length === 0 ? qsTr("The track name is needed.") : ""
        readonly property var textProblems: keep
            ? [keep.textProblem("artist", artistField.text), keep.textProblem("title", titleField.text),
               keep.textProblem("album", albumField.text), keep.textProblem("genre", genreField.text)] : []
        readonly property string yearProblem: keep ? keep.yearProblem(yearField.text) : ""
        readonly property string folderProblem: keep ? keep.folderProblem(folderField.text) : ""
        readonly property string targetProblem: keep && titleProblem.length === 0 && folderProblem.length === 0
            ? (root.keepRevision, keep.targetProblem(folderField.text, artistField.text, titleField.text)) : ""
        readonly property string coverProblem: keep && keep.coverError.length > 0 ? keep.coverError : ""
        readonly property var problems: [titleProblem].concat(textProblems)
            .concat([yearProblem, folderProblem, targetProblem, coverProblem])
            .filter(p => p.length > 0)
        readonly property bool ready: problems.length === 0 && keep !== null && keep.coverPreparing !== true
            && (!keepDialog.unfinished || keepUnfinishedBox.checked)

        function openFor(path, name, recorded, unfinished, heldSeconds, declaredSeconds, declaredUnusable,
                         missingBytes) {
            // The stick the form is for: what is kept from, and whose
            // folders are refused.
            if (root.stickRoot.length > 0) {
                root.keepController.stickRoot = root.stickRoot;
            }
            const suggested = root.keepController.suggestionsFor(name, recorded);
            keepDialog.wavPath = path;
            keepDialog.recording = name;
            keepDialog.unfinished = unfinished === true;
            keepDialog.heldSeconds = heldSeconds ?? -1;
            keepDialog.declaredSeconds = declaredSeconds ?? -1;
            keepDialog.declaredUnusable = declaredUnusable === true;
            keepDialog.missingBytes = missingBytes ?? -1;
            keepUnfinishedBox.checked = false;
            keepDialog.tried = false;
            artistField.text = suggested.artist;
            titleField.text = suggested.title;
            albumField.text = suggested.album;
            genreField.text = suggested.genre;
            yearField.text = suggested.year > 0 ? String(suggested.year) : "";
            folderField.text = suggested.folder;
            root.keepController.pickCover("");
            keepDialog.open();
        }
        function start() {
            keepDialog.tried = true;
            if (!keepDialog.ready) {
                return;
            }
            const taken = root.keepController.keep({
                wavPath: keepDialog.wavPath,
                recording: keepDialog.recording,
                artist: artistField.text,
                title: titleField.text,
                album: albumField.text,
                genre: genreField.text,
                year: yearField.text,
                folder: folderField.text,
                keepUnfinished: keepDialog.unfinished && keepUnfinishedBox.checked,
            });
            if (taken) {
                keepDialog.close();
            }
        }

        // Later than SeabassDialog's own onOpened, which hands focus to a
        // footer button: the form is what is being asked for.
        onOpened: Qt.callLater(() => {
            keepDialog.markFooterDefault(keepStartButton);
            artistField.forceActiveFocus();
        })

        footer: DialogButtonBox {
            Button {
                id: keepStartButton
                objectName: "keepStartButton"
                text: qsTr("Save as FLAC")
                enabled: keepDialog.ready
                // Not AcceptRole, which would close the form whatever
                // keep() answers: start() closes it once the job is taken.
                DialogButtonBox.buttonRole: DialogButtonBox.ActionRole
                onClicked: keepDialog.start()
            }
            Button {
                objectName: "keepCancelButton"
                text: qsTr("Cancel")
                DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            }
        }
        ExperimentalBadge {}

        // A recording whose header was not completed: said first, both
        // lengths where there are two, and kept only with the box ticked.
        // The cause is given as what usually leads there, not as fact.
        ColumnLayout {
            objectName: "keepUnfinishedNotice"
            Layout.fillWidth: true
            visible: keepDialog.unfinished
            spacing: Theme.tightSpacing
            Label {
                objectName: "keepUnfinishedLabel"
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Theme.danger
                readonly property string missing: RecordingSave.missingNote(keepDialog.declaredSeconds, keepDialog.heldSeconds,
                                                                   keepDialog.missingBytes)
                text: keepDialog.declaredUnusable
                    ? qsTr("The header of this recording was not completed: the header's length is not usable, and "
                        + "the file holds %1 of audio. This happens when a player is switched off or a stick is "
                        + "pulled while recording. The recording on the stick is not changed.")
                        .arg(RecordingSave.duration(keepDialog.heldSeconds))
                    : keepDialog.declaredSeconds >= 0 && missing.length > 0
                    ? qsTr("The header of this recording was not completed: the header declares %1 of audio, the "
                        + "file holds %2 (%3). This happens when a player is switched off or a stick is pulled "
                        + "while recording. The recording on the stick is not changed.")
                        .arg(RecordingSave.duration(keepDialog.declaredSeconds)).arg(RecordingSave.duration(keepDialog.heldSeconds))
                        .arg(missing)
                    : keepDialog.declaredSeconds >= 0
                    ? qsTr("The header of this recording was not completed: the header declares %1 of audio, the "
                        + "file holds %2. This happens when a player is switched off or a stick is pulled while "
                        + "recording. The recording on the stick is not changed.")
                        .arg(RecordingSave.duration(keepDialog.declaredSeconds)).arg(RecordingSave.duration(keepDialog.heldSeconds))
                    : qsTr("The header of this recording was not completed: it does not say how long the audio "
                        + "is. The file holds %1 of audio. This happens when a player is switched off or a stick "
                        + "is pulled while recording. The recording on the stick is not changed.")
                        .arg(RecordingSave.duration(keepDialog.heldSeconds))
            }
            SeabassCheckBox {
                id: keepUnfinishedBox
                objectName: "keepUnfinishedBox"
                text: qsTr("Keep the audio the file holds")
            }
        }

        GridLayout {
            Layout.fillWidth: true
            columns: 2
            columnSpacing: Theme.rowSpacing
            rowSpacing: Theme.tightSpacing

            Label { text: qsTr("Artist") }
            TextField {
                id: artistField
                objectName: "keepArtistField"
                Layout.fillWidth: true
                onAccepted: keepDialog.start()
            }
            Label { text: qsTr("Track name") }
            TextField {
                id: titleField
                objectName: "keepTitleField"
                Layout.fillWidth: true
                onAccepted: keepDialog.start()
            }
            Label { text: qsTr("Album") }
            TextField {
                id: albumField
                objectName: "keepAlbumField"
                Layout.fillWidth: true
                onAccepted: keepDialog.start()
            }
            Label { text: qsTr("Genre") }
            TextField {
                id: genreField
                objectName: "keepGenreField"
                Layout.fillWidth: true
                onAccepted: keepDialog.start()
            }
            Label { text: qsTr("Year") }
            TextField {
                id: yearField
                objectName: "keepYearField"
                Layout.preferredWidth: Theme.scaled(90)
                maximumLength: 4
                inputMethodHints: Qt.ImhDigitsOnly
                onAccepted: keepDialog.start()
            }
            Label {
                Layout.alignment: Qt.AlignTop
                text: qsTr("Cover")
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.rowSpacing
                Rectangle {
                    objectName: "keepCoverFrame"
                    Layout.preferredWidth: Theme.scaled(72)
                    Layout.preferredHeight: Theme.scaled(72)
                    color: Theme.surface
                    border.color: Theme.borderSubtle
                    border.width: 1
                    radius: Theme.cornerRadius
                    Image {
                        objectName: "keepCoverPreview"
                        anchors.fill: parent
                        anchors.margins: 1
                        fillMode: Image.PreserveAspectFit
                        asynchronous: true
                        source: keepDialog.keep ? keepDialog.keep.coverPreview : ""
                        visible: source.toString().length > 0
                    }
                    SeabassIcon {
                        anchors.centerIn: parent
                        visible: !keepDialog.keep || keepDialog.keep.coverPreview.length === 0
                        iconName: "media-record"
                        size: Theme.iconSizeSmall
                        color: Theme.textMuted
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.tightSpacing
                    Label {
                        objectName: "keepCoverLabel"
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: keepDialog.coverProblem.length > 0 ? Theme.danger : Theme.textMuted
                        font.pointSize: Theme.fontSmall
                        text: {
                            const keep = keepDialog.keep;
                            if (!keep || keep.coverPath.length === 0) return qsTr("No cover");
                            if (keep.coverPreparing) return qsTr("Preparing the cover...");
                            if (keep.coverError.length > 0) return keep.coverError;
                            return qsTr("Embedded as %1 × %2 pixels").arg(keep.coverWidth).arg(keep.coverHeight);
                        }
                    }
                    RowLayout {
                        spacing: Theme.rowSpacing
                        Button {
                            objectName: "keepCoverButton"
                            text: qsTr("Choose Picture…")
                            icon.source: Theme.iconUrl("folder-open")
                            onClicked: coverFileDialog.open()
                        }
                        Button {
                            objectName: "keepNoCoverButton"
                            text: qsTr("No Cover")
                            enabled: keepDialog.keep !== null && keepDialog.keep.coverPath.length > 0
                            onClicked: keepDialog.keep.pickCover("")
                        }
                    }
                }
            }
            Label { text: qsTr("Folder") }
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.rowSpacing
                TextField {
                    id: folderField
                    objectName: "keepFolderField"
                    Layout.fillWidth: true
                    onAccepted: keepDialog.start()
                }
                Button {
                    objectName: "keepFolderButton"
                    text: qsTr("Choose…")
                    onClicked: keepFolderDialog.open()
                }
            }
        }
        Label {
            objectName: "keepFileNameLabel"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textMuted
            text: qsTr("Saved as %1").arg(keepDialog.fileName)
        }
        Label {
            objectName: "keepProblemsLabel"
            Layout.fillWidth: true
            visible: text.length > 0
            wrapMode: Text.WordWrap
            color: Theme.danger
            text: {
                const lines = keepDialog.problems.slice();
                const refused = keepDialog.keep ? keepDialog.keep.errorMessage : "";
                if (keepDialog.tried && refused.length > 0 && lines.indexOf(refused) < 0) {
                    lines.push(refused);
                }
                return lines.join("\n");
            }
        }
    }
    FileDialog {
        id: coverFileDialog
        title: qsTr("Choose a picture for the cover")
        nameFilters: [qsTr("Pictures (*.jpg *.jpeg *.png *.webp *.bmp *.gif *.tif *.tiff)")]
        onAccepted: root.keepController.pickCover(selectedFile.toString())
    }
    FolderDialog {
        id: keepFolderDialog
        title: qsTr("Choose the folder for the FLAC file")
        currentFolder: root.keepController ? root.keepController.folderUrl(folderField.text) : ""
        onAccepted: folderField.text = root.keepController.localPath(selectedFolder.toString())
    }
}
