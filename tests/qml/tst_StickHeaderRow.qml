// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui
import "../qml-live/LiveHelpers.js" as Live

// StickHeaderRow.qml with fake controllers: the selected stick's row at
// the top of the home pane. What it says about the stick, mounted or not,
// and what its row and buttons do. The cases are the ones the home
// list's rows had (tst_StickListPage.qml). Also screenshots of the row's
// states when SEABASS_SCREENSHOT_DIR is set.
TestCase {
    id: testCase
    name: "StickHeaderRow"
    width: 1000
    height: 400
    visible: true
    when: windowShown

    property int grounds: 0

    Component {
        id: groundComponent
        Rectangle {
            width: 860
            height: 200
            color: Theme.background
        }
    }
    Component {
        id: rowComponent
        StickHeaderRow {
            x: Theme.pageMargin
            y: Theme.pageMargin
            width: 820
        }
    }
    Component {
        id: spyComponent
        SignalSpy {}
    }

    function makeStick(overrides) {
        const s = {
            label: "MAIN", mountPoint: "/media/MAIN", devicePath: "/dev/sdb1", mounted: true,
            hasRekordbox: true, hasEngine: true, rekordboxPath: "/media/MAIN/PIONEER",
            enginePath: "/media/MAIN/Engine Library", isSdCard: false, isFolder: false,
            isBrowsedBackup: false, libraryId: "lib-main", safeToUnplug: false, hasOneLibrary: false,
            readOnly: false,
        };
        for (const key in overrides) {
            s[key] = overrides[key];
        }
        return s;
    }

    function fakeMediaController(overrides) {
        const m = {busy: false, busyDevicePath: "", calls: [],
                   mountStick: function(d) { this.calls.push("mount:" + d); },
                   unmountStick: function(d) { this.calls.push("unmount:" + d); }};
        for (const key in (overrides || {})) {
            m[key] = overrides[key];
        }
        return m;
    }

    // "Sync Needed": the player will offer to import the rekordbox library
    // over the Engine one. The badge sits with the library badges and is
    // the way to Sync Cue Points, where a save settles it.
    function test_syncNeededBadgeLeadsToSync() {
        const quiet = makeRow(makeStick({syncNeeded: false}));
        verify(!findChild(quiet, "syncNeededBadge").visible, "nothing to settle: no badge");

        const row = makeRow(makeStick({syncNeeded: true}));
        const badge = findChild(row, "syncNeededBadge");
        verify(badge.visible);
        compare(badge.label, "Sync Needed");
        const spy = createTemporaryObject(spyComponent, testCase, {target: row, signalName: "syncRequested"});
        mouseClick(badge);
        tryCompare(spy, "count", 1, 2000);
        compare(spy.signalArguments[0][0], "MAIN");
        compare(spy.signalArguments[0][1], "/media/MAIN/PIONEER");
        compare(spy.signalArguments[0][2], "/media/MAIN/Engine Library");
    }

    // The same badge for Sync after Rekordbox Export (EngineUpdateRole):
    // "library" reads "Engine needs syncing" and leads there; "cues" reads
    // "Sync Needed" and leads to Sync Cue Points; "" leaves it to the old
    // counter test, which still shows the badge on its own.
    function test_engineUpdateBadgeRoutesByWhatMoved() {
        const cases = [
            {engineUpdate: "", syncNeeded: false, visible: false},
            {engineUpdate: "", syncNeeded: true, visible: true, label: "Sync Needed", signalName: "syncRequested"},
            {engineUpdate: "cues", syncNeeded: false, visible: true, label: "Sync Needed", signalName: "syncRequested"},
            {engineUpdate: "library", syncNeeded: false, visible: true, label: "Engine needs syncing",
             signalName: "rekordboxExportSyncRequested"},
            {engineUpdate: "library", syncNeeded: true, visible: true, label: "Engine needs syncing",
             signalName: "rekordboxExportSyncRequested"},
        ];
        for (const c of cases) {
            const name = "engineUpdate \"" + c.engineUpdate + "\", syncNeeded " + c.syncNeeded;
            const row = makeRow(makeStick({engineUpdate: c.engineUpdate, syncNeeded: c.syncNeeded}));
            const badge = findChild(row, "syncNeededBadge");
            compare(badge.visible, c.visible, name);
            if (!c.visible) {
                continue;
            }
            compare(badge.label, c.label, name);
            const other = c.signalName === "syncRequested" ? "rekordboxExportSyncRequested" : "syncRequested";
            const spy = createTemporaryObject(spyComponent, testCase, {target: row, signalName: c.signalName});
            const otherSpy = createTemporaryObject(spyComponent, testCase, {target: row, signalName: other});
            mouseClick(badge);
            tryCompare(spy, "count", 1, 2000, name);
            compare(otherSpy.count, 0, name + ": one way only");
            compare(spy.signalArguments[0][0], "MAIN", name);
            compare(spy.signalArguments[0][1], "/media/MAIN/PIONEER", name);
            compare(spy.signalArguments[0][2], "/media/MAIN/Engine Library", name);
        }
        // Not mounted: nothing to say, whatever the last answer was.
        const unmounted = makeRow(makeStick({engineUpdate: "library", mounted: false}));
        verify(!findChild(unmounted, "syncNeededBadge").visible);
    }

    function makeRow(stick, overrides, groundWidth) {
        // The newest on top: a click must land on the row just made.
        const groundProps = {z: ++testCase.grounds};
        if (groundWidth) {
            groundProps.width = groundWidth;
        }
        const ground = createTemporaryObject(groundComponent, testCase, groundProps);
        verify(ground !== null);
        const props = {
            row: stick,
            mediaController: fakeMediaController(),
            playbackController: {stop: function() {}},
        };
        if (groundWidth) {
            props.width = groundWidth - 2 * Theme.pageMargin;
        }
        for (const key in (overrides || {})) {
            props[key] = overrides[key];
        }
        const row = createTemporaryObject(rowComponent, ground, props);
        verify(row !== null);
        ground.height = row.y + row.implicitHeight + Theme.pageMargin;
        waitForRendering(row);
        return row;
    }

    function named(row, objectName) {
        return Live.findByObjectName(row, objectName);
    }

    function saveScreenshot(row, name) {
        if (!screenshotDir || screenshotDir.length === 0) {
            return;
        }
        const ground = row.parent;
        ground.height = row.y + row.implicitHeight + Theme.pageMargin;
        waitForRendering(ground);
        grabImage(ground).save(screenshotDir + "/" + name + ".png");
    }

    function test_aMountedStickShowsItsNamePathSizeAndCatalogs() {
        const row = makeRow(makeStick({hasOneLibrary: true, capacityBytes: 32 * 1000 * 1000 * 1000}));
        compare(named(row, "stickLabel").text, "MAIN");
        compare(named(row, "stickPathLabel").text, "/media/MAIN");
        const size = named(row, "stickCapacityLabel");
        compare(size.visible, true);
        compare(size.text, Theme.humanBytes(32 * 1000 * 1000 * 1000));
        compare(named(row, "deviceLibraryBadge").visible, true);
        compare(named(row, "oneLibraryBadge").visible, true);
        compare(named(row, "engineBadge").visible, true);
        compare(named(row, "unmountedLabel").visible, false, "a mounted stick shows its catalogs in that row instead");
        compare(named(row, "noKnownLibraryLabel").visible, false);
        compare(named(row, "ejectButton").visible, true);
        compare(named(row, "ejectButton").text, "Eject");
        compare(named(row, "closeFolderButton").visible, false);
        saveScreenshot(row, "stick-header-mounted");
    }

    // Hidden rather than shown as "0 B" when the locator could not read a
    // capacity, and a model row without the role at all is not an error.
    function test_anUnknownSizeIsLeftOut() {
        compare(named(makeRow(makeStick({})), "stickCapacityLabel").visible, false);
        compare(named(makeRow(makeStick({capacityBytes: 0})), "stickCapacityLabel").visible, false);
    }

    // What is on the stick is shown as labels, one per catalog, and only
    // for the catalogs that are there.
    function test_theRowLabelsEachCatalogOnTheStick() {
        const row = makeRow(makeStick({hasRekordbox: true, hasEngine: false, hasOneLibrary: true}));
        compare(named(row, "deviceLibraryBadge").label, "DeviceLibrary");
        compare(named(row, "oneLibraryBadge").label, "OneLibrary");
        compare(named(row, "engineBadge").visible, false);
    }

    // A mounted stick with no catalog says so in that row, and under it
    // what can be done with it.
    function test_aStickWithNoLibrarySaysSo() {
        const row = makeRow(makeStick({hasRekordbox: false, hasEngine: false, hasOneLibrary: false}));
        const label = named(row, "noLibraryLabel");
        compare(label.visible, true);
        compare(label.text, "No library");
        const line = named(row, "noKnownLibraryLabel");
        compare(line.visible, true);
        compare(line.text, "No DeviceLibrary or Engine library detected on this stick. "
                           + "Restore a backup onto it, or format it.");
        // Before the next row is built: it would be drawn over this one.
        saveScreenshot(row, "stick-header-no-library");
        compare(named(makeRow(makeStick({})), "noLibraryLabel").visible, false,
                "a stick with a catalog shows its labels instead");
    }

    // The line under an empty stick carries the advisor's news: a stick
    // to copy from, and the backup that can be restored.
    function test_theNoLibraryLineCarriesTheAdvice() {
        const advice = {state: "restore", detail: "The newest backup can be restored onto this empty stick.",
                        backupLabel: "MAIN"};
        const row = makeRow(makeStick({hasRekordbox: false, hasEngine: false}),
                            {advice: advice, adviceState: "restore",
                             cloneSource: {detail: "Copy MAIN's library onto this stick."}});
        compare(named(row, "noKnownLibraryLabel").text,
                "No DeviceLibrary or Engine library detected on this stick. Copy MAIN's library onto this stick. "
                + "The newest backup can be restored onto this empty stick. (MAIN)");
    }

    // What the row says once a stick is unmounted: whether it may be pulled.
    function test_anUnmountedStickSaysItIsSafeToUnplug() {
        const row = makeRow(makeStick({mounted: false, safeToUnplug: true}));
        // Green, and a badge like the library badges it replaces: it is
        // the answer to "may I pull it out", not a remark in grey.
        const badge = named(row, "okToUnplugBadge");
        compare(badge.visible, true);
        compare(badge.label, "OK to unplug");
        verify(Qt.colorEqual(badge.badgeColor, Theme.good), "in the good colour");
        compare(named(row, "unmountedLabel").visible, false, "one statement, not two");
        compare(named(row, "stickPathLabel").text, "/dev/sdb1", "an unmounted stick is named by its device");
        compare(named(row, "ejectButton").text, "Mount");
        compare(named(row, "deviceLibraryBadge").visible, false);
        compare(named(row, "noKnownLibraryLabel").visible, false, "the library is still known from before");
        saveScreenshot(row, "stick-header-unmounted");
    }

    // The claim is about the device, not this partition: the model works
    // it out across rows, and the row only repeats a proven answer.
    function test_anUnmountedPartitionOfABusyDeviceDoesNotSaySoIsSafe() {
        const row = makeRow(makeStick({mounted: false, safeToUnplug: false}));
        compare(named(row, "unmountedLabel").text, "(not mounted)");
    }

    function test_anUnmountedStickWithNothingKnownSaysClickToMount() {
        const row = makeRow(makeStick({mounted: false, hasRekordbox: false, hasEngine: false, mountPoint: ""}));
        compare(named(row, "noKnownLibraryLabel").text,
                "Click to mount, then Seabass will show what's available here.");
    }

    // A click anywhere on an unmounted stick's row mounts it; once it is
    // mounted the row takes no clicks.
    function test_clickingAnUnmountedRowMountsIt() {
        const row = makeRow(makeStick({mounted: false, safeToUnplug: true, mountPoint: ""}));
        const area = named(row, "mountRowArea");
        compare(area.enabled, true);
        mouseClick(named(row, "stickLabel"));
        compare(JSON.stringify(row.mediaController.calls), JSON.stringify(["mount:/dev/sdb1"]));

        const mounted = makeRow(makeStick({}));
        compare(named(mounted, "mountRowArea").enabled, false);
        mouseClick(named(mounted, "stickLabel"));
        compare(mounted.mediaController.calls.length, 0);
    }

    // Eject stops playback first (the playing track's file is on the
    // stick), then unmounts; on an unmounted stick the button mounts.
    function test_ejectStopsPlaybackThenUnmounts() {
        const row = makeRow(makeStick({}));
        // One log for both controllers, so the order shows.
        row.playbackController = {stop: function() { row.mediaController.calls.push("stop"); }};
        named(row, "ejectButton").clicked();
        compare(JSON.stringify(row.mediaController.calls), JSON.stringify(["stop", "unmount:/dev/sdb1"]));
        const unmounted = makeRow(makeStick({mounted: false, mountPoint: ""}));
        named(unmounted, "ejectButton").clicked();
        compare(JSON.stringify(unmounted.mediaController.calls), JSON.stringify(["mount:/dev/sdb1"]));
    }

    // The app-wide busy flag must not grey this row out when another
    // stick's task is the one running: a click just queues.
    function test_ejectButtonStaysUsableWhileAnotherStickIsBusy() {
        const row = makeRow(makeStick({}), {mediaController: fakeMediaController({busy: true, busyDevicePath: "/dev/sdc1"})});
        compare(row.thisRowBusy, false);
        const eject = named(row, "ejectButton");
        compare(eject.visible, true);
        compare(eject.enabled, true);
        eject.clicked();
        verify(row.mediaController.calls.indexOf("unmount:/dev/sdb1") >= 0);
        compare(named(row, "stickBusyIndicator").visible, false);
    }

    // This row's own task: a spinner in the eject button's place, and
    // the row takes no mount click meanwhile.
    function test_thisRowsOwnTaskShowsTheSpinner() {
        const row = makeRow(makeStick({mounted: false, mountPoint: ""}),
                            {mediaController: fakeMediaController({busy: true, busyDevicePath: "/dev/sdb1"})});
        compare(row.thisRowBusy, true);
        compare(named(row, "stickBusyIndicator").visible, true);
        compare(named(row, "ejectButton").visible, false);
        compare(named(row, "mountRowArea").enabled, false);
        // The page may say so itself, too.
        const told = makeRow(makeStick({}), {thisRowBusy: true});
        compare(named(told, "stickBusyIndicator").visible, true);
    }

    // A folder has nothing to eject: its button removes it from the list,
    // which the page does once it has let go of the folder's edit session.
    function test_aFolderRowIsClosedNotEjected() {
        const row = makeRow(makeStick({label: "restored-backup", mountPoint: "/home/dj/restored", devicePath: "",
                                       isFolder: true}));
        compare(named(row, "ejectButton").visible, false);
        const close = named(row, "closeFolderButton");
        compare(close.visible, true);
        compare(named(row, "stickIcon").iconName, "folder");
        const spy = createTemporaryObject(spyComponent, testCase, {target: row, signalName: "closeFolderRequested"});
        close.clicked();
        compare(spy.count, 1);
        compare(spy.signalArguments[0][0], "/home/dj/restored");
        compare(row.mediaController.calls.length, 0, "closing is the page's to do, after its checks");
        saveScreenshot(row, "stick-header-folder");
    }

    function test_anSdCardIsDrawnAsOne() {
        compare(named(makeRow(makeStick({isSdCard: true})), "stickIcon").iconName, "media-flash-sd-mmc");
        compare(named(makeRow(makeStick({})), "stickIcon").iconName, "drive-removable-media-usb-pendrive");
    }

    // Ejecting swaps the labels for "OK to unplug" in the same row, at the
    // same place and height, so the row does not jump under the pointer.
    function test_ejectingDoesNotMoveTheRow() {
        const mounted = makeRow(makeStick({}));
        const unmounted = makeRow(makeStick({mounted: false, safeToUnplug: true}));
        const mountedRow = named(mounted, "stickStateRow");
        const unmountedRow = named(unmounted, "stickStateRow");
        compare(unmountedRow.visible, true, "the row stays when the stick is unmounted");
        compare(unmountedRow.height, mountedRow.height);
        compare(unmountedRow.mapToItem(unmounted, 0, 0).y, mountedRow.mapToItem(mounted, 0, 0).y);
        compare(unmounted.height, mounted.height);
    }

    // The name's left edge is what textInset says, so the pane can line
    // its group heading and cards up with it.
    function test_textInsetIsWhereTheNameIs() {
        const row = makeRow(makeStick({}));
        const name = named(row, "stickLabel");
        compare(Math.round(name.mapToItem(row, 0, 0).x), Math.round(row.textInset));
    }

    function test_aLongMountPointElidesInsteadOfPushingTheRowWide() {
        // A narrow row, so the label really is out of room on any font.
        const row = makeRow(makeStick({
            label: "LONGONE",
            mountPoint: "/run/media/sebas/a-very-long-mount-point-name-that-will-not-fit-on-one-card-line",
            capacityBytes: 64 * 1000 * 1000 * 1000,
        }), {}, 420);
        const label = named(row, "stickPathLabel");
        compare(label.elide, Text.ElideMiddle);
        verify(label.implicitWidth > 0, "the label must have measured its text");
        verify(label.width < label.implicitWidth,
               "a path too long for the row must be elided down (width " + label.width
               + " vs implicit " + label.implicitWidth + ")");
        const size = named(row, "stickCapacityLabel");
        verify(size.mapToItem(row, size.width, 0).x <= row.width, "the size stays on the row");
    }

    // A path the row has room for is shown whole, including one whose
    // natural width is a fraction of a pixel (the layout hands out whole
    // pixels, and a Text a fraction short of its width elides).
    function test_aMountPointWithRoomIsNotElided_data() {
        return [{tag: "short", mountPoint: "/media/MAIN"}, {tag: "real", mountPoint: "/media/sebas/WHALESHARK2"}];
    }
    function test_aMountPointWithRoomIsNotElided(data) {
        const row = makeRow(makeStick({label: "WHALESHARK2", mountPoint: data.mountPoint}));
        const label = named(row, "stickPathLabel");
        verify(!label.truncated,
               "a path with room to spare must not be abbreviated (width " + label.width
               + " vs implicit " + label.implicitWidth + ")");
    }
}
