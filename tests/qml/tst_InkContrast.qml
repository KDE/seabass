// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtTest
import SeabassGui
import "PixelScale.js" as PixelScale

// Text that nobody gave a colour takes the STYLE's ink, while the ground
// under it is often Theme's (a page header, an overlay card). Under KDE's
// own style that ink comes from the desktop's colour scheme, and on a light
// scheme it was near-black on Kelp's near-black: the page title, the restore
// overlay's heading and its counts, a busy overlay's label, all invisible
// at once and only in the style the Linux build ships with. See
// gui/app_color_scheme.hpp.
//
// Measured on the picture, not on `color`: the property can read right
// while the pixels do not (and it is the pixels that went wrong here).
// The ground is the colour most of the label's box is painted; the ink is
// the pixel furthest from it in luminance. WCAG's ratio between the two
// has to reach 3:1, the floor for large or bold text; the failure this
// guards against measured about 1.1.
TestCase {
    id: testCase
    name: "InkContrast"
    width: 900
    height: 900
    visible: true
    when: windowShown

    Component {
        id: restoreComponent
        RestoreStickBackupPage { width: 880; height: 880 }
    }

    Component {
        id: busyComponent
        Page {
            width: 600
            height: 400
            property alias overlay: busy
            BusyOverlay {
                id: busy
                anchors.fill: parent
                busy: true
                label: "Scanning the stick"
            }
        }
    }

    function channel(c) {
        return c <= 0.03928 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4);
    }

    function luminance(color) {
        return 0.2126 * channel(color.r) + 0.7152 * channel(color.g) + 0.0722 * channel(color.b);
    }

    // The contrast ratio of `item`'s text against its own ground, read off
    // `image`, a grab of `grabbed`.
    function inkContrast(image, grabbed, item) {
        const origin = item.mapToItem(grabbed, 0, 0);
        const w = Math.max(1, item.width);
        const h = Math.max(1, item.height);
        const counts = {};
        const samples = [];
        for (let y = 0; y < h; y += 0.5) {
            for (let x = 0; x < w; x += 0.5) {
                const c = PixelScale.pixel(image, grabbed, origin.x + x, origin.y + y);
                const key = Math.round(c.r * 255) + "," + Math.round(c.g * 255) + "," + Math.round(c.b * 255);
                counts[key] = (counts[key] || 0) + 1;
                samples.push(c);
            }
        }
        let groundKey = "";
        let best = -1;
        for (const key in counts) {
            if (counts[key] > best) {
                best = counts[key];
                groundKey = key;
            }
        }
        const parts = groundKey.split(",");
        const ground = luminance(Qt.rgba(parts[0] / 255, parts[1] / 255, parts[2] / 255, 1));
        let ink = ground;
        for (let i = 0; i < samples.length; ++i) {
            const l = luminance(samples[i]);
            if (Math.abs(l - ground) > Math.abs(ink - ground)) {
                ink = l;
            }
        }
        return (Math.max(ink, ground) + 0.05) / (Math.min(ink, ground) + 0.05);
    }

    function findText(item, predicate) {
        for (let i = 0; i < item.children.length; ++i) {
            const child = item.children[i];
            if (child.visible && child.text !== undefined && typeof child.text === "string"
                    && child.contentWidth !== undefined && predicate(child.text)) {
                return child;
            }
            const inner = findText(child, predicate);
            if (inner !== null) return inner;
        }
        return null;
    }

    function fakeRestoreController() {
        return {
            disks: [{label: "STICK", mountPoint: "/media/STICK", devicePath: "/dev/sdb1", wholeDiskPath: "/dev/sdb",
                     capacityBytes: 64 * 1024 * 1024 * 1024, mounted: true, hasNoFilesystem: false,
                     hasDjLibrary: false, usable: true, rootEntries: []}],
            archivePath: "/home/u/Seabass Backups/STICK.zip",
            defaultBackupDirectory: "/home/u/Seabass Backups",
            archiveInfo: {error: "", label: "STICK", identifier: "uuid", status: "complete",
                          createdAt: "2026-09-03T21:14:00", entries: 1161, bytes: 25 * 1024 * 1024 * 1024},
            preview: {filesToWrite: 14, filesUnchanged: 1147, bytesToWrite: 500 * 1024 * 1024, extras: 0,
                      targetHasEngineLibrary: false, freeBytes: 60 * 1024 * 1024 * 1024, enoughFreeSpace: true},
            result: {filesWritten: 14, filesUnchanged: 1147, directoriesCreated: 3, extrasRemoved: 0, rejected: [],
                     writeErrors: [], warnings: [], missingTracks: [], databaseChecked: true},
            busy: false, restoring: false, analyzing: false, phase: "",
            filesDone: 0, filesTotal: 0, bytesDone: 0, bytesTotal: 0, bytesPerSecond: 0, etaSeconds: -1,
            currentFile: "", errorMessage: "", statusMessage: "Restored STICK from its backup.",
            knownBackups: [], listingBackups: false,
            refresh: function() {}, refreshKnownBackups: function() {}, analyze: function() {},
            restore: function() {}, restoreAnyway: function() {}, cancel: function() {},
            clearResult: function() {}, mount: function() {},
            archivePathForLabel: function(label) { return this.defaultBackupDirectory + "/" + label + ".zip"; },
        };
    }

    // The restore page's unstyled labels that measure under 3:1, named
    // with their ratios; empty when all of them read.
    function unreadableOnTheRestorePage(screenshotName) {
        const page = createTemporaryObject(restoreComponent, appWindow, {
            controller: fakeRestoreController(),
            appSettingsController: {toLocalFileUrl: function(p) { return "file://" + p; },
                                    localPathFromUrl: function(u) { return u.replace(/^file:\/\//, ""); }},
        });
        verify(page !== null);
        waitForRendering(page);
        const image = grabImage(page);
        if (screenshotName && screenshotDir && screenshotDir.length > 0) {
            image.save(screenshotDir + "/" + screenshotName);
        }
        const overlay = findChild(page, "restoreOverlay");
        verify(overlay.visible);
        const cases = [
            ["the breadcrumb's page title", page, function(t) { return t === "Restore a Stick Backup"; }],
            ["the overlay's heading", overlay, function(t) { return t === "Restore finished"; }],
            ["the report's heading", overlay, function(t) { return t === "Result"; }],
            ["the report's counts", overlay, function(t) { return t.indexOf("14 written") === 0; }],
        ];
        // Every one measured before failing, so a red run names them all.
        const unreadable = [];
        for (let i = 0; i < cases.length; ++i) {
            const label = findText(cases[i][1], cases[i][2]);
            verify(label !== null, cases[i][0] + " must be on the page");
            const ratio = inkContrast(image, page, label);
            if (ratio < 3.0) {
                unreadable.push(cases[i][0] + " at " + ratio.toFixed(2) + ":1");
            }
        }
        return unreadable.join("; ");
    }

    function busyLabelContrast() {
        const holder = createTemporaryObject(busyComponent, appWindow);
        verify(holder !== null);
        waitForRendering(holder);
        const image = grabImage(holder);
        const label = findText(holder.overlay, function(t) { return t === "Scanning the stick"; });
        verify(label !== null);
        return inkContrast(image, holder, label);
    }

    function test_restorePageTitleAndFinishedOverlayAreReadable() {
        compare(unreadableOnTheRestorePage(), "", "under 3:1");
    }

    function test_busyOverlayLabelIsReadable() {
        const ratio = busyLabelContrast();
        verify(ratio >= 3.0, "the busy overlay's label reads at " + ratio.toFixed(2) + ":1, under 3:1");
    }

    // Preferences flips useSystemTheme while the app runs, and Theme
    // repaints at once. The style's ink has to follow in the same moment:
    // it used to take Kelp's colour scheme once at startup and keep it, so
    // "Match System Theme" on a light Plasma session drew Kelp's
    // near-white on the light Theme until the next start. Toggled through
    // the real AppSettingsController, the one Preferences writes to, with
    // Theme fed what Main.qml's Bindings feed it.
    //
    // The pages are made inside `appWindow`, which carries Main.qml's own
    // Material.theme binding, because that is where the app's pages sit:
    // under Material (the macOS style) an unstyled Label's ink is the
    // Material theme of the nearest item up the tree that sets one. They
    // used to be made directly in the TestCase, which sets none, so their
    // ink was the harness's global default, Material Dark (the screenshot
    // mode's QT_QUICK_CONTROLS_MATERIAL_THEME), while Theme went light
    // from a separate item: 1.10:1 on the Mac in round 9, for a
    // combination the app cannot produce. Main.qml's window sets
    // Material.theme for everything under it, popups included, and feeds
    // Theme from the same window.
    //
    // Which session: under KDE's style the ink comes from KDE's colour
    // scheme, and the lane's sandbox has no kdeglobals, so the system
    // scheme is KDE's default, Breeze Light: the light session the bug
    // needs, and Theme gets Material's light colours to match. Under
    // Material the light session is stood in the same way, Material.Light
    // where the app says Material.System (which is what System resolves to
    // on a light Mac): otherwise the case follows the machine's desktop,
    // and on a dark one the "light session" is dark and passes for the
    // wrong reason. Under any other style the ink is the platform palette,
    // which follows the same system colour scheme Material.System does, so
    // the window keeps Main.qml's binding exactly.
    AppSettingsController { id: appSettings }
    Item { id: lightSession; Material.theme: Material.Light }
    Item {
        id: appWindow
        anchors.fill: parent
        readonly property int systemTheme: materialStyle ? Material.Light : Material.System
        Material.theme: appSettings.useSystemTheme ? systemTheme : Material.Dark
    }

    function chooseSystemTheme(on) {
        const session = kdeDesktopStyle ? lightSession : appWindow;
        appSettings.useSystemTheme = on;
        Theme.useSystemTheme = on;
        Theme.materialBackground = session.Material.background;
        Theme.materialForeground = session.Material.foreground;
        Theme.materialDivider = session.Material.dividerColor;
    }

    function cleanup() {
        appSettings.useSystemTheme = false;
        Theme.useSystemTheme = false;
        Theme.materialBackground = "#121212";
        Theme.materialForeground = "#e0e0e0";
        Theme.materialDivider = "#33ffffff";
    }

    function test_themeToggleKeepsTheInkReadableBothWays() {
        chooseSystemTheme(true);
        if (kdeDesktopStyle || materialStyle) {
            verify(Theme.isLightBackground, "the light session is standing in");
        }
        compare(unreadableOnTheRestorePage("ink-contrast-system-light.png"), "", "system theme on a light session, under 3:1");
        const busyLight = busyLabelContrast();
        verify(busyLight >= 3.0, "system theme: the busy label reads at " + busyLight.toFixed(2) + ":1");
        chooseSystemTheme(false);
        verify(!Theme.isLightBackground, "Kelp again");
        compare(unreadableOnTheRestorePage("ink-contrast-kelp-again.png"), "", "back to Kelp, under 3:1");
        const busyKelp = busyLabelContrast();
        verify(busyKelp >= 3.0, "back to Kelp: the busy label reads at " + busyKelp.toFixed(2) + ":1");
    }
}
