// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import SeabassGui

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
        var model = root.mediaController.sticks;
        if (model === null || model === undefined) {
            return true;
        }
        // A plain array in the tests, the real list model otherwise.
        var count = model.length !== undefined ? model.length : model.rowCount();
        for (var i = 0; i < count; ++i) {
            var row = model.length !== undefined ? model[i] : model.get(i);
            if (!row.isFolder) {
                continue;
            }
            var reg = root.editRegistry;
            if (reg && reg.hasSession(row.libraryId)) {
                var session = reg.sessionFor(row.libraryId);
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
    // The model answers this itself; the loop is for the QML tests,
    // which stand a plain array of stick objects in for the model and
    // have no such property. Same shape as the editRegistry guard above:
    // this page is built to run against fakes.
    readonly property int removableStickCount: {
        var model = root.mediaController.sticks;
        if (model === null || model === undefined) {
            return 0;
        }
        if (model.removableCount !== undefined) {
            return model.removableCount;
        }
        var count = 0;
        for (var i = 0; i < (model.length || 0); ++i) {
            if (!model[i].isFolder) {
                count++;
            }
        }
        return count;
    }

    // The metadata store's count, readable by a test that wants to check
    // the menu entry's enabled state against the thing that decides it
    // rather than against a hardcoded expectation of what the machine holds.
    readonly property int homeBackupsMetadataCount: homeBackups.metadataTrackCount
    readonly property int homeBackupsFullCount: homeBackups.fullBackupCount

    // One size for every icon in the header, the menu's included. Set, not
    // read off the menu button: KDE's ToolButton never sets icon.width, so
    // binding to it gave 0 there and the heart filled its whole button.
    readonly property int headerIconSize: Math.round(22 * Theme.iconScale)

    // The narrowest a stick's action card is laid out at: the grid under
    // each stick drops from three columns to two, then one, rather than
    // squeezing three cards into a window that has room for fewer.
    readonly property real minimumCardWidth: 300 * Theme.iconScale

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
        var model = root.mediaController.sticks;
        if (model === null || model === undefined) {
            return null;
        }
        // A plain array in the tests, the real list model otherwise.
        var count = model.length !== undefined ? model.length : model.rowCount();
        for (var i = 0; i < count; ++i) {
            var row = model.length !== undefined ? model[i] : model.get(i);
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
    signal manageBackupsRequested()
    // Deduplication and Backups are hub pages now (see
    // DuplicatesHubPage.qml / BackupsHubPage.qml), each fanning out to two
    // sub-pages that used to be separate top-level cards here
    // (Deduplication + Clean Up Duplicates; Local Cue Backup + Manage
    // Backups). Library Health used to be a card inside the Deduplication
    // hub too, but it isn't a duplicate-tracks concern (it spans all three
    // catalogs looking for missing files, not just consolidating copies),
    // so it got promoted to its own top-level entry instead.
    signal duplicateTracksHubRequested(string stickLabel, string rekordboxPath, string enginePath)
    signal libraryHealthRequested(string stickLabel, string rekordboxPath, string enginePath)
    signal stickStatisticsRequested(string stickLabel, string rekordboxPath, string enginePath)
    signal stickPerformanceRequested(string stickLabel, string rekordboxPath, string enginePath, string mountPoint)
    signal engineLibraryCreatorRequested(string stickLabel, string rekordboxPath)
    signal settingsRequested(string stickLabel, string pioneerRoot)
    signal syncRequested(string stickLabel, string rekordboxPath, string enginePath)
    signal appSettingsRequested()
    signal backupsHubRequested(string stickLabel, string rekordboxPath, string enginePath, string mountPoint, string devicePath)
    signal aboutRequested()
    signal donationRequested()
    signal formatUsbRequested()
    // mountPoint (or, for a not-yet-mounted stick, devicePath) preselects
    // the target drive; both empty means "pick one there". archivePath
    // preselects the backup (the advisor's pick), empty picks the newest.
    signal restoreStickBackupRequested(string mountPoint, string devicePath, string archivePath)
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
        anchors.margins: 16
        spacing: 12

        RowLayout {
            Layout.fillWidth: true

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
            // What opens a library from this computer rather than from a
            // stick, behind one button: each is used now and then, and as
            // two header buttons plus a row under the list they crowded a
            // page whose subject is the sticks.
            ToolButton {
                id: homeMenuButton
                objectName: "homeMenuButton"
                icon.source: Theme.iconUrl("application-menu")
                icon.color: Theme.text
                icon.width: root.headerIconSize
                icon.height: root.headerIconSize
                display: AbstractButton.IconOnly
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
                    MenuItem {
                        objectName: "browseFullBackupItem"
                        text: "Browse a Full Stick Backup…"
                        icon.source: Theme.iconUrl("backup")
                        icon.color: enabled ? Theme.text : Theme.textMuted
                        onTriggered: openBackupDialog.open()
                    }
                    MenuItem {
                        objectName: "manageBackupsItem"
                        text: homeBackups.fullBackupCount > 0 ? "Manage Full Stick Backups…"
                                                              : "Manage Full Stick Backups (none yet)"
                        icon.source: Theme.iconUrl("deep-history")
                        icon.color: enabled ? Theme.text : Theme.textMuted
                        enabled: homeBackups.fullBackupCount > 0
                        onTriggered: root.manageBackupsRequested()
                    }
                    MenuItem {
                        objectName: "browseMetadataBackupsItem"
                        // Off rather than missing while the store is empty:
                        // the entry says the feature exists.
                        text: homeBackups.metadataTrackCount > 0 ? "Browse Metadata Backups"
                                                                 : "Browse Metadata Backups (none yet)"
                        icon.source: Theme.iconUrl("view-list-details")
                        icon.color: enabled ? Theme.text : Theme.textMuted
                        enabled: homeBackups.metadataTrackCount > 0
                        // No stick: the page opens on its browse half.
                        onTriggered: root.metadataBackupRequested("", "", "", "")
                    }
                    MenuItem {
                        objectName: "openFolderItem"
                        text: "Open a Library From a Folder…"
                        icon.source: Theme.iconUrl("folder-open")
                        icon.color: enabled ? Theme.text : Theme.textMuted
                        onTriggered: openFolderDialog.open()
                    }
                }
            }
            // Bundled Breeze icons, flat in the text colour like every
            // other icon in the app (see SeabassIcon): a theme lookup
            // finds nothing on Windows or macOS, and colour icons next to
            // flat ones read as two different sets. Sized like the menu
            // icon beside them. The heart after them stays red: it is
            // the one button asking for something.
            //
            // text is set on each despite IconOnly: it never renders,
            // and it is what an assistive reader announces.
            ToolButton {
                objectName: "aboutButton"
                icon.source: Theme.iconUrl("help-about")
                icon.color: Theme.text
                icon.width: root.headerIconSize
                icon.height: root.headerIconSize
                display: AbstractButton.IconOnly
                text: "About Seabass"
                ToolTip.visible: hovered
                ToolTip.text: "About Seabass"
                onClicked: root.aboutRequested()
            }
            ToolButton {
                objectName: "preferencesButton"
                icon.source: Theme.iconUrl("configure")
                icon.color: Theme.text
                icon.width: root.headerIconSize
                icon.height: root.headerIconSize
                display: AbstractButton.IconOnly
                text: "Preferences"
                ToolTip.visible: hovered
                ToolTip.text: "Preferences"
                onClicked: root.appSettingsRequested()
            }
            ToolButton {
                id: donateButton
                objectName: "donateButton"
                // The only button in the header asking for something
                // rather than offering something.
                //
                // A filled heart, drawn. Breeze's own "love" is an outline,
                // and a red outline still reads as a grey button with a red
                // edge. HeartIcon fills that icon's outer contour.
                contentItem: HeartIcon {
                    objectName: "donateHeart"
                    iconSize: root.headerIconSize
                    color: "#aa0000"
                }
                display: AbstractButton.IconOnly
                text: "Support Seabass"
                ToolTip.visible: hovered
                ToolTip.text: "Support Seabass"
                onClicked: root.donationRequested()

                // A slight, infrequent heartbeat -- a soft "lub-dub"
                // every six seconds or so, not a continuous throb -- so
                // it reads as a subtle living detail rather than a
                // distracting animated icon. Two small swells with a
                // slight brightening on each, on sine curves (an organic
                // rise and settle, no snap), the second a touch weaker,
                // then a long rest. Drives scale and opacity directly
                // rather than through a Behavior, which would otherwise
                // re-trigger on every intermediate value this same
                // animation produces.
                SequentialAnimation {
                    running: true
                    loops: Animation.Infinite
                    ParallelAnimation {
                        NumberAnimation { target: donateButton; property: "scale"; to: 1.12; duration: 260; easing.type: Easing.OutSine }
                        NumberAnimation { target: donateButton; property: "opacity"; to: 1.0; duration: 260; easing.type: Easing.OutSine }
                    }
                    ParallelAnimation {
                        NumberAnimation { target: donateButton; property: "scale"; to: 1.0; duration: 340; easing.type: Easing.InOutSine }
                        NumberAnimation { target: donateButton; property: "opacity"; to: 0.85; duration: 340; easing.type: Easing.InOutSine }
                    }
                    PauseAnimation { duration: 90 }
                    ParallelAnimation {
                        NumberAnimation { target: donateButton; property: "scale"; to: 1.07; duration: 220; easing.type: Easing.OutSine }
                        NumberAnimation { target: donateButton; property: "opacity"; to: 1.0; duration: 220; easing.type: Easing.OutSine }
                    }
                    ParallelAnimation {
                        NumberAnimation { target: donateButton; property: "scale"; to: 1.0; duration: 520; easing.type: Easing.InOutSine }
                        NumberAnimation { target: donateButton; property: "opacity"; to: 0.85; duration: 520; easing.type: Easing.InOutSine }
                    }
                    PauseAnimation { duration: 4800 }
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

        // Tools that work on this computer's own backup stores, not on
        // whatever stick happens to be plugged in right now -- pulled out
        // of the per-stick Backups hub for exactly that reason (see
        // BackupsHubPage.qml's own comment for what stays there because
        // it genuinely does need a specific stick).
        //
        // Shown only while no stick is plugged in. With one in, the
        // sticks are what this page is about and this card sits above
        // them taking the top of the screen for the case that is not
        // happening. (Local Cue Backup used to sit beside it; it was
        // removed once Metadata Backup and Restore covered the same need.)
        //
        // removableCount, not the row count: a folder someone opened is
        // not a stick, and should not make the no-stick tools vanish.
        ColumnLayout {
            objectName: "noStickBackupTools"
            Layout.fillWidth: true
            visible: root.removableStickCount === 0
            spacing: 8

            Subtitle { text: "Backups"; color: Theme.textMuted }

            GridLayout {
                Layout.fillWidth: true
                columns: 2
                columnSpacing: 12
                rowSpacing: 12

                ActionCard {
                    objectName: "generalRestoreCard"
                    cardTitle: "Restore a Stick Backup"
                    cardSubtitle: "Put a stick backup from this computer onto any drive"
                    cardIcon: "document-revert"
                    // No stick preselected: the page itself lists every
                    // mounted drive and every backup on disk to choose from.
                    onClicked: root.restoreStickBackupRequested("", "", "")
                }
            }
        }

        ListView {
            // Named for the tests, which reach rows and cards through these
            // rather than by guessing at the delegate's properties.
            objectName: "stickList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            model: root.mediaController.sticks
            clip: true
            // Not draggable when every stick already fits.
            interactive: contentHeight > height
            spacing: 4

            // A stick appearing is the one event on this page the user
            // did not cause from the screen -- they caused it at the USB
            // port, and they are usually looking at the port rather than
            // the list. So the card arrives visibly: it fades and grows
            // into place while the cards below it slide down to make
            // room, which is what tells the eye WHERE it went as well as
            // that it came. The model reports an insert now rather than
            // resetting itself, which is what makes any of this possible
            // (see DetectedStickListModel::setSticks).
            add: Transition {
                // Grows past its size and settles back, the way a thing
                // put down on a table does. A straight ease-in stops
                // dead on arrival and reads as a redraw; the small
                // overshoot is what makes it read as something that
                // moved into place.
                NumberAnimation { property: "opacity"; from: 0; to: 1
                                   duration: Theme.arrivalTransitionDuration; easing.type: Easing.OutCubic }
                NumberAnimation { property: "scale"; from: 0.85; to: 1
                                   duration: Theme.arrivalTransitionDuration
                                   easing.type: Easing.OutBack; easing.overshoot: 1.8 }
            }
            // The cards below getting out of the way, and closing up
            // again afterwards. Slightly softer than the arrival so the
            // eye follows the card that appeared rather than the ones
            // making room for it.
            displaced: Transition {
                NumberAnimation { properties: "x,y"; duration: Theme.arrivalTransitionDuration
                                   easing.type: Easing.OutQuint }
            }
            remove: Transition {
                // Shrinks away rather than merely fading: a card that
                // only loses opacity leaves a hole the eye does not
                // connect to the stick being pulled out.
                NumberAnimation { property: "opacity"; to: 0
                                   duration: Theme.departureTransitionDuration; easing.type: Easing.InCubic }
                NumberAnimation { property: "scale"; to: 0.82
                                   duration: Theme.departureTransitionDuration; easing.type: Easing.InBack
                                   easing.overshoot: 1.4 }
            }

            delegate: Rectangle {
                id: delegateRoot
                // The mount point, or the device for a stick that is not
                // mounted (its mount point is empty, and every unmounted
                // stick would share one name).
                objectName: "stickRow:" + (mountPoint.length > 0 ? mountPoint : devicePath)
                width: ListView.view.width
                height: contentColumn.implicitHeight + 24
                color: Theme.surface
                border.color: Theme.border
                radius: 4

                required property string label
                // NOT required: a required property makes delegate
                // creation fail for any model that lacks the role, which
                // took out four StickListPage tests whose fake sticks
                // predate it. Defaulted instead, and the size is hidden
                // when it is zero anyway.
                property var capacityBytes: 0
                required property string mountPoint
                required property string devicePath
                required property bool mounted
                required property bool hasRekordbox
                required property bool hasEngine
                required property string rekordboxPath
                required property string enginePath
                // Required, unlike capacityBytes above, because this one
                // has to carry a real answer: a model role only reaches a
                // delegate that declares it required, so a defaulted
                // version would read false forever and the card would
                // never say anything. Every fake stick in the tests
                // supplies it for that reason.
                required property bool safeToUnplug
                required property bool hasOneLibrary
                required property bool isSdCard
                required property bool isFolder
                required property bool isBrowsedBackup
                required property string libraryId
                // The kernel mounted this stick read-only, which is what a
                // damaged filesystem looks like after an unclean unplug.
                // Nothing can be written until it has been checked, so
                // every card that writes goes read-only too and points at
                // Library Health, which offers the repair.
                required property bool readOnly
                readonly property bool hasKnownLibrary: hasRekordbox || hasEngine
                // Which cards a row may offer that write: a library, and
                // not a stick backup being browsed. Every writing card
                // binds to this one line rather than restating the rule.
                readonly property bool writable: hasKnownLibrary && !isBrowsedBackup
                // Another instance is editing this stick's library: every
                // card that would change it goes read-only.
                readonly property bool lockedByOther: root.isLockedByOther(delegateRoot.libraryId)
                // What a card that writes says when the stick itself is
                // the reason it cannot: a click opens Library Health
                // rather than only explaining, since the repair lives
                // there and sending someone looking for it is no help.
                readonly property string readOnlyNote:
                    "This stick is mounted read-only: its filesystem needs checking. Library Health can do that."
                // What the backup advisor found for this stick (see
                // BackupAdvisorController); null until it has looked.
                readonly property var advice: root.backupAdvisor.advice[mountPoint] || null
                readonly property string adviceState: advice ? advice.state : ""
                // Another mounted stick whose library could be copied onto
                // this empty one / is a newer copy of this stick's library.
                readonly property var cloneSource: advice && advice.cloneSource && advice.cloneSource.kind === "stick"
                    ? advice.cloneSource : null
                readonly property var updateSource: advice && advice.updateSource && advice.updateSource.kind !== "none"
                    ? advice.updateSource : null
                // In flight (mount, unmount, or an automatic mount) via this
                // row's own devicePath -- distinct from mediaController.busy,
                // which is true for the whole app while ANY stick's task is
                // running (they're processed one at a time) and used to
                // disable every OTHER row's button too, making a click on a
                // stick nothing else was busy with look like it did nothing.
                readonly property bool thisRowBusy: root.mediaController.busy
                    && root.mediaController.busyDevicePath === delegateRoot.devicePath
                function assessBackup() {
                    // A browsed backup is never a backup subject or peer:
                    // the advisor would offer it as the newest clone
                    // source, and cloning from it targets its own archive.
                    if (mounted && mountPoint.length > 0 && !isBrowsedBackup) {
                        root.backupAdvisor.assess(label, mountPoint, rekordboxPath, enginePath);
                    }
                }
                Component.onCompleted: assessBackup()
                onMountedChanged: assessBackup()

                ColumnLayout {
                    id: contentColumn
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 4

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12

                    // A plain Item + explicit MouseArea, not an ItemDelegate
                    // -- ItemDelegate's own hover/press background isn't
                    // reliably gated by `enabled` in this KDE-Breeze/Material
                    // style mashup (confirmed: `enabled: !mounted` still left
                    // the row hover-highlighting and accepting clicks once
                    // mounted). ScanPage.qml's track rows already hit the
                    // exact same class of Material-Control-chrome issue and
                    // settled on this same Rectangle+MouseArea sidestep --
                    // see its comment for the fuller story.
                    Item {
                        Layout.fillWidth: true
                        implicitHeight: rowContent.implicitHeight

                        Rectangle {
                            anchors.fill: parent
                            anchors.margins: -6
                            radius: 4
                            visible: !delegateRoot.mounted
                            color: rowMouseArea.pressed ? Theme.rowPressed
                                : rowMouseArea.containsMouse ? Theme.rowHover
                                : "transparent"
                        }

                        MouseArea {
                            id: rowMouseArea
                            anchors.fill: parent
                            // Not gated on the whole app being busy anymore:
                            // mountStick() queues behind whatever else is
                            // running instead of being silently dropped, so
                            // there's no reason to make this look unusable
                            // meanwhile -- only this row's own task (if any)
                            // disables it.
                            enabled: !delegateRoot.mounted && !delegateRoot.thisRowBusy
                            hoverEnabled: !delegateRoot.mounted && !delegateRoot.thisRowBusy
                            ToolTip.visible: containsMouse
                            ToolTip.text: "Click to mount " + delegateRoot.label
                            onClicked: root.mediaController.mountStick(delegateRoot.devicePath)
                        }

                        RowLayout {
                            id: rowContent
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 12

                            UsbStickIcon {
                                size: Theme.iconSizeNormal
                                Layout.alignment: Qt.AlignVCenter
                                isSdCard: delegateRoot.isSdCard
                                isFolder: delegateRoot.isFolder
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                RowLayout {
                                    Layout.fillWidth: true
                                    Subtitle {
                                        text: delegateRoot.label
                                        color: Theme.text
                                    }
                                    Item { Layout.fillWidth: true }
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 8
                                    Label {
                                        objectName: "stickPathLabel"
                                        text: delegateRoot.mounted ? delegateRoot.mountPoint : delegateRoot.devicePath
                                        color: Theme.textMuted
                                        font.pointSize: Theme.baseFontPointSize * 0.9
                                        elide: Text.ElideMiddle
                                        // A Text's Layout.minimumWidth
                                        // defaults to its implicit width,
                                        // so this had a floor at its full
                                        // natural size: the elide could
                                        // never fire and a long mount
                                        // point pushed the size beside it
                                        // off the card instead. The
                                        // maximum keeps a short path from
                                        // stretching; the minimum is what
                                        // lets a long one shorten.
                                        Layout.minimumWidth: 0
                                        // Whole pixels, with one to spare. A layout
                                        // hands out whole pixels, and a Text given
                                        // a fraction less than its natural width
                                        // elides: /media/sebas/WHALESHARK2 measures
                                        // 208.03 and got 208, abbreviated on a card
                                        // with hundreds of pixels left over. Same
                                        // fix as BackBreadcrumb's crumbs.
                                        Layout.preferredWidth: Math.ceil(implicitWidth) + 1
                                        Layout.maximumWidth: Math.ceil(implicitWidth) + 1
                                        Layout.fillWidth: true
                                    }
                                    // The stick's size, beside the path. Hidden
                                    // rather than shown as "0 B" when the locator
                                    // could not read a capacity, which happens for
                                    // a drive with no partition table at all.
                                    Label {
                                        visible: delegateRoot.capacityBytes > 0
                                        text: Theme.humanBytes(delegateRoot.capacityBytes)
                                        color: Theme.textMuted
                                        font.pointSize: Theme.baseFontPointSize * 0.9
                                    }
                                    Item { Layout.fillWidth: true }
                                }
                                // What is on the stick or, once it is
                                // unmounted, whether it may be pulled. One
                                // row for both, at one height, so pressing
                                // eject changes what the row says without
                                // moving the card under the pointer.
                                RowLayout {
                                    objectName: "stickStateRow"
                                    spacing: Theme.tightSpacing
                                    Layout.preferredHeight: Math.max(unmountedLabel.implicitHeight,
                                                                     deviceLibraryBadge.implicitHeight)
                                    // "OK to unplug" where that is provably
                                    // true, and the old description where it
                                    // is not. The state is the same either
                                    // way -- nothing of ours holds the device
                                    // open -- but the reader asks this right
                                    // after pressing eject, and wants to know
                                    // whether they may pull the stick out.
                                    // True for a stick that was never mounted
                                    // too: an unmounted device is safe to pull
                                    // however it got that way.
                                    Label {
                                        id: unmountedLabel
                                        objectName: "unmountedLabel"
                                        visible: !delegateRoot.mounted
                                        text: delegateRoot.safeToUnplug ? "OK to unplug" : "(not mounted)"
                                        color: Theme.textMuted
                                    }
                                    // Said, not left as an empty line, for a
                                    // mounted stick with no catalog on it.
                                    Label {
                                        objectName: "noLibraryLabel"
                                        visible: delegateRoot.mounted && !delegateRoot.hasRekordbox
                                                 && !delegateRoot.hasEngine && !delegateRoot.hasOneLibrary
                                        text: "No library"
                                        color: Theme.textMuted
                                    }
                                    StatusBadge {
                                        id: deviceLibraryBadge
                                        objectName: "deviceLibraryBadge"
                                        visible: delegateRoot.mounted && delegateRoot.hasRekordbox
                                        label: "DeviceLibrary"
                                        badgeColor: Theme.accent
                                    }
                                    StatusBadge {
                                        objectName: "oneLibraryBadge"
                                        visible: delegateRoot.mounted && delegateRoot.hasOneLibrary
                                        label: "OneLibrary"
                                        badgeColor: Theme.accent
                                    }
                                    StatusBadge {
                                        objectName: "engineBadge"
                                        visible: delegateRoot.mounted && delegateRoot.hasEngine
                                        label: "Engine"
                                        badgeColor: Theme.accent
                                    }
                                }
                            }
                        }
                    }

                    // Mount/unmount now run on a background thread (a real
                    // syscall/subprocess that can visibly take a moment --
                    // this exact freeze used to look like the app had hung
                    // or the stick had vanished, with zero feedback that
                    // anything was happening). While this row's own
                    // operation is in flight, show a spinner in the eject
                    // button's place instead of leaving it looking dead.

                    BusyIndicator {
                        visible: delegateRoot.thisRowBusy
                        running: visible
                        Layout.preferredWidth: Theme.iconSizeLarge
                        Layout.preferredHeight: Theme.iconSizeLarge
                        Layout.alignment: Qt.AlignVCenter
                    }

                    // A folder was never mounted, so there is nothing to
                    // eject -- the equivalent is dropping it from the
                    // list, which touches nothing on disk.
                    ToolButton {
                        visible: delegateRoot.isFolder
                        objectName: "closeFolderButton"
                        // Icon only; the text is what an assistive
                        // reader announces.
                        display: AbstractButton.IconOnly
                        text: "Remove from list"
                        icon.source: Theme.iconUrl("window-close")
                        icon.color: Theme.text
                        icon.width: root.headerIconSize
                        icon.height: root.headerIconSize
                        Layout.preferredWidth: Theme.iconSizeLarge
                        Layout.preferredHeight: Theme.iconSizeLarge
                        Layout.alignment: Qt.AlignVCenter
                        ToolTip.visible: hovered
                        ToolTip.text: "Remove " + delegateRoot.label + " from this list (nothing on disk is changed)"
                        onClicked: {
                            if (root.releaseOpenedFolder()) {
                                root.mediaController.closeFolder(delegateRoot.mountPoint);
                            }
                        }
                    }

                    ToolButton {
                        visible: !delegateRoot.thisRowBusy && !delegateRoot.isFolder
                        // Not `!root.mediaController.busy`: that disabled
                        // every OTHER row's button too while any one stick's
                        // task was running (including a background auto-
                        // mount), which read as "eject does nothing" on a
                        // stick that was not itself busy at all. A click
                        // here queues behind whatever else is in flight.
                        objectName: "ejectButton"
                        enabled: true
                        display: AbstractButton.IconOnly
                        text: delegateRoot.mounted ? "Eject" : "Mount"
                        icon.source: Theme.iconUrl("media-eject")
                        icon.color: Theme.text
                        icon.width: root.headerIconSize
                        icon.height: root.headerIconSize
                        // Rotating the eject icon 180° to mean "mount"
                        // isn't a real convention -- it just reads as
                        // an upside-down (broken-looking) eject icon.
                        // Kept upright always; the tooltip (and now
                        // click-anywhere-on-the-row) carry the "mount"
                        // meaning instead.
                        Layout.preferredWidth: Theme.iconSizeLarge
                        Layout.preferredHeight: Theme.iconSizeLarge
                        Layout.alignment: Qt.AlignVCenter
                        ToolTip.visible: hovered
                        ToolTip.text: delegateRoot.mounted ? "Eject " + delegateRoot.label : "Mount " + delegateRoot.label
                        onClicked: {
                            if (delegateRoot.mounted) {
                                // Stop first -- unmounting out from under an open
                                // file handle on the playing track would be bad.
                                root.playbackController.stop();
                                root.mediaController.unmountStick(delegateRoot.devicePath);
                            } else {
                                root.mediaController.mountStick(delegateRoot.devicePath);
                            }
                        }
                    }
                }

                // No point showing a wall of disabled action buttons for a
                // stick that isn't mounted yet (nothing here is clickable
                // until it is -- click the row itself to mount) or that's
                // mounted but has no rekordbox/Engine library on it at all
                // (there's nothing for any of these actions to do).
                Label {
                    Layout.fillWidth: true
                    Layout.topMargin: 8
                    visible: !delegateRoot.hasKnownLibrary
                    wrapMode: Text.WordWrap
                    color: Theme.textMuted
                    text: delegateRoot.mounted
                        ? "No DeviceLibrary or Engine library detected on this stick. "
                          + (delegateRoot.cloneSource !== null
                             ? delegateRoot.cloneSource.detail + " "
                             : "")
                          + (delegateRoot.adviceState === "restore"
                             ? delegateRoot.advice.detail + " (" + delegateRoot.advice.backupLabel + ")"
                             : "Restore a backup onto it, or format it.")
                        : "Click to mount, then Seabass will show what's available here."
                }

                ColumnLayout {
                    // Always visible now, unlike the individual cards
                    // below -- Format is available for every stick
                    // regardless of whether it has a recognized library
                    // (it's the one thing you can do to a stick that
                    // *doesn't*), so this whole grid can no longer be
                    // gated behind hasKnownLibrary the way it used to be.
                    Layout.fillWidth: true

                    GridLayout {
                        objectName: "actionGrid"
                        Layout.fillWidth: true
                        Layout.topMargin: 8
                        Layout.bottomMargin: 8
                        // From the space the cards get -- this column's width,
                        // set by the stick card -- counting the spacing between
                        // them. Not the grid's own width: that follows its
                        // columns, and binding to it would feed back.
                        columns: Math.max(1, Math.min(3, Math.floor((parent.width + columnSpacing)
                                                                    / (root.minimumCardWidth + columnSpacing))))
                        columnSpacing: 12
                        rowSpacing: 12

                        ActionCard {
                            cardTitle: "Browse Library"
                            cardSubtitle: "View tracks, playlists and cues"
                            cardIcon: "view-media-track"
                            visible: delegateRoot.hasKnownLibrary
                            enabled: delegateRoot.hasRekordbox || delegateRoot.hasEngine
                            onClicked: root.browseRequested(delegateRoot.label, delegateRoot.rekordboxPath, delegateRoot.enginePath)
                        }
                        // Every card below that writes is withheld for a
                        // browsed backup: its analysis files are in the
                        // archive, not on disk, so a rekordbox cue write
                        // would fail mid-save, and the directory is
                        // replaced on the next open, so an Engine write
                        // would silently vanish. Browse, Statistics and
                        // Metadata Backup only read, and stay.
                        ActionCard {
                            cardTitle: "Housekeeping"
                            readOnly: delegateRoot.lockedByOther || delegateRoot.readOnly
                            readOnlyReason: delegateRoot.readOnly ? delegateRoot.readOnlyNote
                                : "Another Seabass instance is editing this library"
                            onReadOnlyClicked: {
                                if (delegateRoot.readOnly) {
                                    root.libraryHealthRequested(delegateRoot.label, delegateRoot.rekordboxPath,
                                                                delegateRoot.enginePath);
                                } else {
                                    root.explainLock(delegateRoot.libraryId);
                                }
                            }
                            cardSubtitle: "Duplicate stats, copy cues between copies, and clean up"
                            cardIcon: "edit-clear-all"
                            visible: delegateRoot.writable
                            enabled: delegateRoot.hasRekordbox || delegateRoot.hasEngine
                            onClicked: root.duplicateTracksHubRequested(delegateRoot.label, delegateRoot.rekordboxPath, delegateRoot.enginePath)
                        }
                        ActionCard {
                            cardTitle: "Library Health"
                            readOnly: delegateRoot.lockedByOther
                            onReadOnlyClicked: root.explainLock(delegateRoot.libraryId)
                            cardSubtitle: "Find rows whose file is missing and repair or clean them up"
                            cardIcon: "kt-check-data"
                            // Graduated from experimental (see
                            // docs/experimental-features.md) after real
                            // use with no incidents.
                            visible: delegateRoot.writable
                            enabled: delegateRoot.hasRekordbox || delegateRoot.hasEngine
                            onClicked: root.libraryHealthRequested(delegateRoot.label, delegateRoot.rekordboxPath, delegateRoot.enginePath)
                        }
                        ActionCard {
                            cardTitle: "Library Statistics"
                            cardSubtitle: "Filesystem, library stats, and disk usage"
                            cardIcon: "office-chart-bar"
                            // Graduated from experimental (see
                            // docs/experimental-features.md) after real
                            // use with no incidents.
                            visible: delegateRoot.hasKnownLibrary
                            enabled: delegateRoot.hasRekordbox || delegateRoot.hasEngine
                            onClicked: root.stickStatisticsRequested(delegateRoot.label, delegateRoot.rekordboxPath, delegateRoot.enginePath)
                        }
                        ActionCard {
                            cardTitle: "USB Stick Performance"
                            cardSubtitle: "Measure the stick the way a player reads it, per player generation"
                            cardIcon: "speedometer"
                            // The write test has nowhere to write on a
                            // read-only stick, and half a benchmark is
                            // worse than none.
                            readOnly: delegateRoot.readOnly
                            readOnlyReason: delegateRoot.readOnlyNote
                            onReadOnlyClicked: root.libraryHealthRequested(delegateRoot.label, delegateRoot.rekordboxPath,
                                                                           delegateRoot.enginePath)
                            // Needs no library: a stick with any files on
                            // it is measured on those, a blank one on
                            // throwaway files the page writes and removes.
                            // A browsed backup or an opened folder is on
                            // this computer, and measuring it would say
                            // nothing about any stick.
                            visible: delegateRoot.mounted && !delegateRoot.isBrowsedBackup && !delegateRoot.isFolder
                            onClicked: root.stickPerformanceRequested(delegateRoot.label, delegateRoot.rekordboxPath,
                                                                      delegateRoot.enginePath, delegateRoot.mountPoint)
                        }
                        ActionCard {
                            cardTitle: "Metadata Backup"
                            cardSubtitle: "Copy this stick's cues, ratings and comments to this computer"
                            cardIcon: "document-save"
                            // Not gated on the write lock: this only ever
                            // writes to the local store, so another
                            // session editing the library is no reason to
                            // refuse a copy of what is on it.
                            visible: delegateRoot.hasKnownLibrary
                            enabled: delegateRoot.hasRekordbox || delegateRoot.hasEngine
                            onClicked: root.metadataBackupRequested(delegateRoot.label, delegateRoot.rekordboxPath,
                                                                    delegateRoot.enginePath, delegateRoot.libraryId)
                        }
                        ActionCard {
                            cardTitle: "Restore Metadata"
                            readOnly: delegateRoot.lockedByOther || delegateRoot.readOnly
                            readOnlyReason: delegateRoot.readOnly ? delegateRoot.readOnlyNote
                                : "Another Seabass instance is editing this library"
                            onReadOnlyClicked: {
                                if (delegateRoot.readOnly) {
                                    root.libraryHealthRequested(delegateRoot.label, delegateRoot.rekordboxPath,
                                                                delegateRoot.enginePath);
                                } else {
                                    root.explainLock(delegateRoot.libraryId);
                                }
                            }
                            cardSubtitle: "Put cues from this computer back on tracks that have lost them"
                            cardIcon: "document-import"
                            visible: delegateRoot.writable
                            enabled: delegateRoot.hasRekordbox || delegateRoot.hasEngine
                            onClicked: root.metadataRestoreRequested(delegateRoot.label, delegateRoot.rekordboxPath,
                                                                     delegateRoot.enginePath, delegateRoot.libraryId)
                        }
                        ActionCard {
                            cardTitle: "Create Engine Library"
                            readOnly: delegateRoot.lockedByOther || delegateRoot.readOnly
                            readOnlyReason: delegateRoot.readOnly ? delegateRoot.readOnlyNote
                                : "Another Seabass instance is editing this library"
                            onReadOnlyClicked: {
                                if (delegateRoot.readOnly) {
                                    root.libraryHealthRequested(delegateRoot.label, delegateRoot.rekordboxPath,
                                                                delegateRoot.enginePath);
                                } else {
                                    root.explainLock(delegateRoot.libraryId);
                                }
                            }
                            cardSubtitle: "Build a new Engine Library from this stick's DeviceLibrary export"
                            cardIcon: "server-database"
                            // Experimental (see docs/experimental-features.md):
                            // the first feature here that fabricates a whole
                            // new database from scratch. Only makes sense
                            // when there's rekordbox data to build from and
                            // no Engine Library already present to overwrite.
                            experimental: true
                            experimentalFeaturesEnabled: root.appSettingsController.experimentalFeaturesEnabled
                            // Both sides of a merge, kept. Master hides
                            // this once the stick HAS an Engine Library
                            // rather than showing it disabled: a disabled
                            // control is an offer the user has to work out
                            // they cannot take, and it pushes every card
                            // below it down the page for nothing.
                            // backup-browsing hides it on anything not
                            // writable, which is how a browsed backup
                            // stops offering write actions at all.
                            // `writable` already implies hasKnownLibrary
                            // (hasKnownLibrary && !isBrowsedBackup), so
                            // the two compose without repeating it.
                            // The experimental gate restated: a visible
                            // binding of our own replaces ActionCard's
                            // default, which is where the gate lives, and
                            // this is the one card still behind it.
                            visible: delegateRoot.writable && !delegateRoot.hasEngine
                                && (!experimental || experimentalFeaturesEnabled)
                            enabled: delegateRoot.hasRekordbox
                            onClicked: root.engineLibraryCreatorRequested(delegateRoot.label, delegateRoot.rekordboxPath)
                        }
                        ActionCard {
                            cardTitle: "Sync Cue Points"
                            readOnly: delegateRoot.lockedByOther || delegateRoot.readOnly
                            readOnlyReason: delegateRoot.readOnly ? delegateRoot.readOnlyNote
                                : "Another Seabass instance is editing this library"
                            onReadOnlyClicked: {
                                if (delegateRoot.readOnly) {
                                    root.libraryHealthRequested(delegateRoot.label, delegateRoot.rekordboxPath,
                                                                delegateRoot.enginePath);
                                } else {
                                    root.explainLock(delegateRoot.libraryId);
                                }
                            }
                            cardSubtitle: "Copy cues between DeviceLibrary and Engine"
                            cardIcon: "exchange-positions"
                            visible: delegateRoot.writable
                            enabled: delegateRoot.hasRekordbox && delegateRoot.hasEngine
                            onClicked: root.syncRequested(delegateRoot.label, delegateRoot.rekordboxPath, delegateRoot.enginePath)
                        }
                        ActionCard {
                            cardTitle: "Backups"
                            readOnly: delegateRoot.lockedByOther
                            onReadOnlyClicked: root.explainLock(delegateRoot.libraryId)
                            // The advisor's verdict on the full stick backup
                            // leads when it has one; the generic line otherwise.
                            cardSubtitle: {
                                if (delegateRoot.updateSource !== null) {
                                    return "Newer copy on " + delegateRoot.updateSource.label + ": update this stick from here";
                                }
                                switch (delegateRoot.adviceState) {
                                case "outdated": return "Update the full stick backup: " + delegateRoot.advice.detail;
                                case "behind-backup": return delegateRoot.advice.detail;
                                case "current": return "Full stick backup is up to date";
                                case "back-up-new":
                                case "no-backups": return "No full stick backup of this library yet";
                                case "different-library": return delegateRoot.advice.detail + " Back it up as new.";
                                default: return "Back up the whole stick, and manage its backups on this computer";
                                }
                            }
                            cardIcon: "backup"
                            visible: delegateRoot.writable
                            enabled: delegateRoot.hasRekordbox || delegateRoot.hasEngine
                            onClicked: root.backupsHubRequested(delegateRoot.label, delegateRoot.rekordboxPath, delegateRoot.enginePath,
                                delegateRoot.mountPoint, delegateRoot.devicePath)
                        }
                        ActionCard {
                            cardTitle: "Device Profile"
                            readOnly: delegateRoot.lockedByOther
                            onReadOnlyClicked: root.explainLock(delegateRoot.libraryId)
                            cardSubtitle: "View this stick's saved Rekordbox player settings"
                            cardIcon: "view-media-equalizer"
                            visible: delegateRoot.writable
                            enabled: delegateRoot.hasRekordbox
                            onClicked: root.settingsRequested(delegateRoot.label, delegateRoot.rekordboxPath)
                        }
                        ActionCard {
                            cardTitle: "Format USB Stick"
                            // There is no drive behind a folder row to
                            // erase, and devicePath is empty for one.
                            visible: !delegateRoot.isFolder
                            cardSubtitle: "Erase and prepare this drive for CDJs, XDJs, and Denon Engine players"
                            cardIcon: "edit-delete-shred"
                            // The one action here that can permanently erase a
                            // drive, not just modify or consolidate library
                            // data on one, so it keeps its own warnings and
                            // its type-to-confirm dialog.
                            // Available for every stick regardless of
                            // hasKnownLibrary -- unlike every other card
                            // here, this is the one action meant for a
                            // stick with nothing recognizable on it yet.
                            enabled: !root.mediaController.busy
                            onClicked: root.formatUsbRequested()
                        }
                        // "Restore a Backup" and "Create Backup USB Stick" used
                        // to be two separate cards for the same job (putting
                        // a library onto an empty stick, whichever copy is
                        // newer/available) -- merged into one, since an
                        // empty stick never needs both at once. Prefers a
                        // peer stick's own live copy (cloneSource) over a
                        // disk backup when both exist, same priority order
                        // adviseStickBackup already uses for the update case.
                        ActionCard {
                            cardTitle: "Create Backup USB Stick"
                            readOnly: delegateRoot.lockedByOther || delegateRoot.readOnly
                            readOnlyReason: delegateRoot.readOnly ? delegateRoot.readOnlyNote
                                : "Another Seabass instance is editing this library"
                            onReadOnlyClicked: {
                                if (delegateRoot.readOnly) {
                                    root.libraryHealthRequested(delegateRoot.label, delegateRoot.rekordboxPath,
                                                                delegateRoot.enginePath);
                                } else {
                                    root.explainLock(delegateRoot.libraryId);
                                }
                            }
                            // Visible unconditionally (see below), so its
                            // wording must not presuppose a backup exists:
                            // "no-backups" is exactly the state where none
                            // do, and it is a real, common state for this
                            // card -- a freshly formatted stick with an
                            // empty default backup directory reaches it
                            // every time. The old fallback text, "Restore a
                            // library onto this USB stick", read as though
                            // a backup were known to exist and just needed
                            // picking, which is what was reported as
                            // "Seabass offers to restore a backup ... but
                            // we don't have one".
                            cardSubtitle: delegateRoot.cloneSource !== null ? delegateRoot.cloneSource.detail
                                : (delegateRoot.adviceState === "restore"
                                    ? "Restore " + delegateRoot.advice.backupLabel + "'s library onto this stick"
                                    : "No known stick backups yet. Browse for a backup file to restore")
                            cardIcon: "edit-copy"
                            // Built on the stick backup: either a copy of
                            // another mounted stick's own current library,
                            // or an existing backup from this computer,
                            // written onto this stick.
                            // Only for a stick with nothing recognizable on
                            // it: the disaster case is a blank replacement
                            // drive. A stick that already has a library
                            // updates from its own Backups page instead.
                            // Restoring writes a whole stick through
                            // devicePath, which a folder row does not have.
                            visible: !delegateRoot.hasKnownLibrary && !delegateRoot.isFolder
                            // The disk-backup path isn't gated on `mounted`:
                            // a stick fresh out of Format USB Stick is not
                            // remounted, and the restore page mounts it
                            // itself when handed the device path. The clone
                            // path does need the source stick mounted, which
                            // cloneSource being non-null already implies
                            // (peers are only ever mounted sticks).
                            enabled: !delegateRoot.thisRowBusy && (delegateRoot.cloneSource === null
                                || (delegateRoot.mounted && delegateRoot.cloneSource.enoughSpace !== false))
                            onClicked: {
                                if (delegateRoot.cloneSource !== null) {
                                    root.cloneStickRequested(delegateRoot.cloneSource.label,
                                        delegateRoot.cloneSource.rekordboxPath, delegateRoot.cloneSource.enginePath,
                                        delegateRoot.mountPoint, delegateRoot.label, false);
                                } else {
                                    root.restoreStickBackupRequested(delegateRoot.mountPoint, delegateRoot.devicePath,
                                        delegateRoot.adviceState === "restore" ? delegateRoot.advice.backupPath : "");
                                }
                            }
                        }
                    }
                }
                }
            }

            Label {
                anchors.centerIn: parent
                visible: parent.count === 0
                text: "No USB sticks detected. Insert one to get started."
                font.pointSize: Theme.fontLarge
                color: Theme.textMuted
            }
        }

    }
}
