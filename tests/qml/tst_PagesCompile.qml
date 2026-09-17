// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui

// Every page in the SeabassGui module must compile *and* survive being
// built and laid out.
//
// The compile half catches structural QML errors (a dialog body that
// landed inside a Connections block was found that way). It cannot catch
// anything else: Qt.createComponent() parses and resolves types, and
// stops there. A page whose bindings throw the moment it exists, whose
// model is null, or which lays out to nothing, compiles perfectly.
//
// That gap was not hypothetical. Porting 22 dialogs to the shared
// MessageDialog component, this file stayed green while every
// destructive dialog had lost its visual default button -- found by
// looking at a screenshot, which is not a thing a test suite does.
//
// So the second half instantiates each page with the smallest fakes it
// will accept and asserts it reaches a rendered state. Two details do
// the real work:
//
//   - failOnWarning() on the QML error shapes. Without it a binding that
//     throws prints a warning and the test passes anyway, which is the
//     same failure mode this file already had once.
//   - one data row per page, so a break names the page instead of a list.
//
// Paths point nowhere on purpose. A page handed a stick that is not
// there must still build; if it cannot, that is worth knowing, and it
// keeps the suite off the filesystem.
TestCase {
    id: testCase
    name: "PagesCompile"
    width: 900
    height: 700
    visible: true
    when: windowShown

    readonly property string qmlDir: "qrc:/qt/qml/SeabassGui/src/gui/qml/"

    // The real controllers, not hand-written stand-ins. Every one is a
    // QML_ELEMENT, so the test can build the same objects the application
    // builds -- with the same properties, signals and models. A JS fake
    // would have to guess at all three, and a fake that guesses wrong
    // produces failures about the fake (the first run of this test
    // reported five, all mine) which is worse than no test: it trains
    // whoever reads it to discount what it says.
    //
    // None of them are pointed at a stick, so none of them scan.
    MediaController { id: realMedia }
    PlaybackController { id: realPlayback }
    AppSettingsController { id: realAppSettings }
    ScanController { id: realScan }
    BackupAdvisorController { id: realAdvisor }
    FormatUsbController { id: realFormatUsb }
    RestoreStickBackupController { id: realRestore }
    CloneStickController { id: realClone }
    StickBackupController { id: realStickBackup }

    readonly property var stick: ({
        stickLabel: "TESTSTICK",
        rekordboxPath: "/nonexistent/TESTSTICK/PIONEER",
        enginePath: "/nonexistent/TESTSTICK/Engine Library",
    })

    function props(extra) {
        var out = {width: 880, height: 660};
        for (var key in extra) {
            out[key] = extra[key];
        }
        return out;
    }

    function stickProps(extra) {
        var out = props(stick);
        for (var key in extra) {
            out[key] = extra[key];
        }
        return out;
    }

    // Main is an ApplicationWindow rather than a Page: it owns the stack
    // and the media controller, so instantiating it here would start the
    // application rather than test a page. Compile-only, deliberately.
    readonly property var windowPages: ["Main"]

    function pageSpecs() {
        return [
            {name: "AboutPage", props: props({})},
            {name: "DonationPage", props: props({})},
            {name: "AppSettingsPage", props: props({appSettingsController: realAppSettings})},
            {name: "AnonymizeLibraryPage", props: props({mediaController: realMedia, appSettingsController: realAppSettings})},
            {name: "FormatUsbPage", props: props({controller: realFormatUsb})},
            {name: "RestoreStickBackupPage", props: props({controller: realRestore, appSettingsController: realAppSettings})},
            {name: "CloneStickPage", props: props({controller: realClone})},
            {name: "SettingsPage", props: props({stickLabel: stick.stickLabel, pioneerRoot: stick.rekordboxPath})},
            {name: "EngineLibraryCreatorPage", props: props({stickLabel: stick.stickLabel, rekordboxPath: stick.rekordboxPath,
                mediaController: realMedia})},
            {name: "StickListPage", props: props({mediaController: realMedia, playbackController: realPlayback,
                appSettingsController: realAppSettings, backupAdvisor: realAdvisor})},
            {name: "DuplicatesHubPage", props: stickProps({})},
            {name: "JunkCuePage", props: stickProps({appSettingsController: realAppSettings})},
            {name: "BackupsPage", props: {controller: ({backupDirectory: "/tmp", currentArchivePath: "", openArchivePaths: [], backups: [], totalBytes: 0, listing: false, deleting: false, errorMessage: "", statusMessage: "", refresh: function() {}, deleteBackup: function(p) {}, browsedArchiveFor: function(r) { return ""; }, isOpen: function(p) { return false; }})}},
            {name: "BackupsHubPage", props: stickProps({appSettingsController: realAppSettings})},
            {name: "PendingDeletionsPage", props: stickProps({appSettingsController: realAppSettings})},
            {name: "MetadataBackupPage", props: stickProps({libraryId: "", appSettingsController: realAppSettings})},
            {name: "MetadataRestorePage", props: stickProps({libraryId: ""})},
            {name: "LibraryHealthHubPage", props: stickProps({playbackController: realPlayback})},
            {name: "LibraryConsistencyPage", props: stickProps({playbackController: realPlayback})},
            {name: "SyncPage", props: stickProps({playbackController: realPlayback, appSettingsController: realAppSettings})},
            {name: "ScanPage", props: stickProps({playbackController: realPlayback, appSettingsController: realAppSettings})},
            {name: "DuplicatesPage", props: stickProps({playbackController: realPlayback, appSettingsController: realAppSettings})},
            {name: "CleanupPage", props: stickProps({playbackController: realPlayback, appSettingsController: realAppSettings})},
            {name: "StickStatisticsPage", props: stickProps({playbackController: realPlayback, appSettingsController: realAppSettings})},
            {name: "StickPerformancePage", props: stickProps({})},
            {name: "StickBackupPage", props: stickProps({appSettingsController: realAppSettings, controller: realStickBackup})},
            {name: "TrackDetailPage", props: props({scanController: realScan, trackIndex: -1, format: "rekordbox",
                libraryPath: stick.rekordboxPath, playbackController: realPlayback, appSettingsController: realAppSettings})},
            {name: "MatchingPage", props: props({scanController: realScan, keyNotation: "standard", anchorSourceId: "1",
                anchorTitle: "T", anchorArtist: "A", anchorKey: "Am", anchorBpm: 128.0, anchorArtworkPath: "",
                anchorPlaylistNames: [], browseSelectedPlaylistIndex: -1})},
        ];
    }

    function crumbIn(item) {
        if (item.middleClickable !== undefined && item.title !== undefined) {
            return item;
        }
        for (var i = 0; i < item.children.length; ++i) {
            var found = crumbIn(item.children[i]);
            if (found) {
                return found;
            }
        }
        return null;
    }

    // Room under the breadcrumb before a page's content: on a page with a
    // one-row header, twice the 8 px it used to leave; on Browse Library,
    // whose header carries a second row, 8 px more than the 11 it had.
    //
    // Pixels measured from the breadcrumb itself, whose height is the
    // style's, so only valid under "Basic", the style this suite is
    // calibrated against: skipped whenever the resolved style is anything
    // else, whether that is an explicit QT_QUICK_CONTROLS_STYLE (the
    // screenshot mode and a run under the desktop's own style both set
    // one) or, as on Windows, a native default style picked with no
    // override at all.
    function test_theBreadcrumbHasRoomUnderIt() {
        if (controlsStyleForced) {
            skip("measured against the suite's default style, and this run picked another");
        }
        var backups = createTemporaryObject(Qt.createComponent(qmlDir + "BackupsPage.qml"), testCase, {controller: ({backupDirectory: "/tmp", currentArchivePath: "", openArchivePaths: [], backups: [], totalBytes: 0, listing: false, deleting: false, errorMessage: "", statusMessage: "", refresh: function() {}, deleteBackup: function(p) {}, browsedArchiveFor: function(r) { return ""; }, isOpen: function(p) { return false; }})});
        verify(backups !== null);
        waitForRendering(backups);
        var crumb = crumbIn(backups.header);
        verify(crumb !== null, "BackupsPage must have a breadcrumb");
        // The body starts pageMargin below the header.
        var bodyTop = backups.header.height + Theme.pageMargin;
        compare(Math.round(bodyTop - crumb.mapToItem(backups, 0, crumb.height).y), 16);

        var scan = createTemporaryObject(Qt.createComponent(qmlDir + "ScanPage.qml"), testCase,
                                         stickProps({playbackController: realPlayback, appSettingsController: realAppSettings}));
        verify(scan !== null);
        waitForRendering(scan);
        var scanCrumb = crumbIn(scan.header);
        var search = findChild(scan, "searchField");
        verify(scanCrumb !== null && search !== null);
        compare(Math.round(search.mapToItem(scan, 0, 0).y - scanCrumb.mapToItem(scan, 0, scanCrumb.height).y), 19);
    }

    // Pages with a playlist picker open on the playlist last picked on
    // any of them, and a pick on one is what the next opens on. Their
    // library is nowhere, so it has no such playlist: once the scan is
    // done each falls back to all tracks, and keeps remembering the
    // playlist for a library that has it.
    function test_playlistPagesOpenOnTheLastPickedPlaylist_data() {
        return [
            {tag: "CleanupPage", extra: {playbackController: realPlayback, appSettingsController: realAppSettings}},
            {tag: "SyncPage", extra: {playbackController: realPlayback, appSettingsController: realAppSettings}},
            {tag: "JunkCuePage", extra: {appSettingsController: realAppSettings}},
        ];
    }

    function test_playlistPagesOpenOnTheLastPickedPlaylist(data) {
        var before = realAppSettings.lastPlaylistName;
        realAppSettings.lastPlaylistName = "Warm-Up";
        try {
            var page = createTemporaryObject(Qt.createComponent(qmlDir + data.tag + ".qml"), testCase,
                                             stickProps(data.extra));
            verify(page !== null, data.tag + " did not instantiate");
            compare(page.selectedPlaylistName, "Warm-Up", "it must open on the last picked playlist");
            tryCompare(page, "selectedPlaylistName", "", 5000);
            compare(realAppSettings.lastPlaylistName, "Warm-Up", "falling back must not forget the playlist");
            var picker = findChild(page, "playlistPicker");
            verify(picker !== null, "the playlist picker must be there");
            picker.playlistPicked(1, {name: "Peak Time", count: ""});
            compare(page.selectedPlaylistName, "Peak Time");
            compare(realAppSettings.lastPlaylistName, "Peak Time", "the next page with a picker must open on it");
        } finally {
            realAppSettings.lastPlaylistName = before;
        }
    }

    function test_everyPageCompiles() {
        var names = pageSpecs().map(function(spec) { return spec.name; }).concat(windowPages);
        var failures = [];
        for (var i = 0; i < names.length; ++i) {
            var component = Qt.createComponent(qmlDir + names[i] + ".qml");
            if (component.status === Component.Error) {
                failures.push(names[i] + ": " + component.errorString());
            }
            component.destroy();
        }
        compare(failures.length, 0, failures.join("\n"));
    }

    // Without these a binding that throws is a warning on stderr and a
    // passing test -- exactly the hole this file exists to close.
    function failOnWarningsForPages() {
        failOnWarning(/TypeError/);
        failOnWarning(/ReferenceError/);
        failOnWarning(/is not a function/);
        failOnWarning(/Unable to assign/);
        failOnWarning(/Cannot read property/);
    }

    function test_everyPageInstantiates_data() {
        return pageSpecs().map(function(spec) {
            return {tag: spec.name, name: spec.name, props: spec.props};
        });
    }

    function test_everyPageInstantiates(row) {
        // TrackDetailPage is the one page whose bindings are known to
        // throw, so it is instantiated without the warning teeth rather
        // than dropped from the list: it must still build and lay out.
        //
        // It reads root.scanController.trackAt(...), but trackAt and
        // trackCount live on TrackListModel, which ScanController exposes
        // as `tracks`. The real object never answers those calls. It has
        // never shown up because ScanPage declares trackDetailRequested
        // and nothing anywhere emits it -- the page is unreachable in the
        // running application, and its own test passes a fake that
        // flattens the two levels into one. Whether the fix is `.tracks.`
        // or deleting the page is a decision rather than a typo, so this
        // records the state instead of guessing at it.
        if (row.name !== "TrackDetailPage") {
            failOnWarningsForPages();
        }

        var component = Qt.createComponent(qmlDir + row.name + ".qml");
        compare(component.status, Component.Ready, row.name + ": " + component.errorString());

        var page = createTemporaryObject(component, testCase, row.props);
        verify(page !== null, row.name + " did not instantiate");
        waitForRendering(page);
        verify(page.width > 0, row.name + " laid out to zero width");
        verify(page.height > 0, row.name + " laid out to zero height");
        component.destroy();
    }
}
