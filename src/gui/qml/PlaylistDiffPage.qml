// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// Compare Playlists: two playlists of one catalog side by side, the way
// git shows two versions of a file. The set is the finding: red rows are
// tracks only A has, green rows only B has. Order is a side note: a track
// in both lists but somewhere else is amber on both sides with a ribbon
// between them, never reported as missing. Runs of identical rows fold
// away like unchanged context. Read-only, like Browse Library.
Page {
    id: root
    required property string stickLabel
    required property string rekordboxPath
    required property string enginePath
    // For the key notation; may be null (tests, previews), then Camelot.
    property var appSettingsController: null

    // Overridable so a test can hand in a fake with known rows; the real
    // app never sets it.
    property var controller: realController

    PlaylistDiffController {
        id: realController
    }

    readonly property bool hasRekordbox: root.rekordboxPath.length > 0
    readonly property bool hasEngine: root.enginePath.length > 0
    property string format: root.hasEngine ? "engine" : "rekordbox"
    function currentPath() {
        return root.format === "engine" ? root.enginePath : root.rekordboxPath;
    }
    function rescan() {
        controller.scan(root.format, root.currentPath());
    }
    onFormatChanged: rescan()
    Component.onCompleted: rescan()

    readonly property string keyNotation: root.appSettingsController && root.appSettingsController.keyNotation
        ? root.appSettingsController.keyNotation : "camelot"
    function keyLabel(key) {
        if (!key || key.length === 0) {
            return "";
        }
        return root.keyNotation === "traditional" ? Theme.traditionalLabel(key) : Theme.camelotLabel(key);
    }

    // The pickers' model: every playlist with its entry count.
    readonly property var pickerModel: {
        const names = controller.playlistNames || [];
        const counts = controller.playlistTrackCounts || {};
        const out = [];
        for (let i = 0; i < names.length; ++i) {
            out.push({name: names[i], count: counts[names[i]] || 0});
        }
        return out;
    }
    function pickerIndexOf(name) {
        const model = root.pickerModel;
        for (let i = 0; i < model.length; ++i) {
            if (model[i].name === name) {
                return i;
            }
        }
        return -1;
    }

    // Row tints, from the semantic colours: the sign tells, the tint helps.
    readonly property color onlyATint: Qt.alpha(Theme.danger, 0.16)
    readonly property color onlyBTint: Qt.alpha(Theme.good, 0.16)
    readonly property color movedTint: Qt.alpha(Theme.conflictText, 0.14)
    function tintFor(kind) {
        if (kind === "only") {
            return "transparent";
        }
        return kind === "moved" ? root.movedTint : "transparent";
    }

    // Geometry the header row, the delegates and the ribbons all share.
    readonly property real gutterWidth: Theme.scaled(56)
    readonly property real rowHeight: Theme.scaled(40)
    readonly property real positionWidth: Theme.scaled(44)
    readonly property real bpmWidth: Theme.scaled(40)
    readonly property real keyWidth: Theme.scaled(36)
    readonly property real timeWidth: Theme.scaled(44)

    header: ToolBar {
        // Every side zeroed so the header's inset is Theme.pageMargin
        // and nothing else (see ScanPage.qml's header for why).
        leftPadding: 0
        rightPadding: 0
        topPadding: 0
        bottomPadding: Theme.headerBottomPadding
        background: Rectangle { color: Theme.surface }
        implicitHeight: headerLayout.implicitHeight + 20

        ColumnLayout {
            id: headerLayout
            anchors.fill: parent
            anchors.margins: Theme.pageMargin
            spacing: 8

            RowLayout {
                Layout.fillWidth: true
                spacing: 12
                Layout.bottomMargin: Theme.headerBottomPadding - headerLayout.spacing
                BackBreadcrumb {
                    stack: root.StackView.view
                    middleLabel: root.stickLabel
                    title: "Compare Playlists"
                    onHomeRequested: root.StackView.view.pop(null)
                    onBackRequested: root.StackView.view.pop()
                }
                Item { Layout.fillWidth: true }
                SeabassBusyIndicator { running: controller.busy; visible: controller.busy; implicitWidth: 20; implicitHeight: 20 }
                LibrarySourceToggle {
                    visible: root.hasRekordbox && root.hasEngine
                    current: root.format
                    hasRekordbox: root.hasRekordbox
                    hasEngine: root.hasEngine
                    hasOneLibrary: false
                    onSourceRequested: (value) => root.format = value
                }
            }

            // A, swap, B: the two pickers share the row's width.
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.rowSpacing
                Label {
                    objectName: "labelA"
                    text: "A"
                    font.bold: true
                    font.family: Theme.dataFamily
                    color: Theme.danger
                }
                PlaylistPickerCombo {
                    id: comboA
                    objectName: "playlistACombo"
                    Layout.fillWidth: true
                    implicitHeight: Theme.compactControlHeight
                    model: root.pickerModel
                    currentIndex: root.pickerIndexOf(controller.playlistA)
                    onPlaylistPicked: (index, modelData) => controller.playlistA = modelData.name
                }
                IconToolButton {
                    objectName: "swapButton"
                    iconName: "exchange-positions"
                    text: "Swap A and B"
                    ToolTip.visible: hovered
                    ToolTip.text: "Swap A and B"
                    onClicked: controller.swapPlaylists()
                }
                Label {
                    text: "B"
                    font.bold: true
                    font.family: Theme.dataFamily
                    color: Theme.good
                }
                PlaylistPickerCombo {
                    id: comboB
                    objectName: "playlistBCombo"
                    Layout.fillWidth: true
                    implicitHeight: Theme.compactControlHeight
                    model: root.pickerModel
                    currentIndex: root.pickerIndexOf(controller.playlistB)
                    onPlaylistPicked: (index, modelData) => controller.playlistB = modelData.name
                }
                SeabassCheckBox {
                    objectName: "foldCheck"
                    text: "Fold identical runs"
                    checked: controller.foldIdentical
                    onToggled: controller.foldIdentical = checked
                }
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.leftMargin: Theme.pageMargin
        anchors.rightMargin: Theme.pageMargin
        spacing: Theme.sectionSpacing

        // The other playlists A shares tracks with, most alike first; a
        // chip sets B. This is what sorts out a drawer of near copies.
        Flow {
            objectName: "relatives"
            Layout.fillWidth: true
            spacing: Theme.tightSpacing
            visible: controller.relatives.length > 0
            Label {
                text: "Overlaps with"
                color: Theme.textMuted
                height: Theme.compactControlHeight
                verticalAlignment: Text.AlignVCenter
                rightPadding: Theme.tightSpacing
            }
            Repeater {
                model: controller.relatives
                delegate: Button {
                    id: chip
                    required property var modelData
                    objectName: "relativeChip"
                    implicitHeight: Theme.compactControlHeight
                    // The style's own width guess clips the detail text;
                    // the row knows how wide it is.
                    implicitWidth: chipRow.implicitWidth + leftPadding + rightPadding
                    leftPadding: Theme.rowSpacing
                    rightPadding: Theme.rowSpacing
                    flat: chip.modelData.name !== controller.playlistB
                    highlighted: chip.modelData.name === controller.playlistB
                    onClicked: controller.playlistB = chip.modelData.name
                    ToolTip.visible: hovered
                    ToolTip.text: chip.modelData.detail
                    contentItem: RowLayout {
                        id: chipRow
                        spacing: Theme.tightSpacing
                        Label {
                            text: chip.modelData.glyph
                            font.family: Theme.dataFamily
                            font.bold: true
                            color: chip.modelData.relation === "superset" ? Theme.good
                                : chip.modelData.relation === "subset" ? Theme.danger
                                : chip.modelData.relation === "identical" ? Theme.accent : Theme.textMuted
                        }
                        Label { text: chip.modelData.name; elide: Text.ElideRight; Layout.maximumWidth: 260 }
                        Label { text: chip.modelData.detail; color: Theme.textMuted; font.pointSize: Theme.fontSmall }
                    }
                }
            }
        }

        // The counts and the verdict, one line.
        RowLayout {
            objectName: "summaryRow"
            Layout.fillWidth: true
            spacing: Theme.rowSpacing
            Label {
                objectName: "onlyACount"
                text: "−" + controller.onlyACount
                font.family: Theme.dataFamily
                font.bold: true
                color: Theme.danger
            }
            Label { text: "only in A"; color: Theme.textMuted; font.pointSize: Theme.fontSmall }
            Label {
                objectName: "onlyBCount"
                text: "+" + controller.onlyBCount
                font.family: Theme.dataFamily
                font.bold: true
                color: Theme.good
            }
            Label { text: "only in B"; color: Theme.textMuted; font.pointSize: Theme.fontSmall }
            Label {
                objectName: "movedCount"
                text: controller.movedCount
                font.family: Theme.dataFamily
                font.bold: true
                color: Theme.conflictText
            }
            Label { text: "moved"; color: Theme.textMuted; font.pointSize: Theme.fontSmall }
            Label {
                objectName: "sharedCount"
                text: controller.sharedCount
                font.family: Theme.dataFamily
                font.bold: true
                color: Theme.textMuted
            }
            Label { text: "in both"; color: Theme.textMuted; font.pointSize: Theme.fontSmall }
            Label {
                objectName: "verdict"
                text: controller.errorMessage.length > 0 ? controller.errorMessage : controller.verdict
                color: controller.errorMessage.length > 0 ? Theme.danger : Theme.text
                Layout.fillWidth: true
                Layout.leftMargin: Theme.rowSpacing
                elide: Text.ElideRight
            }
            Button {
                objectName: "copyOnlyA"
                flat: true
                text: "Copy only-in-A"
                enabled: controller.onlyACount > 0
                onClicked: controller.copyToClipboard(controller.onlyInAText())
            }
            Button {
                objectName: "copyOnlyB"
                flat: true
                text: "Copy only-in-B"
                enabled: controller.onlyBCount > 0
                onClicked: controller.copyToClipboard(controller.onlyInBText())
            }
        }

        // Column headers, mirroring the delegate's geometry exactly.
        Item {
            objectName: "columnHeader"
            Layout.fillWidth: true
            implicitHeight: Theme.scaled(26)
            readonly property real cellWidth: (width - root.gutterWidth) / 2
            Rectangle {
                anchors.fill: parent
                color: Theme.groupBackground
            }
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.borderSubtle }
            // No names while there are no playlists (a scan running, or
            // one that failed): the picked pair is another catalog's.
            DiffCellHeader {
                objectName: "headerA"
                x: 0
                width: parent.cellWidth
                height: parent.height
                title: controller.playlistNames.length > 0 ? controller.playlistA : ""
                count: controller.entriesA
                accent: Theme.danger
            }
            DiffCellHeader {
                x: parent.cellWidth + root.gutterWidth
                width: parent.cellWidth
                objectName: "headerB"
                height: parent.height
                title: controller.playlistNames.length > 0 ? controller.playlistB : ""
                count: controller.entriesB
                accent: Theme.good
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ListView {
                id: diffList
                objectName: "diffList"
                anchors.fill: parent
                clip: true
                interactive: contentHeight > height
                model: controller.rows
                readonly property real cellWidth: (width - root.gutterWidth) / 2
                ScrollBar.vertical: BigScrollBar {}

                delegate: Rectangle {
                    id: row
                    required property int index
                    required property string kind
                    required property string leftKind
                    required property int leftPosition
                    required property string leftTitle
                    required property string leftArtist
                    required property double leftBpm
                    required property string leftKey
                    required property double leftDurationSeconds
                    required property int leftPartnerPosition
                    required property string rightKind
                    required property int rightPosition
                    required property string rightTitle
                    required property string rightArtist
                    required property double rightBpm
                    required property string rightKey
                    required property double rightDurationSeconds
                    required property int rightPartnerPosition
                    required property int partnerRow
                    required property int foldCount

                    width: ListView.view.width
                    height: root.rowHeight
                    readonly property bool isFold: row.kind === "fold"
                    readonly property bool hovered: rowMouse.containsMouse
                        || (diffList.hoveredPartner === row.index && diffList.hoveredPartner >= 0)
                    color: row.isFold ? Theme.groupBackground
                        : row.hovered ? Theme.rowHover
                        : (row.index % 2 === 0 ? Theme.rowEven : Theme.rowOdd)

                    MouseArea {
                        id: rowMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: row.isFold ? Qt.PointingHandCursor : Qt.ArrowCursor
                        onContainsMouseChanged: diffList.hoveredPartner = containsMouse ? row.partnerRow : -1
                        onClicked: {
                            if (row.isFold) {
                                controller.expandFold(row.index);
                            }
                        }
                    }

                    // A fold: one line standing for the identical run.
                    Label {
                        visible: row.isFold
                        anchors.centerIn: parent
                        text: "⋯ " + row.foldCount + " identical tracks"
                        font.family: Theme.dataFamily
                        color: Theme.textMuted
                    }

                    DiffCell {
                        visible: !row.isFold
                        x: 0
                        width: diffList.cellWidth
                        height: parent.height
                        kind: row.leftKind
                        side: "A"
                        position: row.leftPosition
                        title: row.leftTitle
                        artist: row.leftArtist
                        bpm: row.leftBpm
                        keyName: row.leftKey
                        durationSeconds: row.leftDurationSeconds
                        partnerPosition: row.leftPartnerPosition
                    }
                    Rectangle {
                        visible: !row.isFold
                        x: diffList.cellWidth
                        width: root.gutterWidth
                        height: parent.height
                        color: "transparent"
                        Rectangle { anchors.left: parent.left; width: 1; height: parent.height; color: Theme.borderSubtle }
                        Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: Theme.borderSubtle }
                    }
                    DiffCell {
                        visible: !row.isFold
                        x: diffList.cellWidth + root.gutterWidth
                        width: diffList.cellWidth
                        height: parent.height
                        kind: row.rightKind
                        side: "B"
                        position: row.rightPosition
                        title: row.rightTitle
                        artist: row.rightArtist
                        bpm: row.rightBpm
                        keyName: row.rightKey
                        durationSeconds: row.rightDurationSeconds
                        partnerPosition: row.rightPartnerPosition
                    }
                }

                property int hoveredPartner: -1
                onContentYChanged: ribbons.requestPaint()
                onWidthChanged: ribbons.requestPaint()
                onHeightChanged: ribbons.requestPaint()
                onCountChanged: ribbons.requestPaint()
                onHoveredPartnerChanged: ribbons.requestPaint()
            }

            // The ribbons in the gutter: a stub for a track one side lacks,
            // a curve from a moved track to its other half. Rows are all
            // one height, so a row's y is arithmetic, on or off screen.
            Canvas {
                id: ribbons
                objectName: "ribbons"
                anchors.fill: diffList
                // Nothing under it needs the pointer; the rows do.
                enabled: false
                onPaint: {
                    const ctx = getContext("2d");
                    ctx.reset();
                    ctx.clearRect(0, 0, width, height);
                    const model = controller.rows;
                    if (!model || diffList.count === 0) {
                        return;
                    }
                    const x0 = diffList.cellWidth;
                    const x1 = x0 + root.gutterWidth;
                    const w = root.gutterWidth;
                    const rowH = root.rowHeight;
                    const first = Math.max(0, Math.floor(diffList.contentY / rowH) - 1);
                    const last = Math.min(diffList.count - 1, Math.ceil((diffList.contentY + height) / rowH) + 1);
                    const yOf = (i) => i * rowH - diffList.contentY + rowH / 2;
                    const colour = (name) => name === "good" ? Theme.good : name === "danger" ? Theme.danger
                        : name === "accent" ? Theme.accent : Theme.conflictText;
                    const drawn = {};
                    for (let i = first; i <= last; ++i) {
                        const idx = model.index(i, 0);
                        const kind = model.data(idx, Qt.UserRole + 1);
                        if (kind === "fold") {
                            continue;
                        }
                        const leftKind = model.data(idx, Qt.UserRole + 2);
                        const rightKind = model.data(idx, Qt.UserRole + 10);
                        const partner = model.data(idx, Qt.UserRole + 18);
                        const y = yOf(i);
                        ctx.lineWidth = 1.5;
                        if (kind === "same") {
                            ctx.strokeStyle = Theme.borderSubtle;
                            ctx.beginPath(); ctx.moveTo(x0, y); ctx.lineTo(x1, y); ctx.stroke();
                            continue;
                        }
                        if (leftKind === "only") {
                            ctx.strokeStyle = colour("danger");
                            ctx.beginPath(); ctx.moveTo(x0, y); ctx.lineTo(x0 + w * 0.4, y); ctx.stroke();
                        }
                        if (rightKind === "only") {
                            ctx.strokeStyle = colour("good");
                            ctx.beginPath(); ctx.moveTo(x1, y); ctx.lineTo(x1 - w * 0.4, y); ctx.stroke();
                        }
                        const movedHere = leftKind === "moved" || rightKind === "moved";
                        if (movedHere && partner >= 0) {
                            const key = Math.min(i, partner) + ":" + Math.max(i, partner);
                            if (drawn[key]) {
                                continue;
                            }
                            drawn[key] = true;
                            const fromLeft = leftKind === "moved";
                            const ya = fromLeft ? y : yOf(partner);
                            const yb = fromLeft ? yOf(partner) : y;
                            const hot = diffList.hoveredPartner === i || diffList.hoveredPartner === partner;
                            ctx.strokeStyle = hot ? colour("accent") : colour("conflict");
                            ctx.lineWidth = hot ? 2.5 : 1.5;
                            ctx.beginPath();
                            ctx.moveTo(x0, ya);
                            ctx.bezierCurveTo(x0 + w * 0.5, ya, x1 - w * 0.5, yb, x1, yb);
                            ctx.stroke();
                        } else if (movedHere) {
                            // The other half is inside a fold: a stub says so.
                            ctx.strokeStyle = colour("conflict");
                            ctx.beginPath();
                            if (leftKind === "moved") { ctx.moveTo(x0, y); ctx.lineTo(x0 + w * 0.4, y); }
                            else { ctx.moveTo(x1, y); ctx.lineTo(x1 - w * 0.4, y); }
                            ctx.stroke();
                        }
                    }
                }
                Connections {
                    target: controller.rows
                    ignoreUnknownSignals: true
                    function onModelReset() { ribbons.requestPaint(); }
                }
            }

            EmptyState {
                objectName: "emptyNote"
                visible: !controller.busy && diffList.count === 0
                iconName: controller.errorMessage.length > 0 ? "dialog-warning" : "view-list-details"
                text: controller.errorMessage.length > 0 ? controller.errorMessage
                    : (controller.playlistNames.length === 0 ? "This catalog has no playlists." : "Nothing to show.")
            }
        }
    }

    // One half of a row: sign and position, artist and title, then bpm,
    // key and time in the data face so the columns line up.
    component DiffCell: Item {
        id: cell
        property string kind: ""
        property string side: "A"
        property int position: 0
        property string title: ""
        property string artist: ""
        property double bpm: 0
        property string keyName: ""
        readonly property string keyText: root.keyLabel(cell.keyName)
        property double durationSeconds: 0
        property int partnerPosition: 0
        readonly property bool blank: cell.kind.length === 0
        readonly property bool isOnly: cell.kind === "only"
        readonly property bool isMoved: cell.kind === "moved"
        readonly property color inkFor: cell.isOnly ? (cell.side === "A" ? Theme.danger : Theme.good)
            : cell.isMoved ? Theme.conflictText : Theme.textMuted
        Rectangle {
            anchors.fill: parent
            color: cell.isOnly ? (cell.side === "A" ? root.onlyATint : root.onlyBTint)
                : cell.isMoved ? root.movedTint : "transparent"
        }
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: Theme.tightSpacing
            anchors.rightMargin: Theme.tightSpacing
            spacing: Theme.tightSpacing
            visible: !cell.blank
            Label {
                Layout.preferredWidth: root.positionWidth
                text: (cell.isOnly ? (cell.side === "A" ? "−" : "+") : cell.isMoved ? "↕" : " ") + " " + cell.position
                font.family: Theme.dataFamily
                font.pointSize: Theme.fontSmall
                color: cell.inkFor
                horizontalAlignment: Text.AlignRight
            }
            Label {
                text: cell.artist
                font.bold: true
                elide: Text.ElideRight
                Layout.maximumWidth: parent.width * 0.35
            }
            Label {
                text: cell.title
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
            Label {
                visible: cell.isMoved
                text: (cell.side === "A" ? "B" : "A") + " #" + cell.partnerPosition
                font.family: Theme.dataFamily
                font.pointSize: Theme.fontSmall
                color: Theme.conflictText
            }
            Label {
                Layout.preferredWidth: root.bpmWidth
                text: cell.bpm > 0 ? Math.round(cell.bpm) : ""
                font.family: Theme.dataFamily
                font.pointSize: Theme.fontSmall
                color: Theme.textMuted
                horizontalAlignment: Text.AlignRight
            }
            Label {
                Layout.preferredWidth: root.keyWidth
                text: cell.keyText
                font.family: Theme.dataFamily
                font.pointSize: Theme.fontSmall
                color: cell.keyText.length > 0 ? Theme.colorForKey(cell.keyName) : Theme.textMuted
                horizontalAlignment: Text.AlignHCenter
            }
            Label {
                Layout.preferredWidth: root.timeWidth
                text: cell.durationSeconds > 0 ? Theme.trackTime(cell.durationSeconds * 1000) : ""
                font.family: Theme.dataFamily
                font.pointSize: Theme.fontSmall
                color: Theme.textMuted
                horizontalAlignment: Text.AlignRight
            }
        }
    }

    component DiffCellHeader: Item {
        id: head
        property string title: ""
        property int count: 0
        property color accent: Theme.text
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: Theme.tightSpacing
            anchors.rightMargin: Theme.tightSpacing
            spacing: Theme.tightSpacing
            Rectangle { width: 8; height: 8; radius: 4; color: head.accent }
            Label {
                text: head.title
                font.bold: true
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
            Label {
                text: head.count + " tracks"
                font.family: Theme.dataFamily
                font.pointSize: Theme.tableHeaderSize
                color: Theme.textMuted
            }
        }
    }
}
