// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import SeabassGui
import "common/HomeModel.js" as HomeModel

// A Page, not a plain Item, specifically so it gets the same
// Material-style implicit background every other page in this app gets
// for free -- a plain Item has no background mechanism at all, which is
// exactly why this page (the very first one shown) kept rendering the
// Qt-default white despite the window/StackView-level background fixes
// applied elsewhere, while every `Page`-rooted page rendered correctly.
Page {
    id: root
    required property var mediaController
    required property var playbackController
    required property var appSettingsController
    required property var backupAdvisor
    // Null in tests and in any build that does not wire it: the label
    // below simply never appears.
    property var updateChecker: null
    // The edit-lock registry (EditSessionRegistry singleton; a fake in
    // tests): which libraries another instance is editing right now.
    property var editRegistry: typeof EditSessionRegistry !== "undefined" ? EditSessionRegistry : null
    function refreshLocks() {
        if (root.editRegistry !== null && root.editRegistry !== undefined) {
            root.editRegistry.refreshLocks();
        }
    }
    // Only one folder is open at a time, so closing it, opening another
    // and browsing a backup all let go of the current one first -- and
    // its staged edits would go with it. Refused while its session is
    // dirty or saving; a clean session is closed, releasing its lock.
    // Returns whether the way is clear.
    function releaseOpenedFolder() {
        const model = root.sticksModel;
        const count = HomeModel.rowCount(model);
        for (let i = 0; i < count; ++i) {
            const row = HomeModel.rowAt(model, i);
            if (!row.isFolder) {
                continue;
            }
            const reg = root.editRegistry;
            if (reg && reg.hasSession(row.libraryId)) {
                const session = reg.sessionFor(row.libraryId);
                if (session && (session.dirty === true || session.writing === true)) {
                    openFolderError.text = "\"" + row.label
                        + "\" has unsaved changes. Save or discard them first.";
                    openFolderError.open();
                    return false;
                }
                reg.closeSession(row.libraryId);
            }
        }
        return true;
    }
    // How many rows are actual removable media, i.e. not a folder
    // someone opened and not a browsed backup.
    //
    // The model answers this itself; the count over the copied rows is
    // for the QML tests, which stand a plain array or a ListModel in for
    // the model and have no such property. Same shape as the editRegistry
    // guard above: this page is built to run against fakes.
    readonly property int removableStickCount: {
        const model = root.sticksModel;
        if (model !== null && model.removableCount !== undefined) {
            return model.removableCount;
        }
        return root.rows.filter((row) => row.isFolder !== true).length;
    }

    // The metadata store's count, readable by a test that wants to check
    // the menu entry's enabled state against the thing that decides it
    // rather than against a hardcoded expectation of what the machine holds.
    readonly property int homeBackupsMetadataCount: homeBackups.metadataTrackCount
    readonly property int homeBackupsFullCount: homeBackups.fullBackupCount

    // One size for every icon in the header, the menu's included. Set, not
    // read off the menu button: KDE's ToolButton never sets icon.width, so
    // binding to it gave 0 there and the heart filled its whole button.
    // The window's header row (AppHeaderOverlay) shows over this page's
    // header, with the home's menu: it names this place and sets how much
    // of the header's right to keep clear.
    readonly property string appHeaderPlace: "home"
    property real appHeaderReserve: 0
    // The menu's two entries, called by the window.
    function browseFullBackup() { openBackupDialog.open(); }
    function openFolder() { openFolderDialog.open(); }

    // ---- the stick model, read as plain rows ---------------------------
    //
    // MediaController's DetectedStickListModel (count, get(i), inserts,
    // removes, dataChanged), a ListModel, or a plain array of stick
    // objects in the tests: every row is read through HomeModel, which
    // the rail reads them through too.
    readonly property var sticksModel: root.mediaController.sticks !== undefined ? root.mediaController.sticks : null
    // The roles the pane reads, copied out of the model into a plain
    // object: a get(i) result does not follow later changes, so the page
    // takes a fresh copy of a row whenever the model says it changed, and
    // the stick row and the cards get the new object.
    readonly property var rowRoles: ["label", "mountPoint", "devicePath", "mounted", "hasRekordbox", "hasEngine",
        "hasOneLibrary", "syncNeeded", "rekordboxPath", "enginePath", "isSdCard", "isFolder", "isBrowsedBackup", "libraryId",
        "safeToUnplug", "readOnly", "capacityBytes"]
    property var rows: []
    property bool rowsLoaded: false
    // The mount points that were mounted at the last copy, so a stick
    // is assessed when it appears mounted or gets mounted, and not again
    // on every other change to its row.
    property var mountedAtLastCopy: ({})

    // mountPoint when there is one, else devicePath: the key the rail
    // and the pane's stick section are named by.
    function keyOf(row) {
        return HomeModel.keyOf(row);
    }
    function stickKeys() {
        return root.rows.map((row) => root.keyOf(row));
    }
    function copyRow(model, i) {
        const source = HomeModel.rowAt(model, i);
        const copy = {};
        for (let r = 0; r < root.rowRoles.length; ++r) {
            copy[root.rowRoles[r]] = source[root.rowRoles[r]];
        }
        return copy;
    }
    function copyRows() {
        const model = root.sticksModel;
        const count = HomeModel.rowCount(model);
        const copies = [];
        for (let i = 0; i < count; ++i) {
            copies.push(root.copyRow(model, i));
        }
        return copies;
    }
    // Every row copied afresh: on load, and whenever rows come, go or move.
    function refreshRows() {
        root.applyRows(root.copyRows());
    }
    // Only rows first to last changed: those are copied again and every
    // other row keeps its object, so the selected stick's row stays the
    // one the pane holds when some other stick changes. Anything that
    // does not add up (a range outside the rows copied) copies them all.
    function refreshChangedRows(first, last) {
        const model = root.sticksModel;
        const count = HomeModel.rowCount(model);
        if (count !== root.rows.length || first < 0 || last < first || last >= count) {
            root.refreshRows();
            return;
        }
        const next = root.rows.slice();
        for (let i = first; i <= last; ++i) {
            next[i] = root.copyRow(model, i);
        }
        root.applyRows(next);
    }
    function applyRows(next) {
        const previous = root.rows;
        root.rows = next;
        root.assessNewlyMounted(next);
        root.reconcileSelection(previous, next);
        root.rowsLoaded = true;
    }
    // Every mounted stick is assessed, not only the selected one: which
    // stick an empty one could be cloned from, and which holds a newer
    // copy, depend on the other sticks' assessments. A browsed backup is
    // never a backup subject or peer: the advisor would offer it as the
    // newest clone source, and cloning from it targets its own archive.
    function assessNewlyMounted(next) {
        const mountedNow = {};
        for (let i = 0; i < next.length; ++i) {
            const row = next[i];
            const mountPoint = String(row.mountPoint || "");
            if (row.mounted !== true || mountPoint.length === 0 || row.isBrowsedBackup === true) {
                continue;
            }
            mountedNow[mountPoint] = true;
            if (root.mountedAtLastCopy[mountPoint] !== true) {
                root.backupAdvisor.assess(String(row.label || ""), mountPoint, String(row.rekordboxPath || ""),
                                          String(row.enginePath || ""));
            }
        }
        root.mountedAtLastCopy = mountedNow;
    }
    Component.onCompleted: {
        // Always one of the four: the settings refuse anything else, when
        // it is set and when it is read back.
        root.selectedGroup = root.appSettingsController.homeGroup;
        root.refreshRows();
    }
    onSticksModelChanged: {
        if (root.rowsLoaded) {
            root.refreshRows();
        }
    }
    Connections {
        // Only a model object says when it changes; a plain array is
        // copied again when it is replaced (onSticksModelChanged).
        target: root.sticksModel !== null && typeof root.sticksModel.get === "function" ? root.sticksModel : null
        ignoreUnknownSignals: true
        function onDataChanged(topLeft, bottomRight) {
            root.refreshChangedRows(topLeft.row, bottomRight.row);
        }
        function onRowsInserted() { root.refreshRows(); }
        function onRowsRemoved() { root.refreshRows(); }
        function onRowsMoved() { root.refreshRows(); }
        function onModelReset() { root.refreshRows(); }
    }

    // ---- selection --------------------------------------------------
    //
    // The stick is chosen afresh every run: on load the first stick with
    // a library, else the first stick. A stick that arrives while none is
    // selected becomes selected; the selected one leaving selects the
    // first that remains. Mounting or unmounting changes a stick's key
    // (devicePath and mountPoint), so the selection follows its device.
    property string selectedStickKey: ""
    property string selectedDevicePath: ""
    readonly property var selectedRow: {
        for (let i = 0; i < root.rows.length; ++i) {
            if (root.keyOf(root.rows[i]) === root.selectedStickKey) {
                return root.rows[i];
            }
        }
        return null;
    }
    // What the backup advisor found for the selected stick; null until
    // it has looked.
    readonly property var selectedAdvice: root.selectedRow !== null && root.backupAdvisor.advice
        ? (root.backupAdvisor.advice[String(root.selectedRow.mountPoint || "")] || null) : null

    function selectStick(key) {
        for (let i = 0; i < root.rows.length; ++i) {
            if (root.keyOf(root.rows[i]) === key) {
                root.selectedStickKey = key;
                root.selectedDevicePath = String(root.rows[i].devicePath || "");
                return true;
            }
        }
        return false;
    }
    function firstWithLibrary(rows) {
        for (let i = 0; i < rows.length; ++i) {
            if (rows[i].hasRekordbox === true || rows[i].hasEngine === true) {
                return rows[i];
            }
        }
        return rows.length > 0 ? rows[0] : null;
    }
    function reconcileSelection(previous, next) {
        if (next.length === 0) {
            root.selectedStickKey = "";
            root.selectedDevicePath = "";
            return;
        }
        if (root.selectedStickKey.length > 0) {
            if (root.selectStick(root.selectedStickKey)) {
                return;
            }
            // The same device under its other key: mounted or unmounted.
            if (root.selectedDevicePath.length > 0) {
                for (let i = 0; i < next.length; ++i) {
                    if (String(next[i].devicePath || "") === root.selectedDevicePath) {
                        root.selectStick(root.keyOf(next[i]));
                        return;
                    }
                }
            }
            // Gone: the first that remains.
            root.selectStick(root.keyOf(next[0]));
            return;
        }
        if (root.rowsLoaded) {
            // Nothing selected and a stick arrived: that one.
            const before = {};
            for (let i = 0; i < previous.length; ++i) {
                before[root.keyOf(previous[i])] = true;
            }
            for (let i = 0; i < next.length; ++i) {
                if (before[root.keyOf(next[i])] !== true) {
                    root.selectStick(root.keyOf(next[i]));
                    return;
                }
            }
        }
        root.selectStick(root.keyOf(root.firstWithLibrary(next)));
    }

    // The group is remembered across runs (AppSettingsController.homeGroup).
    // The groups themselves, and the rail's order, are HomeModel's.
    readonly property var groupKeys: HomeModel.groupKeys()
    property string selectedGroup: "explore"
    readonly property var selectedGroupInfo: HomeModel.group(root.selectedGroup)
    function selectGroup(group) {
        if (root.groupKeys.indexOf(group) < 0) {
            return;
        }
        root.selectedGroup = group;
        if (root.appSettingsController.homeGroup !== group) {
            root.appSettingsController.homeGroup = group;
        }
    }

    // The home's three forms, by the window's width (see Theme's
    // homeWideWidth and homeMediumWidth): "wide", the rail a column and
    // the cards two to a row; "medium", the rail a column and the cards
    // one to a row; "narrow", the rail as chips above the pane and the
    // cards one to a row. The stick's row and the group's heading are
    // the same in all three.
    readonly property string homeForm: root.width >= Theme.homeWideWidth ? "wide"
        : root.width >= Theme.homeMediumWidth ? "medium" : "narrow"
    readonly property bool compact: root.homeForm === "narrow"
    readonly property int cardColumns: root.homeForm === "wide" ? 2 : 1
    // Where the pane's text starts, from the pane's left edge: the cards'
    // titles, and with them the stick's name and the group heading (see
    // StickToolCards.textInset, which reads its cards' own).
    readonly property real paneTextInset: toolCards.textInset

    function isLockedByOther(libraryId) {
        return libraryId.length > 0 && root.editRegistry !== null && root.editRegistry !== undefined
            && root.editRegistry.lockedByOther.indexOf(libraryId) >= 0;
    }
    function explainLock(libraryId) {
        lockedDialog.openFor(libraryId, root.editRegistry ? root.editRegistry.lockHolder(libraryId) : {});
    }

    // Coming back from a backup or restore: the advice is stale.
    StackView.onActivated: {
        root.backupAdvisor.reassessAll();
        root.refreshLocks();
        // Coming back from having made a backup, or deleted the last
        // one: the menu's Browse Metadata Backups entry is stale until asked.
        homeBackups.refresh();
        // Back from a clean-up, which can leave files orphaned or clear
        // them: Maintain's counts are stale.
        toolCards.refreshMaintain();
    }
    // Another instance taking or dropping a lock shows up within 2 s
    // while this page is in front.
    Timer {
        interval: 2000
        repeat: true
        running: root.StackView.status === StackView.Active
        onTriggered: root.refreshLocks()
    }
    // Opening a library that is not on removable media: a restored stick
    // backup, a copy on an internal disk, a test fixture. The folder to
    // pick is the one *holding* PIONEER / Engine Library, which is what a
    // stick's own root looks like -- see MediaController::openFolder.
    FolderDialog {
        id: openFolderDialog
        objectName: "openFolderDialog"
        title: "Open a folder holding a rekordbox or Engine DJ library"
        onAccepted: {
            if (!root.releaseOpenedFolder()) {
                return;
            }
            // Handed over as the URL it is; the controller converts it
            // with QUrl::toLocalFile (gui/local_file_url.hpp, localPathFromUrl).
            var message = root.mediaController.openFolder(selectedFolder.toString());
            if (message.length > 0) {
                openFolderError.text = message;
                openFolderError.open();
            }
        }
    }
    // Browsing a full stick backup without unpacking it: only the
    // catalogs are extracted (see MediaController::openBackup), the
    // analysis files stay in the archive and are read per track.
    FileDialog {
        id: openBackupDialog
        objectName: "openBackupDialog"
        title: "Open a full stick backup to browse"
        nameFilters: ["Stick backups (*.zip)", "All files (*)"]
        currentFolder: root.appSettingsController.toLocalFileUrl(root.appSettingsController.stickBackupDirectory)
        onAccepted: root.openBackupArchive(selectedFile.toString())
    }
    // Also Manage Backups' Browse, which comes back here to do it. A
    // backup is opened to look at its library, so it goes straight on into
    // Browse Library; its row stays here for the other read-only cards.
    function openBackupArchive(archivePath) {
        if (!root.releaseOpenedFolder()) {
            return;
        }
        var message = root.mediaController.openBackup(archivePath);
        if (message.length > 0) {
            openFolderError.text = message;
            openFolderError.open();
            return;
        }
        // Listed by the time openBackup returns: opening re-detects before
        // it does. Only one backup or folder is open at a time.
        var row = root.browsedBackupRow();
        if (row && (row.hasRekordbox || row.hasEngine)) {
            root.browseRequested(row.label, row.rekordboxPath, row.enginePath);
        }
    }
    function browsedBackupRow() {
        const model = root.sticksModel;
        const count = HomeModel.rowCount(model);
        for (let i = 0; i < count; ++i) {
            const row = HomeModel.rowAt(model, i);
            if (row.isBrowsedBackup) {
                return row;
            }
        }
        return null;
    }
    Dialog {
        id: openFolderError
        objectName: "openFolderError"
        property alias text: openFolderErrorLabel.text
        anchors.centerIn: Overlay.overlay
        // Explicit, so the dialog's implicit width never has to be derived
        // from content that is itself sized from the dialog -- the loop
        // Qt reports as "Binding loop detected for implicitWidth".
        width: 460
        modal: true
        title: "Cannot open that folder"
        standardButtons: Dialog.Ok
        Label {
            id: openFolderErrorLabel
            width: parent.width
            wrapMode: Text.WordWrap
        }
    }
    // The counts behind the menu's backup entries, and nothing
    // else. Both are probes that open nothing and create nothing -- see
    // HomeBackupsController.
    HomeBackupsController {
        id: homeBackups
        backupDirectory: root.appSettingsController.stickBackupDirectory
    }

    LockedLibraryDialog {
        id: lockedDialog
        objectName: "lockedDialog"
        onRemoveLockRequested: {
            if (root.editRegistry) {
                root.editRegistry.removeLock(lockedDialog.libraryId);
            }
        }
    }
    signal browseRequested(string stickLabel, string rekordboxPath, string enginePath)
    signal playlistDiffRequested(string stickLabel, string rekordboxPath, string enginePath)
    // Every full stick backup on this computer. From a stick's Backup
    // group, with its label and the backup the advisor matched to it
    // (listed first); from the no-stick pane, both empty.
    signal manageBackupsRequested(string stickLabel, string currentArchivePath)
    // Maintain's clean-up cards. They sat behind a Housekeeping page of
    // their own; the group is that page now (Sebastian, 2026-10-06), as
    // the Backup group took over the backups hub's cards before.
    signal duplicatesStatsRequested(string stickLabel, string rekordboxPath, string enginePath)
    signal cleanupRequested(string stickLabel, string rekordboxPath, string enginePath)
    signal pendingDeletionsRequested(string stickLabel, string rekordboxPath, string enginePath)
    signal junkCueCleanupRequested(string stickLabel, string rekordboxPath, string enginePath)
    signal recordingsRequested(string stickLabel, string rekordboxPath, string enginePath)
    signal libraryHealthRequested(string stickLabel, string rekordboxPath, string enginePath)
    signal stickStatisticsRequested(string stickLabel, string rekordboxPath, string enginePath)
    signal stickPerformanceRequested(string stickLabel, string rekordboxPath, string enginePath, string mountPoint)
    signal engineLibraryCreatorRequested(string stickLabel, string rekordboxPath)
    signal settingsRequested(string stickLabel, string pioneerRoot)
    signal syncRequested(string stickLabel, string rekordboxPath, string enginePath)
    signal appSettingsRequested()
    signal fullStickBackupRequested(string stickLabel, string rekordboxPath, string enginePath)
    signal aboutRequested()
    signal donationRequested()
    signal formatUsbRequested()
    // mountPoint (or, for a not-yet-mounted stick, devicePath) preselects
    // the target drive; both empty means "pick one there". archivePath
    // preselects the backup (the advisor's pick), empty picks the newest.
    // stickLabel is the row's name for the restore page's breadcrumb,
    // empty from the general card that is not about any one stick.
    signal restoreStickBackupRequested(string mountPoint, string devicePath, string archivePath,
                                       string stickLabel)
    // General entry point (Backups block above the stick list): not
    // tied to any particular stick, so mountPoint/devicePath/archivePath
    // (or stickLabel/rekordboxPath/enginePath) may all be empty; the
    // target page picks its own drive/backup/library from within itself.
    signal metadataBackupRequested(string stickLabel, string rekordboxPath, string enginePath, string libraryId)
    signal metadataRestoreRequested(string stickLabel, string rekordboxPath, string enginePath, string libraryId)
    // Copy the library on another mounted stick onto this one -- either a
    // fresh backup stick (targetHasLibrary false) or an update of an older
    // copy (true). The source's catalog paths come from the advisor.
    signal cloneStickRequested(string sourceLabel, string sourceRekordboxPath, string sourceEnginePath,
                               string targetMountPoint, string targetLabel, bool targetHasLibrary)

    // The brand watermark that used to sit in this corner is gone: the
    // header now carries "Seabass / Your DJ Toolbox" as the page's own
    // title, and the same two words twice on one screen read as a
    // mistake rather than as branding. The window still has the Sound
    // Bass mark behind everything (Main.qml's WatermarkLayer).

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.pageMargin
        spacing: Theme.sectionSpacing

        RowLayout {
            Layout.fillWidth: true
            // As tall as the other pages' Back button: the window's header
            // row centres on that height on every page.
            Layout.minimumHeight: Theme.headerBackButtonSize

            // The app's own name where every other page has a page
            // title, because this page's subject IS the app: it is the
            // one screen you arrive at rather than navigate to, and
            // "Home" named the position rather than the thing.
            // The name and the slogan on one line, sharing a baseline: one
            // mark rather than a title with a line under it. The name is
            // large and in the regular weight, a wordmark rather than a
            // heading shouting over the page.
            RowLayout {
                objectName: "brandLockup"
                spacing: Theme.rowSpacing
                // On the rail's text line, where its section labels and
                // the entries' icons start, not on its pills' edge: the
                // pills reach crumbTextInset left of that line so the
                // accent bar and the shading have somewhere to be, and a
                // wordmark standing on the bar read as misaligned with
                // every word under it (Sebastian, 2026-09-28).
                Layout.leftMargin: Theme.crumbTextInset
                // Same two colours AboutPage gives this pair, which is
                // the only other place the lockup appears: the Kelp
                // palette's text, and its accent lightened.
                Label {
                    objectName: "brandName"
                    text: "Seabass"
                    font.family: Theme.titleFamily
                    font.weight: Font.Normal
                    font.pointSize: Theme.titleLarge
                    color: Theme.text
                    Layout.alignment: Qt.AlignBaseline
                }
                Label {
                    objectName: "brandSlogan"
                    text: "Your DJ Toolbox"
                    font.family: Theme.titleFamily
                    font.weight: Theme.titleWeight
                    // A clear step down from the name beside it: at the
                    // same size the pair read as two competing titles.
                    font.pointSize: Theme.subtitleSize
                    color: Qt.lighter(Theme.accent, 1.3)
                    Layout.alignment: Qt.AlignBaseline
                }
            }
            Item { Layout.fillWidth: true }

            // Room for the window's header row (AppHeaderOverlay), which
            // lies over this header and sets this width itself.
            Item {
                objectName: "appHeaderSpace"
                Layout.preferredWidth: root.appHeaderReserve
                Layout.minimumWidth: root.appHeaderReserve
            }
        }

        Rectangle {
            id: updateBanner
            objectName: "updateBanner"
            readonly property bool alarming: root.updateChecker !== null
                && root.updateChecker.runningWithdrawn
            readonly property bool newer: root.updateChecker !== null
                && root.updateChecker.updateAvailable
            visible: alarming || newer
            Layout.fillWidth: true
            implicitHeight: visible ? bannerColumn.implicitHeight + 2 * Theme.cardPadding : 0
            radius: Theme.cornerRadius
            // Opaque, and from the Theme's own surface: the text inside is
            // Theme ink, and a see-through tint would put it on whatever
            // ground the style paints, where it can vanish.
            color: alarming ? Theme.dangerBg
                            : Qt.tint(Theme.surface, Qt.rgba(Theme.good.r, Theme.good.g, Theme.good.b, 0.14))
            border.width: 1
            border.color: alarming ? Theme.dangerBorder : Theme.good

            readonly property string linkText:
                "<a href=\"" + (root.updateChecker !== null ? root.updateChecker.downloadPage : "") + "\">vizzzion.org/seabass</a>"

            ColumnLayout {
                id: bannerColumn
                anchors.fill: parent
                anchors.margins: Theme.cardPadding
                spacing: Theme.tightSpacing

                Label {
                    objectName: "updateBannerTitle"
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    font.weight: Theme.cardTitleWeight
                    font.pointSize: Theme.cardTitleSize
                    color: updateBanner.alarming ? Theme.dangerText : Theme.text
                    text: root.updateChecker === null ? ""
                        : updateBanner.alarming
                            ? "Seabass " + root.updateChecker.currentVersion + " has been withdrawn."
                            : "Seabass " + root.updateChecker.latestVersion + " has been released!"
                }
                // What the website wants said about it: the reason a build
                // was withdrawn, or a note on the new one ("fixes a bug that
                // could lose cues"). Nothing at all when there is none. Plain
                // text, deliberately: it is the website's text, and a Label's
                // default guesses at markup, so a note with an angle bracket
                // in it could format the home page.
                Label {
                    objectName: "updateBannerNote"
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    textFormat: Text.PlainText
                    visible: text.length > 0
                    color: updateBanner.alarming ? Theme.dangerText
                        : root.updateChecker !== null && root.updateChecker.latestNoteLevel === "warning"
                            ? Theme.conflictText : Theme.text
                    text: root.updateChecker === null ? ""
                        : updateBanner.alarming ? root.updateChecker.runningWithdrawnReason
                        : root.updateChecker.latestNote
                }
                Label {
                    id: updateBannerLink
                    objectName: "updateBannerLink"
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    textFormat: Text.StyledText
                    linkColor: Theme.accent
                    color: updateBanner.alarming ? Theme.dangerText : Theme.text
                    // Withdrawn with a newer release to move to names it:
                    // the title is about the build being used, so this line
                    // is the only place that says what to get instead.
                    text: root.updateChecker === null ? ""
                        : updateBanner.newer && updateBanner.alarming
                            ? "Download Seabass " + root.updateChecker.latestVersion + " from "
                              + updateBanner.linkText + "."
                        : updateBanner.newer
                            ? "Download the new version from " + updateBanner.linkText + "."
                            : "There is no newer release yet. Watch " + updateBanner.linkText + "."
                    onLinkActivated: link => Qt.openUrlExternally(link)
                    HoverHandler {
                        cursorShape: updateBannerLink.hoveredLink.length > 0 ? Qt.PointingHandCursor : Qt.ArrowCursor
                    }
                }
            }
        }
        Label {
            visible: root.mediaController.errorMessage.length > 0
            text: root.mediaController.errorMessage
            color: Theme.danger
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        // The rail beside the pane: which stick and which kind of tool
        // down the left, and on the right the selected stick's row, the
        // selected group's heading and that group's cards. In the medium
        // form the cards go one to a row; in the narrow one the rail also
        // becomes a grid of chips above the pane.
        GridLayout {
            objectName: "homeBody"
            Layout.fillWidth: true
            Layout.fillHeight: true
            columns: root.compact ? 1 : 2
            columnSpacing: Theme.pageMargin
            rowSpacing: Theme.sectionSpacing

            // First in item order, so Tab reaches the rail's two
            // sections before the cards.
            HomeRail {
                id: rail
                // The model itself, handed over once: the rail's own
                // Repeater follows its inserts and removes.
                sticks: root.sticksModel
                selectedStickKey: root.selectedStickKey
                selectedGroup: root.selectedGroup
                compact: root.compact
                Layout.alignment: Qt.AlignTop
                Layout.fillWidth: root.compact
                Layout.preferredWidth: root.compact ? -1 : implicitWidth
                onStickActivated: (key) => root.selectStick(key)
                onStickEjectRequested: (devicePath) => {
                    // As the header's eject button does: stop first, an
                    // open handle on the playing track would hold the stick.
                    if (root.playbackController) {
                        root.playbackController.stop();
                    }
                    root.mediaController.unmountStick(devicePath);
                }
                onGroupActivated: (group) => root.selectGroup(group)
            }

            Flickable {
                id: pane
                objectName: "homePane"
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                contentWidth: width
                // Room inside the clip for a card's focus frame (Theme.focusRoom).
                contentHeight: paneColumn.implicitHeight + 2 * Theme.focusRoom
                // Not draggable when everything already fits.
                interactive: contentHeight > height
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {
                    policy: pane.interactive ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff
                }

                ColumnLayout {
                    id: paneColumn
                    x: Theme.focusRoom
                    y: Theme.focusRoom
                    width: pane.width - 2 * Theme.focusRoom
                    spacing: Theme.sectionSpacing

                    // The selected stick: its row, the group's heading and
                    // the group's cards. Named for the stick, as each row
                    // of the old list was, so the tests and the live
                    // helpers reach its cards under it (LiveHelpers.js).
                    ColumnLayout {
                        id: stickSection
                        objectName: "stickRow:" + root.selectedStickKey
                        visible: root.selectedRow !== null
                        Layout.fillWidth: true
                        spacing: Theme.sectionSpacing
                        // What the old delegate carried, for the tests.
                        readonly property string label: root.selectedRow ? String(root.selectedRow.label || "") : ""
                        readonly property string mountPoint: root.selectedRow ? String(root.selectedRow.mountPoint || "") : ""
                        readonly property string devicePath: root.selectedRow ? String(root.selectedRow.devicePath || "") : ""

                        StickHeaderRow {
                            id: stickHeader
                            objectName: "stickHeader"
                            Layout.fillWidth: true
                            row: root.selectedRow
                            mediaController: root.mediaController
                            playbackController: root.playbackController
                            advice: root.selectedAdvice
                            adviceState: root.selectedAdvice ? String(root.selectedAdvice.state || "") : ""
                            cloneSource: root.selectedAdvice && root.selectedAdvice.cloneSource
                                && root.selectedAdvice.cloneSource.kind === "stick"
                                ? root.selectedAdvice.cloneSource : null
                            onCloseFolderRequested: (mountPoint) => {
                                if (root.releaseOpenedFolder()) {
                                    root.mediaController.closeFolder(mountPoint);
                                }
                            }
                            onSyncRequested: (stickLabel, rekordboxPath, enginePath) =>
                                root.syncRequested(stickLabel, rekordboxPath, enginePath)
                        }

                        GroupHeading {
                            groupName: root.selectedGroupInfo.name
                            groupDescription: root.selectedGroupInfo.description
                            textInset: root.paneTextInset
                        }

                        StickToolCards {
                            id: toolCards
                            Layout.fillWidth: true
                            row: root.selectedRow
                            group: root.selectedGroup
                            mediaController: root.mediaController
                            appSettingsController: root.appSettingsController
                            backupAdvisor: root.backupAdvisor
                            editRegistry: root.editRegistry
                            columns: root.cardColumns
                            onBrowseRequested: (stickLabel, rekordboxPath, enginePath) =>
                                root.browseRequested(stickLabel, rekordboxPath, enginePath)
                            onPlaylistDiffRequested: (stickLabel, rekordboxPath, enginePath) =>
                                root.playlistDiffRequested(stickLabel, rekordboxPath, enginePath)
                            onDuplicatesStatsRequested: (stickLabel, rekordboxPath, enginePath) =>
                                root.duplicatesStatsRequested(stickLabel, rekordboxPath, enginePath)
                            onCleanupRequested: (stickLabel, rekordboxPath, enginePath) =>
                                root.cleanupRequested(stickLabel, rekordboxPath, enginePath)
                            onPendingDeletionsRequested: (stickLabel, rekordboxPath, enginePath) =>
                                root.pendingDeletionsRequested(stickLabel, rekordboxPath, enginePath)
                            onJunkCueCleanupRequested: (stickLabel, rekordboxPath, enginePath) =>
                                root.junkCueCleanupRequested(stickLabel, rekordboxPath, enginePath)
                            onRecordingsRequested: (stickLabel, rekordboxPath, enginePath) =>
                                root.recordingsRequested(stickLabel, rekordboxPath, enginePath)
                            onLibraryHealthRequested: (stickLabel, rekordboxPath, enginePath) =>
                                root.libraryHealthRequested(stickLabel, rekordboxPath, enginePath)
                            onStickStatisticsRequested: (stickLabel, rekordboxPath, enginePath) =>
                                root.stickStatisticsRequested(stickLabel, rekordboxPath, enginePath)
                            onStickPerformanceRequested: (stickLabel, rekordboxPath, enginePath, mountPoint) =>
                                root.stickPerformanceRequested(stickLabel, rekordboxPath, enginePath, mountPoint)
                            onEngineLibraryCreatorRequested: (stickLabel, rekordboxPath) =>
                                root.engineLibraryCreatorRequested(stickLabel, rekordboxPath)
                            onSettingsRequested: (stickLabel, pioneerRoot) => root.settingsRequested(stickLabel, pioneerRoot)
                            onSyncRequested: (stickLabel, rekordboxPath, enginePath) =>
                                root.syncRequested(stickLabel, rekordboxPath, enginePath)
                            onFullStickBackupRequested: (stickLabel, rekordboxPath, enginePath) =>
                                root.fullStickBackupRequested(stickLabel, rekordboxPath, enginePath)
                            onManageBackupsRequested: (stickLabel, currentArchivePath) =>
                                root.manageBackupsRequested(stickLabel, currentArchivePath)
                            onFormatUsbRequested: root.formatUsbRequested()
                            onRestoreStickBackupRequested: (mountPoint, devicePath, archivePath, stickLabel) =>
                                root.restoreStickBackupRequested(mountPoint, devicePath, archivePath, stickLabel)
                            onMetadataBackupRequested: (stickLabel, rekordboxPath, enginePath, libraryId) =>
                                root.metadataBackupRequested(stickLabel, rekordboxPath, enginePath, libraryId)
                            onMetadataRestoreRequested: (stickLabel, rekordboxPath, enginePath, libraryId) =>
                                root.metadataRestoreRequested(stickLabel, rekordboxPath, enginePath, libraryId)
                            onCloneStickRequested: (sourceLabel, sourceRekordboxPath, sourceEnginePath,
                                                    targetMountPoint, targetLabel, targetHasLibrary) =>
                                root.cloneStickRequested(sourceLabel, sourceRekordboxPath, sourceEnginePath,
                                                         targetMountPoint, targetLabel, targetHasLibrary)
                            onExplainLockRequested: (libraryId) => root.explainLock(libraryId)
                        }
                    }

                    // No stick and no folder: the group's heading all the
                    // same, and what can be done on this computer alone.
                    GroupHeading {
                        objectName: "noStickHeading"
                        visible: root.selectedRow === null
                        groupName: root.selectedGroupInfo.name
                        groupDescription: root.selectedGroupInfo.description
                        textInset: root.paneTextInset
                    }
                    // The whole pane's message while there is nothing to
                    // work on, so it reads as one: centred, the stick's own
                    // icon at size, and what to do about it underneath.
                    ColumnLayout {
                        objectName: "noStickNothingHere"
                        visible: root.selectedRow === null && root.selectedGroup !== "backup"
                        Layout.fillWidth: true
                        Layout.topMargin: Theme.sectionSpacing * 3
                        spacing: Theme.sectionSpacing

                        UsbStickIcon {
                            objectName: "noStickIcon"
                            Layout.alignment: Qt.AlignHCenter
                            size: Theme.iconSizeLarge * 1.5
                        }
                        Label {
                            objectName: "noStickTitle"
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignHCenter
                            wrapMode: Text.WordWrap
                            text: "No USB stick"
                            color: Theme.text
                            font.pointSize: Theme.fontLarge
                            font.bold: true
                        }
                        Label {
                            objectName: "noStickHint"
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignHCenter
                            wrapMode: Text.WordWrap
                            text: "Nothing here without a USB stick. Insert one to get started."
                            color: Theme.textMuted
                            font.pointSize: Theme.subtitleSize
                        }
                    }

                    // Tools that work on this computer's own backup stores,
                    // not on whatever stick happens to be plugged in: the
                    // Backup group's cards while no stick is. The header
                    // menu offers the same, and a card here says so
                    // plainly on a screen that would otherwise be empty.
                    //
                    // removableCount, not the row count: a folder someone
                    // opened is not a stick, and should not make these
                    // vanish.
                    //
                    // Restore a Stick Backup is not among them: it needs a
                    // drive to restore onto, and this block is only ever
                    // shown when there is none -- the card led to a page
                    // with nothing to pick (Sebastian, 2026-09-28). With a
                    // stick in, its own Backup cards offer the restore.
                    ColumnLayout {
                        objectName: "noStickBackupTools"
                        Layout.fillWidth: true
                        visible: root.removableStickCount === 0 && root.selectedGroup === "backup"
                        spacing: Theme.tightSpacing

                        // Only beside an opened folder's own cards, where
                        // it says these are not about that folder.
                        TableHeaderLabel {
                            objectName: "noStickToolsLabel"
                            visible: root.selectedRow !== null
                            label: "On this computer"
                            leftPadding: root.paneTextInset
                        }

                        GridLayout {
                            id: computerGrid
                            Layout.fillWidth: true
                            columns: root.cardColumns
                            columnSpacing: Theme.rowSpacing
                            rowSpacing: Theme.rowSpacing
                            // Every column the same width, as the stick's
                            // cards above have it.
                            readonly property real cellWidth: Math.max(0, (paneColumn.width
                                - (computerGrid.columns - 1) * computerGrid.columnSpacing) / computerGrid.columns)

                            ActionCard {
                                objectName: "browseFullBackupCard"
                                large: true
                                Layout.preferredWidth: computerGrid.cellWidth
                                Layout.fillHeight: true
                                cardTitle: "Browse a Full Stick Backup"
                                cardSubtitle: "Look at a backup's library without restoring it"
                                cardIcon: "backup"
                                onClicked: openBackupDialog.open()
                            }
                            ActionCard {
                                objectName: "manageBackupsCard"
                                large: true
                                Layout.preferredWidth: computerGrid.cellWidth
                                Layout.fillHeight: true
                                cardTitle: "Manage Full Stick Backups"
                                cardSubtitle: homeBackups.fullBackupCount > 0
                                    ? "See, browse and delete the stick backups on this computer"
                                    : "None yet"
                                cardIcon: "deep-history"
                                enabled: homeBackups.fullBackupCount > 0
                                onClicked: root.manageBackupsRequested("", "")
                            }
                            ActionCard {
                                objectName: "browseMetadataBackupsCard"
                                large: true
                                Layout.preferredWidth: computerGrid.cellWidth
                                Layout.fillHeight: true
                                cardTitle: "Browse Metadata Backups"
                                cardSubtitle: homeBackups.metadataTrackCount > 0
                                    ? "The cues, ratings and comments kept on this computer"
                                    : "None yet"
                                cardIcon: "view-list-details"
                                // Off rather than missing while the store is
                                // empty: the card says the feature exists.
                                enabled: homeBackups.metadataTrackCount > 0
                                // No stick: the page opens on its browse half.
                                onClicked: root.metadataBackupRequested("", "", "", "")
                            }
                        }
                    }
                }
            }
        }
    }

    // The selected group's name, and beside it what the group is for,
    // on the pane's text line: the stick's name above it and the cards'
    // titles below start at the same x. An inline component sees none of
    // this file's ids, so what it shows is handed in.
    component GroupHeading: RowLayout {
        id: heading
        property string groupName
        property string groupDescription
        property real textInset: 0
        objectName: "groupHeading"
        Layout.fillWidth: true
        Layout.leftMargin: textInset
        Layout.topMargin: Theme.tightSpacing
        spacing: Theme.rowSpacing
        Label {
            objectName: "groupHeadingName"
            text: heading.groupName
            font.family: Theme.titleFamily
            font.weight: Font.DemiBold
            font.pointSize: Theme.cardTitleSize * 1.1
            color: Theme.text
            Layout.alignment: Qt.AlignBaseline
        }
        Label {
            objectName: "groupHeadingDescription"
            text: heading.groupDescription
            color: Theme.textMuted
            elide: Text.ElideRight
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            Layout.alignment: Qt.AlignBaseline
        }
    }
}
