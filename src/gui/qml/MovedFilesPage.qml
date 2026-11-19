// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// Library Health's page for tracks whose file has moved on the stick
// (#103): every catalog still points at the old path, and a file no
// catalog names looks like the track. The default repair moves the file
// back where the catalogs expect it, which writes no database and keeps
// the three catalogs level; the other points the catalogs at the file,
// where export.pdb can take the new path. A file that only probably is
// the track is confirmed first; several files, or none, are listed and
// nothing is offered.
HealthCheckPage {
    id: root

    checkTitle: "Moved files"

    // Pointing the library at the file is Experimental
    // (docs/experimental-features.md): it rewrites export.pdb in place,
    // and no player has browsed a relinked track yet. Main.qml binds the
    // app's setting; moving the file back is offered either way.
    property bool experimentalFeaturesEnabled: false

    readonly property bool checked: consistencyController?.movedFilesChecked ?? false
    readonly property string error: consistencyController?.movedFilesError ?? ""
    readonly property var findings: consistencyController?.movedFiles ?? []
    readonly property int foundCount: consistencyController?.movedFileFoundCount ?? 0
    readonly property int oneClickCount: consistencyController?.movedFileOneClickCount ?? 0
    readonly property int unstagedOneClickCount: consistencyController?.unstagedMovedFileOneClickCount ?? 0
    readonly property int stagedCount: consistencyController?.movedFileStagedCount ?? 0
    readonly property bool canStage: !consistencyController?.busy && !consistencyController?.writing
        && !consistencyController?.stickReadOnly

    // The last folder and the file: "Old/Track.mp3", enough to tell two
    // places apart without the stick's mount point on every row.
    function shortPath(path) {
        const parts = path.split(/[\\/]/).filter(p => p.length > 0);
        return parts.length > 1 ? parts[parts.length - 2] + "/" + parts[parts.length - 1] : path;
    }

    Label {
        objectName: "movedFilesSummary"
        visible: root.checked || root.error.length > 0
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        text: {
            if (root.error.length > 0) {
                return "The check could not run: " + root.error;
            }
            if (root.findings.length === 0) {
                return "Every track's file is where its library expects it.";
            }
            return (root.findings.length === 1 ? "One track points" : root.findings.length + " tracks point")
                + " at a file that is not there. Found on the stick: " + root.foundCount + ".";
        }
    }

    Label {
        objectName: "movedFilesExplanation"
        visible: root.checked && root.findings.length > 0
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: Theme.textMuted
        text: "Moving a file back puts it where every library on the stick already looks for it, and writes no "
            + "database; Undo moves it back again. "
            + (root.experimentalFeaturesEnabled
               ? "Pointing the library at the file instead rewrites each catalog's path, which rekordbox's "
                 + "export can only take when the new path and file name are exactly as long as the old. "
               : "")
            + "A file with the same name and size is the track; one that only probably is gets asked "
            + "about first. Nothing changes until you Save."
    }

    RowLayout {
        visible: root.checked && root.findings.length > 0
        spacing: Theme.rowSpacing
        Button {
            objectName: "moveAllBackButton"
            text: root.stagedCount > 0 && root.unstagedOneClickCount === 0 ? "Unstage All"
                : (root.unstagedOneClickCount === 1 ? "Move 1 File Back"
                                                    : "Move " + root.unstagedOneClickCount + " Files Back")
            enabled: root.canStage && (root.unstagedOneClickCount > 0 || root.stagedCount > 0)
            onClicked: {
                if (root.stagedCount > 0 && root.unstagedOneClickCount === 0) {
                    consistencyController?.unstageMovedFiles();
                } else {
                    consistencyController?.moveAllFilesBack();
                }
            }
        }
    }

    // A candidate that only probably is the track: the page asks before
    // it stages, and names the reason so the person can say no.
    MessageDialog {
        id: confirmMoveDialog
        objectName: "confirmMoveDialog"
        property string expectedPath: ""
        property bool relink: false
        severity: SeabassDialog.Question
        title: relink ? "Point the library at this file?" : "Move this file back?"
        acceptText: relink ? "Stage Relink" : "Stage Move"
        onAccepted: {
            if (relink) {
                consistencyController?.relinkMovedFile(expectedPath);
            } else {
                consistencyController?.moveFileBack(expectedPath);
            }
        }
    }

    function stage(finding, relink) {
        if (finding.oneToConfirm) {
            confirmMoveDialog.expectedPath = finding.expectedPath;
            confirmMoveDialog.relink = relink;
            confirmMoveDialog.headline = (finding.title.length > 0 ? "\"" + finding.title + "\"" : "This track")
                + " only probably is " + root.shortPath(finding.candidates[0].filePath) + ": "
                + finding.candidates[0].reason + ".";
            confirmMoveDialog.detailText = relink
                ? "Every library's entry for the track is pointed at this file. Backed up first."
                : "The file is moved to " + finding.expectedPath + ". Undo moves it back.";
            confirmMoveDialog.open();
        } else if (relink) {
            consistencyController?.relinkMovedFile(finding.expectedPath);
        } else {
            consistencyController?.moveFileBack(finding.expectedPath);
        }
    }

    // The findings, as many as there are: a list that scrolls, with room
    // under the last row for the floating Save button.
    Flickable {
        id: rowsView
        objectName: "movedFilesView"
        Layout.fillWidth: true
        Layout.fillHeight: true
        clip: true
        contentWidth: width
        contentHeight: rowsColumn.implicitHeight
        bottomMargin: root.saveClearance
        interactive: contentHeight + bottomMargin > height
        ScrollBar.vertical: BigScrollBar {}
        ColumnLayout {
            id: rowsColumn
            width: rowsView.width
            spacing: Theme.sectionSpacing
            Repeater {
                objectName: "movedFileRows"
                model: root.findings
                delegate: ColumnLayout {
                    id: findingRow
                    required property var modelData
                    readonly property bool staged: findingRow.modelData.staged.length > 0
                    readonly property bool offered: findingRow.modelData.oneClick || findingRow.modelData.oneToConfirm
                    Layout.fillWidth: true
                    spacing: Theme.tightSpacing
                    Label {
                        objectName: "title_" + findingRow.modelData.expectedPath
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                        font.bold: true
                        font.strikeout: findingRow.staged
                        text: findingRow.modelData.title.length > 0 ? findingRow.modelData.title
                            : root.shortPath(findingRow.modelData.expectedPath)
                    }
                    Label {
                        Layout.fillWidth: true
                        elide: Text.ElideMiddle
                        color: Theme.textMuted
                        text: (findingRow.modelData.artist.length > 0 ? findingRow.modelData.artist + " · " : "")
                            + findingRow.modelData.libraries + " expect " + findingRow.modelData.expectedPath
                    }
                    Label {
                        objectName: "state_" + findingRow.modelData.expectedPath
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: {
                            const c = findingRow.modelData.candidates;
                            if (findingRow.staged) {
                                return findingRow.modelData.staged === "relink"
                                    ? "Staged: the library will point at " + c[0].filePath
                                    : "Staged: " + c[0].filePath + " moves back";
                            }
                            if (c.length === 0) {
                                return "No file on the stick matches this track.";
                            }
                            if (c.length === 1) {
                                return (c[0].confidence === "high" ? "Found: " : "Probably: ") + c[0].filePath
                                    + " (" + c[0].reason + ")";
                            }
                            return c.length + " files could be this track, so nothing is offered: "
                                + c.map(x => x.filePath + " (" + x.reason + ")").join("; ");
                        }
                    }
                    RowLayout {
                        visible: findingRow.offered
                        spacing: Theme.rowSpacing
                        Button {
                            objectName: "move_" + findingRow.modelData.expectedPath
                            text: findingRow.staged ? "Unstage" : "Move Back"
                            enabled: root.canStage
                            onClicked: {
                                if (findingRow.staged) {
                                    consistencyController?.unstageMovedFile(findingRow.modelData.expectedPath);
                                } else {
                                    root.stage(findingRow.modelData, false);
                                }
                            }
                        }
                        Button {
                            objectName: "relink_" + findingRow.modelData.expectedPath
                            visible: !findingRow.staged && root.experimentalFeaturesEnabled
                            text: "Point Library at File"
                            enabled: root.canStage && findingRow.modelData.relinkRefusal.length === 0
                            onClicked: root.stage(findingRow.modelData, true)
                        }
                        ExperimentalBadge {
                            visible: !findingRow.staged && root.experimentalFeaturesEnabled
                        }
                        Label {
                            objectName: "relinkRefusal_" + findingRow.modelData.expectedPath
                            visible: !findingRow.staged && root.experimentalFeaturesEnabled
                                && findingRow.modelData.relinkRefusal.length > 0
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            color: Theme.textMuted
                            text: "Cannot point the library at it: " + findingRow.modelData.relinkRefusal
                        }
                    }
                }
            }
        }
    }
}
