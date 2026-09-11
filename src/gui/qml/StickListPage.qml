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

    // The two store counts, readable by a test that wants to check a
    // button's enabled state against the thing that decides it rather
    // than against a hardcoded expectation of what the machine holds.
    readonly property int homeBackupsFullCount: homeBackups.fullBackupCount
    readonly property int homeBackupsMetadataCount: homeBackups.metadataTrackCount

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
        // one: the two buttons under the list are stale until asked.
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
        onAccepted: {
            var message = root.mediaController.openBackup(selectedFile.toString());
            if (message.length > 0) {
                openFolderError.text = message;
                openFolderError.open();
            }
        }
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
    // The two counts the row under the stick list needs, and nothing
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
    signal localCueRequested(string stickLabel, string rekordboxPath, string enginePath)
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
            ColumnLayout {
                objectName: "brandLockup"
                spacing: 0
                // Same two colours AboutPage gives this pair, which is
                // the only other place the lockup appears: the Kelp
                // palette's text, and its accent lightened, so the name
                // and the slogan read as one mark rather than as a title
                // that happens to have a line under it.
                Label {
                    objectName: "brandName"
                    text: "Seabass"
                    font.family: Theme.titleFamily
                    font.weight: Font.Bold
                    font.pointSize: Theme.titleMedium
                    color: Theme.text
                }
                Label {
                    objectName: "brandSlogan"
                    text: "Your DJ Toolbox"
                    font.family: Theme.titleFamily
                    font.weight: Theme.titleWeight
                    // A clear step down from the name above it: this is
                    // the subtitle, and at the same size the pair read as
                    // two competing titles.
                    font.pointSize: Theme.subtitleSize
                    color: Qt.lighter(Theme.accent, 1.3)
                }
            }
            Item { Layout.fillWidth: true }
            ToolButton {
                objectName: "openBackupButton"
                icon.name: "package-x-generic"
                display: AbstractButton.IconOnly
                text: "Browse a full stick backup"
                ToolTip.visible: hovered
                ToolTip.text: "Browse a full stick backup -- opened in place, nothing is unpacked"
                onClicked: openBackupDialog.open()
            }
            ToolButton {
                objectName: "openFolderButton"
                icon.name: "folder-open"
                display: AbstractButton.IconOnly
                text: "Open a library from a folder"
                ToolTip.visible: hovered
                ToolTip.text: "Open a library from a folder -- a copy on this computer, or a restored stick"
                onClicked: openFolderDialog.open()
            }
            // Breeze's own icons rather than emoji.
            //
            // These were plain emoji with no font.family, which the
            // system resolves to the color emoji font: five full-color
            // pictures in a header whose every other mark is a flat
            // glyph. Named theme icons instead, so they are monochrome,
            // follow the desktop's icon theme, and recolor with it.
            //
            // icon.name, not a bundled .svg: Breeze recolors through
            // `fill:currentColor`, which Qt's SVG renderer does not
            // resolve (see HomeIcon.qml, which had to become a Shape for
            // exactly that reason). Going through QIcon::fromTheme gets
            // the platform's own resolved, recolored icon instead.
            //
            // text is set on each despite IconOnly: it never renders,
            // and it is what an assistive reader announces.
            ToolButton {
                objectName: "aboutButton"
                icon.name: "help-about"
                display: AbstractButton.IconOnly
                text: "About Seabass"
                ToolTip.visible: hovered
                ToolTip.text: "About Seabass"
                onClicked: root.aboutRequested()
            }
            ToolButton {
                objectName: "preferencesButton"
                icon.name: "configure"
                display: AbstractButton.IconOnly
                text: "Preferences"
                ToolTip.visible: hovered
                ToolTip.text: "Preferences"
                onClicked: root.appSettingsRequested()
            }
            ToolButton {
                id: donateButton
                objectName: "donateButton"
                // The one that stays in colour, deliberately: this is
                // the only button in the header asking for something
                // rather than offering something, and a red mark among
                // five grey ones is what makes it findable.
                //
                // Breeze's favourite emblem, which is a star rather than
                // a heart -- the theme ships no heart in any category
                // (checked across every installed theme). Its artwork is
                // fill:currentColor, so icon.color is what colours it.
                icon.name: "emblem-favorite"
                icon.color: Theme.danger
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
        // sticks are what this page is about and these two cards sit
        // above them taking the top of the screen for the case that is
        // not happening. They are still reachable from the row under the
        // list, which is where someone looks for them once there is a
        // stick to compare against.
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
                    cardIcon: "🗂"
                    experimental: true
                    experimentalFeaturesEnabled: root.appSettingsController.experimentalFeaturesEnabled
                    // No stick preselected: the page itself lists every
                    // mounted drive and every backup on disk to choose from.
                    onClicked: root.restoreStickBackupRequested("", "", "")
                }
                ActionCard {
                    objectName: "generalLocalCueCard"
                    cardTitle: "Local Cue Backup"
                    cardSubtitle: "Cue backups kept on this computer, across every stick"
                    cardIcon: "💿"
                    deprecated: true
                    deprecatedNote: "Needs rework"
                    onClicked: root.localCueRequested("", "", "")
                }
            }
        }

        ListView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            model: root.mediaController.sticks
            clip: true
            // Not draggable when every stick already fits.
            interactive: contentHeight > height
            spacing: 4

            // A plain Rectangle, not a Frame -- Qt Quick Controls' Material
            // style gives Frame its own additional implicit chrome that a
            // custom `background:` assignment doesn't fully replace (kept
            // rendering a stray light margin around the real content no
            // matter what the override's own color was set to). Every
            // other grouping frame in this app (SyncPage.qml/
            // DuplicatesPage.qml's meta-track groups) already uses this
            // same plain-Rectangle pattern for exactly that reason.
            delegate: Rectangle {
                id: delegateRoot
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
                required property bool isSdCard
                required property bool isFolder
                required property bool isBrowsedBackup
                required property string libraryId
                readonly property bool hasKnownLibrary: hasRekordbox || hasEngine
                // Which cards a row may offer that write: a library, and
                // not a stick backup being browsed. Every writing card
                // binds to this one line rather than restating the rule.
                readonly property bool writable: hasKnownLibrary && !isBrowsedBackup
                // Another instance is editing this stick's library: every
                // card that would change it goes read-only.
                readonly property bool lockedByOther: root.isLockedByOther(delegateRoot.libraryId)
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
                                Layout.preferredWidth: Theme.iconSizeNormal
                                Layout.preferredHeight: Theme.iconSizeNormal
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
                                    Label {
                                        text: delegateRoot.mounted ? "" : "(not mounted)"
                                        color: Theme.textMuted
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
                                        Layout.maximumWidth: implicitWidth
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
                                RowLayout {
                                    visible: delegateRoot.mounted
                                    Label { text: "DeviceLibrary: " + (delegateRoot.hasRekordbox ? "yes" : "no"); color: Theme.text }
                                    Label { text: "  Engine: " + (delegateRoot.hasEngine ? "yes" : "no"); color: Theme.text }
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
                        text: "✕"
                        font.pointSize: Theme.fontLarge
                        Layout.preferredWidth: Theme.iconSizeLarge
                        Layout.preferredHeight: Theme.iconSizeLarge
                        Layout.alignment: Qt.AlignVCenter
                        ToolTip.visible: hovered
                        ToolTip.text: "Remove " + delegateRoot.label + " from this list (nothing on disk is changed)"
                        onClicked: {
                            // A folder row takes no part in the pulled-stick
                            // prompts, so this is the one place its unsaved
                            // edits would otherwise go unseen: refuse while
                            // dirty, release a clean session's lock, then drop.
                            var reg = root.editRegistry;
                            var id = delegateRoot.libraryId;
                            if (reg && reg.hasSession(id)) {
                                var session = reg.sessionFor(id);
                                if (session && session.dirty === true) {
                                    openFolderError.text = "\"" + delegateRoot.label
                                        + "\" has unsaved changes. Save or discard them before removing it from the list.";
                                    openFolderError.open();
                                    return;
                                }
                                reg.closeSession(id);
                            }
                            root.mediaController.closeFolder(delegateRoot.mountPoint);
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
                        text: "⏏"
                        font.family: "Noto Sans Symbols2"
                        font.pointSize: Theme.fontHuge
                        // Rotating the eject glyph 180° to mean "mount"
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
                        Layout.fillWidth: true
                        Layout.topMargin: 8
                        Layout.bottomMargin: 8
                        columns: 3
                        columnSpacing: 12
                        rowSpacing: 12

                        ActionCard {
                            cardTitle: "Browse Library"
                            cardSubtitle: "View tracks, playlists and cues"
                            cardIcon: "▤"
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
                            readOnly: delegateRoot.lockedByOther
                            onReadOnlyClicked: root.explainLock(delegateRoot.libraryId)
                            cardSubtitle: "Duplicate stats, copy cues between copies, and clean up"
                            cardIcon: "▣"
                            visible: delegateRoot.writable
                            enabled: delegateRoot.hasRekordbox || delegateRoot.hasEngine
                            onClicked: root.duplicateTracksHubRequested(delegateRoot.label, delegateRoot.rekordboxPath, delegateRoot.enginePath)
                        }
                        ActionCard {
                            cardTitle: "Library Health"
                            readOnly: delegateRoot.lockedByOther
                            onReadOnlyClicked: root.explainLock(delegateRoot.libraryId)
                            cardSubtitle: "Find rows whose file is missing and repair or clean them up"
                            cardIcon: "🩹"
                            // Graduated from experimental (see
                            // docs/experimental-features.md) after real
                            // use with no incidents.
                            visible: delegateRoot.writable
                            enabled: delegateRoot.hasRekordbox || delegateRoot.hasEngine
                            onClicked: root.libraryHealthRequested(delegateRoot.label, delegateRoot.rekordboxPath, delegateRoot.enginePath)
                        }
                        ActionCard {
                            cardTitle: "Library Statistics"
                            cardSubtitle: "Filesystem, library stats, disk usage, and read-speed benchmark"
                            cardIcon: "📊"
                            // Graduated from experimental (see
                            // docs/experimental-features.md) after real
                            // use with no incidents.
                            visible: delegateRoot.hasKnownLibrary
                            enabled: delegateRoot.hasRekordbox || delegateRoot.hasEngine
                            onClicked: root.stickStatisticsRequested(delegateRoot.label, delegateRoot.rekordboxPath, delegateRoot.enginePath)
                        }
                        ActionCard {
                            cardTitle: "Metadata Backup"
                            cardSubtitle: "Copy this stick's cues, ratings and comments to this computer"
                            cardIcon: "💾"
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
                            readOnly: delegateRoot.lockedByOther
                            onReadOnlyClicked: root.explainLock(delegateRoot.libraryId)
                            cardSubtitle: "Put cues from this computer back on tracks that have lost them"
                            cardIcon: "📥"
                            visible: delegateRoot.writable
                            enabled: delegateRoot.hasRekordbox || delegateRoot.hasEngine
                            onClicked: root.metadataRestoreRequested(delegateRoot.label, delegateRoot.rekordboxPath,
                                                                     delegateRoot.enginePath, delegateRoot.libraryId)
                        }
                        ActionCard {
                            cardTitle: "Create Engine Library"
                            readOnly: delegateRoot.lockedByOther
                            onReadOnlyClicked: root.explainLock(delegateRoot.libraryId)
                            cardSubtitle: "Build a new Engine Library from this stick's DeviceLibrary export"
                            cardIcon: "⚙"
                            cardIconFont: "Noto Sans Symbols"
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
                            visible: delegateRoot.writable && !delegateRoot.hasEngine
                            enabled: delegateRoot.hasRekordbox
                            onClicked: root.engineLibraryCreatorRequested(delegateRoot.label, delegateRoot.rekordboxPath)
                        }
                        ActionCard {
                            cardTitle: "Sync Cue Points"
                            readOnly: delegateRoot.lockedByOther
                            onReadOnlyClicked: root.explainLock(delegateRoot.libraryId)
                            cardSubtitle: "Copy cues between DeviceLibrary and Engine"
                            cardIcon: "⇄"
                            cardIconFont: "Noto Sans Math"
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
                                default: return "Local cue backup/restore and automatic write backups";
                                }
                            }
                            cardIcon: "🗄"
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
                            cardIcon: "⚙"
                            cardIconFont: "Noto Sans Symbols"
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
                            cardIcon: "💽"
                            // Experimental (see docs/experimental-features.md):
                            // the first feature here that can permanently
                            // erase a drive, not just modify/consolidate
                            // library data on one -- wants real use before
                            // graduating.
                            experimental: true
                            experimentalFeaturesEnabled: root.appSettingsController.experimentalFeaturesEnabled
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
                            readOnly: delegateRoot.lockedByOther
                            onReadOnlyClicked: root.explainLock(delegateRoot.libraryId)
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
                                    : "No known stick backups yet -- browse for a backup file to restore")
                            cardIcon: "⧉"
                            cardIconFont: "Noto Sans Math"
                            // Experimental with the stick backup it is built
                            // on: either a copy of another mounted stick's
                            // own current library, or an existing backup
                            // from this computer, written onto this stick.
                            experimental: true
                            experimentalFeaturesEnabled: root.appSettingsController.experimentalFeaturesEnabled
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

        // Under every stick, because what is in here is not about any of
        // them: these are the two stores this computer keeps, and the
        // question they answer ("what have I got saved?") is one you ask
        // after looking at the sticks rather than before.
        //
        // Each is off when its store is empty rather than hidden. A
        // button that is missing tells you nothing; one that is there
        // and grey tells you the feature exists and that you have not
        // used it yet, and its tooltip says how to start.
        RowLayout {
            objectName: "browseBackupsRow"
            Layout.fillWidth: true
            spacing: Theme.rowSpacing

            Button {
                objectName: "browseFullBackupsButton"
                text: "Browse Full Backups"
                icon.name: "package-x-generic"
                enabled: homeBackups.fullBackupCount > 0
                onClicked: openBackupDialog.open()
                ToolTip.visible: hovered
                ToolTip.delay: 400
                ToolTip.text: homeBackups.fullBackupCount > 0
                    ? "Open one of the " + homeBackups.fullBackupCount
                      + " full stick backups on this computer. Nothing is unpacked."
                    : "No full stick backups yet. A stick's Backups card makes one."
            }
            Button {
                objectName: "browseMetadataBackupsButton"
                text: "Browse Metadata Backups"
                icon.name: "view-list-details"
                enabled: homeBackups.metadataTrackCount > 0
                // No stick: the page opens on its browse half, which is
                // the half this button is about. Backing up needs a
                // stick and says so there.
                onClicked: root.metadataBackupRequested("", "", "", "")
                ToolTip.visible: hovered
                ToolTip.delay: 400
                ToolTip.text: homeBackups.metadataTrackCount > 0
                    ? "The cues, ratings and comments kept on this computer for "
                      + homeBackups.metadataTrackCount + " tracks"
                    : "Nothing backed up yet. A stick's Metadata Backup card fills this."
            }
            Item { Layout.fillWidth: true }
        }
    }
}
