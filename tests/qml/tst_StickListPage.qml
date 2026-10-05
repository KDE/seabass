// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui
import "../qml-live/LiveHelpers.js" as Live

// StickListPage.qml headless with fake controllers: the rail and the pane
// beside it (which stick and which group are selected, and what that
// selection follows), which cards an empty stick and a library stick show
// once the backup advisor has spoken, and what the clone / update cards
// request. Also the page's screenshot, in every state the home has, when
// SEABASS_SCREENSHOT_DIR is set.
TestCase {
    id: testCase
    name: "StickListPage"
    width: 1100
    height: 1000
    visible: true
    when: windowShown

    Component {
        id: pageComponent
        StickListPage { width: 1080; height: 980 }
    }

    function makeStick(overrides) {
        var s = {
            label: "MAIN", mountPoint: "/media/MAIN", devicePath: "/dev/sdb1", mounted: true,
            hasRekordbox: true, hasEngine: true, rekordboxPath: "/media/MAIN/PIONEER",
            enginePath: "/media/MAIN/Engine Library", isSdCard: false, isFolder: false,
            isBrowsedBackup: false, libraryId: "lib-main", safeToUnplug: false, hasOneLibrary: false,
            readOnly: false,
        };
        for (var key in overrides) {
            s[key] = overrides[key];
        }
        return s;
    }

    function noSource() {
        return {kind: "none", label: "", mountPoint: "", backupPath: "", modifiedAt: "", enoughSpace: true, detail: "",
                rekordboxPath: "", enginePath: ""};
    }

    function makeAdvice(overrides) {
        var a = {state: "no-backups", matchedBy: "none", backupPath: "", backupLabel: "", backupCreatedAt: "",
                 trackOverlap: -1, cueOverlap: -1, detail: "No backup of this library yet.",
                 cloneSource: noSource(), updateSource: noSource(), diverged: false};
        for (var key in overrides) {
            a[key] = overrides[key];
        }
        return a;
    }

    function makePage(sticks, advice, overrides) {
        var props = {
            mediaController: {sticks: sticks, errorMessage: "", busy: false, busyDevicePath: "", calls: [],
                              mountStick: function(d) { this.calls.push("mount:" + d); },
                              unmountStick: function(d) { this.calls.push("unmount:" + d); },
                              openFolder: function(p) { this.calls.push("openFolder:" + p); return ""; },
                              closeFolder: function(p) { this.calls.push("closeFolder:" + p); },
                              openBackup: function(p) { this.calls.push("openBackup:" + p); return ""; }},
            playbackController: {stop: function() {}},
            appSettingsController: fakeAppSettings(),
            backupAdvisor: {advice: advice, calls: [],
                            assess: function(l, m, r, e) { this.calls.push("assess:" + m); },
                            reassessAll: function() { this.calls.push("reassessAll"); },
                            forget: function(m) { this.calls.push("forget:" + m); }},
        };
        for (var key in (overrides || {})) {
            props[key] = overrides[key];
        }
        var page = createTemporaryObject(pageComponent, testCase, props);
        verify(page !== null);
        waitForRendering(page);
        return page;
    }

    // Rows and cards by the objectNames StickListPage gives them, shared
    // with the live tests: one place that knows how the page is built.
    // Finding a card selects its stick and its group, as the rail would;
    // the frame after that is what a click at the card's centre needs.
    function findCard(page, mountPoint, title) {
        const card = Live.cardInRow(page, mountPoint, title);
        if (card !== null && card.visible) {
            waitForRendering(page);
        }
        return card;
    }
    function findRowObject(page, mountPoint, objectName) {
        return Live.objectInRow(page, mountPoint, objectName);
    }

    function saveScreenshot(page, name) {
        if (!screenshotDir || screenshotDir.length === 0) return;
        var image = grabImage(page);
        image.save(screenshotDir + "/" + name + ".png");
    }

    // What StickListPage reads and calls on the real AppSettingsController.
    function fakeAppSettings() {
        return {
            experimentalFeaturesEnabled: true, stickBackupDirectory: "/tmp", homeGroup: "explore",
            toLocalFileUrl: function(p) { return "file://" + p; },
            localPathFromUrl: function(u) { return u.replace(/^file:\/\//, ""); },
        };
    }

    function fakeEditRegistry(lockedIds) {
        return {
            lockedByOther: lockedIds, calls: [],
            refreshLocks: function() { this.calls.push("refresh"); },
            removeLock: function(id) { this.calls.push("remove:" + id); },
            lockHolder: function(id) { return {hostname: "studio-pc", pid: 4242, startedAtUtc: "2026-09-06T10:00:00Z"}; },
            libraryIdForPath: function(p) { return "lib-main"; },
            // What the real registry always has; tests override as needed.
            hasSession: function(id) { return false; },
            sessionFor: function(id) { return null; },
            closeSession: function(id) { this.calls.push("closeSession:" + id); },
        };
    }

    // Another instance holds the library's edit lock: every card that
    // would change the library is read-only and explains itself when
    // clicked; Browse stays a plain card.
    function test_readOnlyCardsWhileAnotherInstanceEdits() {
        var page = makePage([makeStick({})], {}, {editRegistry: fakeEditRegistry(["lib-main"])});
        // Browse first: finding Housekeeping switches the pane to its
        // group, Maintain, where the click below has to land.
        var browse = findCard(page, "/media/MAIN", "Browse Library");
        verify(browse !== null);
        compare(browse.readOnly, false);
        compare(findChild(browse, "readOnlyBadge").visible, false);
        var housekeeping = findCard(page, "/media/MAIN", "Housekeeping");
        verify(housekeeping !== null);
        compare(page.selectedGroup, "maintain");
        compare(housekeeping.readOnly, true);
        compare(findChild(housekeeping, "readOnlyBadge").visible, true);

        var spy = createTemporaryObject(spyComponent, testCase, {target: page, signalName: "duplicateTracksHubRequested"});
        mouseClick(housekeeping);
        compare(spy.count, 0);
        var dialog = findChild(page, "lockedDialog");
        tryCompare(dialog, "opened", true);
        compare(dialog.libraryId, "lib-main");
        verify(findChild(dialog, "holderLabel").text.indexOf("studio-pc") >= 0);
        saveScreenshot(page, "stick-list-read-only");

        findChild(dialog, "removeLockButton").clicked();
        tryCompare(dialog, "opened", false);
        tryCompare(dialog, "visible", false);  // the modal overlay eats clicks until the exit is over
        compare(page.editRegistry.calls.indexOf("remove:lib-main") >= 0, true);

        // No lock: the same card opens the feature.
        page.editRegistry = fakeEditRegistry([]);
        compare(housekeeping.readOnly, false);
        mouseClick(housekeeping);
        compare(spy.count, 1);
    }

    // A stick the kernel mounted read-only (a damaged filesystem after an
    // unclean unplug) can take no writes at all: every writing card goes
    // read-only and a click sends the user to Library Health, which is
    // where the filesystem repair lives -- and Library Health itself, plus
    // the cards that only read, stay ordinary.
    function test_readOnlyStickGreysOutEveryWritingCard() {
        var page = makePage([makeStick({readOnly: true})], {});
        var writers = ["Housekeeping", "Restore Metadata", "Sync Cue Points", "USB Stick Performance"];
        for (var i = 0; i < writers.length; ++i) {
            var card = findCard(page, "/media/MAIN", writers[i]);
            verify(card !== null, writers[i] + " missing");
            compare(card.readOnly, true, writers[i] + " should be read-only");
            verify(card.readOnlyReason.indexOf("Library Health") >= 0, writers[i] + " should point at Library Health");
        }
        var health = findCard(page, "/media/MAIN", "Library Health");
        verify(health !== null);
        compare(health.readOnly, false);
        compare(findCard(page, "/media/MAIN", "Browse Library").readOnly, false);
        compare(findCard(page, "/media/MAIN", "Library Statistics").readOnly, false);

        // Clicking a greyed card does not start the feature; it opens the
        // page that can fix the stick.
        var housekeepingSpy = createTemporaryObject(spyComponent, testCase,
            {target: page, signalName: "duplicateTracksHubRequested"});
        var healthSpy = createTemporaryObject(spyComponent, testCase, {target: page, signalName: "libraryHealthRequested"});
        mouseClick(findCard(page, "/media/MAIN", "Housekeeping"));
        compare(housekeepingSpy.count, 0);
        compare(healthSpy.count, 1);
        saveScreenshot(page, "stick-list-read-only-stick");
    }

    // A ListModel stands in for the real model where rows come and go:
    // it has count, get(i) and the inserts and removes the page follows.
    Component {
        id: listModelComponent
        ListModel {}
    }
    function makeModel(sticks) {
        const model = createTemporaryObject(listModelComponent, testCase);
        for (let i = 0; i < sticks.length; ++i) {
            model.append(sticks[i]);
        }
        return model;
    }
    function noLibrary(overrides) {
        const s = makeStick({label: "SPARE", mountPoint: "/media/SPARE", devicePath: "/dev/sdc1",
                             hasRekordbox: false, hasEngine: false, rekordboxPath: "", enginePath: "", libraryId: ""});
        for (const key in (overrides || {})) {
            s[key] = overrides[key];
        }
        return s;
    }

    // On load the first stick with a library is selected, not simply the
    // first stick: an empty stick listed first would open the home on a
    // pane with nothing to do.
    function test_theFirstStickWithALibraryIsSelectedOnLoad() {
        const page = makePage([noLibrary({}), makeStick({})], {});
        compare(page.selectedStickKey, "/media/MAIN");
        compare(Live.findByObjectName(page, "stickLabel").text, "MAIN");
        // No stick has one: the first.
        const bare = makePage([noLibrary({}), noLibrary({label: "BLANK", mountPoint: "/media/BLANK", devicePath: "/dev/sdd1"})], {});
        compare(bare.selectedStickKey, "/media/SPARE");
        // The rail shows which.
        compare(findByName(page, "railStick:/media/MAIN").selected, true);
        compare(findByName(page, "railStick:/media/SPARE").selected, false);
    }

    // A click on the rail selects that stick; the pane follows.
    function test_clickingARailStickSelectsIt() {
        const page = makePage([makeStick({}), noLibrary({})], {});
        const entry = findByName(page, "railStick:/media/SPARE");
        verify(entry !== null);
        mouseClick(entry);
        compare(page.selectedStickKey, "/media/SPARE");
        compare(findByName(page, "stickLabel").text, "SPARE");
        compare(findByName(page, "stickRow:/media/SPARE").visible, true);
        compare(findByName(page, "noKnownLibraryLabel").visible, true);
    }

    // Sticks come and go at the USB port. The rail animates them in and
    // out (HomeRail's own tests look at the animation), and the selection
    // follows: an arrival is selected only when nothing was, the selected
    // stick leaving selects the first that remains, and mounting, which
    // changes a stick's key from its device to its mount point, keeps it
    // selected.
    function test_theSelectionFollowsSticksArrivingAndLeaving() {
        const model = makeModel([noLibrary({label: "ONE", mountPoint: "/media/ONE", devicePath: "/dev/sdb1"})]);
        const page = makePage(model, {});
        compare(page.selectedStickKey, "/media/ONE");
        verify(findByName(page, "railStick:/media/ONE") !== null, "the rail lists the stick");

        // Another arrives while one is selected: the selection stays.
        model.append(makeStick({label: "TWO", mountPoint: "/media/TWO", devicePath: "/dev/sdc1"}));
        compare(page.selectedStickKey, "/media/ONE");
        tryVerify(() => findByName(page, "railStick:/media/TWO") !== null, 2000, "the rail lists the arrival");
        compare(page.backupAdvisor.calls.indexOf("assess:/media/TWO") >= 0, true, "an arrival is assessed");

        // The selected one leaves: the first that remains.
        model.remove(0);
        compare(page.selectedStickKey, "/media/TWO");
        compare(findByName(page, "stickLabel").text, "TWO");
        verify(findByName(page, "railStickLeaving:/media/ONE") !== null, "the rail shows the stick going");

        // Another stick listed ahead of it, so "the first that remains"
        // would be the wrong answer to the key changing below.
        model.insert(0, noLibrary({label: "ZERO", mountPoint: "/media/ZERO", devicePath: "/dev/sde1"}));
        compare(page.selectedStickKey, "/media/TWO");

        // Unmounted and mounted again: the same stick, still selected,
        // and assessed again once it is back.
        const assessedBefore = page.backupAdvisor.calls.filter((c) => c === "assess:/media/TWO").length;
        model.setProperty(1, "mountPoint", "");
        model.setProperty(1, "mounted", false);
        compare(page.selectedStickKey, "/dev/sdc1");
        compare(findByName(page, "unmountedLabel").visible, true);
        model.setProperty(1, "mounted", true);
        model.setProperty(1, "mountPoint", "/media/TWO");
        compare(page.selectedStickKey, "/media/TWO");
        compare(page.backupAdvisor.calls.filter((c) => c === "assess:/media/TWO").length, assessedBefore + 1);

        // The last ones leave: nothing selected, the no-stick pane.
        model.remove(1);
        compare(page.selectedStickKey, "/media/ZERO");
        model.remove(0);
        compare(page.selectedStickKey, "");
        compare(findByName(page, "noStickHeading").visible, true);
        compare(findByName(page, "railNoSticks").visible, true);

        // One arrives while none is selected: that one.
        model.append(noLibrary({label: "THREE", mountPoint: "/media/THREE", devicePath: "/dev/sdd1"}));
        compare(page.selectedStickKey, "/media/THREE");
        compare(findByName(page, "noStickHeading").visible, false);
    }

    // The group is remembered across runs, through the settings; a value
    // nobody recognises falls back to Explore.
    function test_theGroupIsRememberedAcrossRuns() {
        const settings = fakeAppSettings();
        settings.homeGroup = "maintain";
        const page = makePage([makeStick({})], {}, {appSettingsController: settings});
        compare(page.selectedGroup, "maintain");
        compare(findByName(page, "railGroup:maintain").selected, true);
        compare(findByName(page, "groupHeadingName").text, "Maintain");
        compare(findByName(page, "groupHeadingDescription").text, "Find and fix what is wrong");

        mouseClick(findByName(page, "railGroup:sync"));
        compare(page.selectedGroup, "sync");
        compare(page.appSettingsController.homeGroup, "sync", "the choice is stored for the next run");
        compare(findByName(page, "groupHeadingName").text, "Sync");
        compare(findByName(page, "groupHeadingDescription").text, "Keep cues and catalogs in step");
        compare(findCard(page, "/media/MAIN", "Sync Cue Points").visible, true);
    }

    Component {
        id: realAppSettingsComponent
        AppSettingsController {}
    }

    // What the page is handed is a group it knows: the settings refuse a
    // stored value nobody recognises when they read it, as they do when
    // it is set, so a hand-edited or foreign settings file opens the home
    // on Explore rather than on no group at all.
    function test_aStoredGroupNobodyKnowsReadsAsExplore() {
        const before = controllerFixture.storeRawSetting("homeGroup", "sync");
        try {
            // The store the fixture wrote is the one the controller reads:
            // otherwise "explore" below would only be the default.
            compare(createTemporaryObject(realAppSettingsComponent, testCase).homeGroup, "sync");
            controllerFixture.storeRawSetting("homeGroup", "bogus");
            const settings = createTemporaryObject(realAppSettingsComponent, testCase);
            compare(settings.homeGroup, "explore");
            const page = makePage([makeStick({})], {}, {appSettingsController: settings});
            compare(page.selectedGroup, "explore");
            // Gone before the settings are: a page outliving its
            // controller reads null in its bindings on the way out.
            page.destroy();
            wait(0);
        } finally {
            controllerFixture.storeRawSetting("homeGroup", before);
        }
    }

    // A dataChanged about one stick re-copies that stick's row alone: the
    // selected stick's row stays the very object the pane was handed, so
    // nothing bound to it is worked out again for another stick's change.
    function test_anotherStickChangingKeepsTheSelectedRow() {
        const model = makeModel([makeStick({}), noLibrary({})]);
        const page = makePage(model, {});
        compare(page.selectedStickKey, "/media/MAIN");
        const selected = page.selectedRow;
        const spare = page.rows[1];
        verify(selected !== null);

        model.setProperty(1, "label", "RENAMED");
        compare(page.rows[1].label, "RENAMED", "the changed row is copied again");
        verify(page.rows[1] !== spare, "as a new object");
        verify(page.selectedRow === selected, "the selected row is the same object");
        verify(page.rows[0] === selected);
        compare(findByName(page, "railStick:/media/SPARE").text, "RENAMED");

        // Its own change does reach it.
        model.setProperty(0, "label", "MAIN2");
        verify(page.selectedRow !== selected);
        compare(page.selectedRow.label, "MAIN2");
        compare(findByName(page, "stickLabel").text, "MAIN2");
    }

    // A card below the fold is scrolled into view before a test clicks
    // it (Live.scrollIntoView): a click at the centre of a card outside
    // the pane lands on nothing. A short window, as a laptop with the
    // update banner showing has, puts Backup's second row out of sight.
    function test_aCardBelowTheFoldIsScrolledIntoView() {
        const page = makePage([makeStick({})], {}, {height: 420});
        const pane = findByName(page, "homePane");
        const card = findCard(page, "/media/MAIN", "Restore Metadata");
        verify(card !== null && card.visible);
        const inPane = () => card.mapToItem(pane, 0, 0).y;
        verify(inPane() + card.height > pane.height,
               "the card starts out below the pane's bottom edge (y " + inPane() + ", pane " + pane.height + ")");

        verify(Live.scrollIntoView(page, card), "scrolled into view");
        waitForRendering(page);
        verify(inPane() >= 0 && inPane() + card.height <= pane.height,
               "inside the pane (y " + inPane() + ", height " + card.height + ", pane " + pane.height + ")");
        const inPage = card.mapToItem(page, 0, 0).y;
        verify(inPage >= 0 && inPage + card.height <= page.height, "and so inside the window");
        const spy = createTemporaryObject(spyComponent, testCase, {target: page, signalName: "metadataRestoreRequested"});
        mouseClick(card);
        compare(spy.count, 1, "a click at its centre reaches it");
    }

    // One left line for the page and one for the pane: the brand stands
    // on the rail's text line (the pills reach left of it by their inset),
    // and the stick's name, the group heading and the cards' titles share
    // an edge of their own.
    function test_thePaneTextSharesOneLeftEdge() {
        const page = makePage([makeStick({})], {});
        const x = (item) => Math.round(item.mapToItem(page, 0, 0).x);
        const sticksLabel = findByName(page, "railSticksLabel");
        verify(sticksLabel !== null, "the rail has a section label to line up with");
        compare(x(findByName(page, "brandLockup")), x(sticksLabel) + Math.round(sticksLabel.leftPadding),
                "the brand stands on the rail's text line, where its section labels and icons start");
        compare(x(findByName(page, "brandLockup")), x(findByName(page, "homeRail")) + Theme.crumbTextInset);
        const name = findByName(page, "stickLabel");
        const heading = findByName(page, "groupHeadingName");
        const card = findCard(page, "/media/MAIN", "Browse Library");
        const title = findChild(card, "cardTitleLabel");
        compare(x(heading), x(name), "the group heading starts under the stick's name");
        compare(x(title), x(name), "the cards' titles start under the stick's name");
        verify(x(name) > x(findByName(page, "homePane")), "inside the pane");
    }

    // A group with nothing for this stick says so rather than showing a
    // blank under its heading.
    function test_anEmptyGroupSaysSo() {
        const page = makePage([noLibrary({})], {});
        page.selectGroup("sync");
        const nothing = findByName(page, "nothingHereLabel");
        verify(nothing !== null);
        compare(nothing.visible, true);
        compare(nothing.text, "Nothing here for this stick.");
        waitForRendering(page);
        saveScreenshot(page, "stick-list-no-library-sync");
        page.selectGroup("backup");
        compare(nothing.visible, false);
    }

    // Tab goes from the rail to the pane: the stick row, then the cards.
    function test_tabGoesFromTheRailToTheCards() {
        const page = makePage([makeStick({})], {});
        const tools = findByName(page, "railToolKeys");
        tools.forceActiveFocus();
        verify(tools.activeFocus);
        keyClick(Qt.Key_Down);
        keyClick(Qt.Key_Return);
        compare(page.selectedGroup, "sync", "Down and Enter on the rail pick the next group");
        // The stick row's eject button is the pane's first stop, then the
        // group's cards.
        keyClick(Qt.Key_Tab);
        compare(page.Window.activeFocusItem, findByName(page, "ejectButton"));
        keyClick(Qt.Key_Tab);
        const focused = page.Window.activeFocusItem;
        verify(focused !== null && focused.cardTitle !== undefined,
               "Tab after the stick row lands on a card, not on " + focused);
        compare(focused.cardTitle, "Sync Cue Points");
    }

    function test_everyMountedStickIsAssessed() {
        var page = makePage([makeStick({}), makeStick({label: "SPARE", mountPoint: "/media/SPARE", devicePath: "/dev/sdc1",
                                                       hasRekordbox: false, hasEngine: false, rekordboxPath: "", enginePath: ""})], {});
        compare(page.backupAdvisor.calls.indexOf("assess:/media/MAIN") >= 0, true);
        compare(page.backupAdvisor.calls.indexOf("assess:/media/SPARE") >= 0, true);
    }

    // USB Stick Performance needs no library: an empty mounted stick gets
    // the card, and it carries the mount point the page measures at.
    function test_emptyStickCanBeMeasured() {
        var spare = makeStick({label: "SPARE", mountPoint: "/media/SPARE", devicePath: "/dev/sdc1",
                               hasRekordbox: false, hasEngine: false, rekordboxPath: "", enginePath: ""});
        var advice = {};
        advice["/media/SPARE"] = makeAdvice({state: "no-backups"});
        var page = makePage([spare], advice);
        var card = findCard(page, "/media/SPARE", "USB Stick Performance");
        verify(card !== null);
        compare(card.visible, true);
        compare(card.enabled, true);
        var spy = createTemporaryObject(spyComponent, testCase, {target: page, signalName: "stickPerformanceRequested"});
        card.clicked();
        compare(spy.count, 1);
        compare(spy.signalArguments[0][0], "SPARE");
        compare(spy.signalArguments[0][3], "/media/SPARE");
        saveScreenshot(page, "stick-list-no-library");
    }

    // Graduated on 2026-09-17: the card is there with the experimental
    // setting off, and so is every other card that used to be gated. Only
    // Create Engine Library still answers to the setting.
    function test_onlyCreateEngineLibraryAnswersToTheExperimentalSetting() {
        var settings = fakeAppSettings();
        settings.experimentalFeaturesEnabled = false;
        var page = makePage([makeStick({hasEngine: false, enginePath: ""})], {},
                            {appSettingsController: settings});
        var shown = ["USB Stick Performance", "Format USB Stick", "Metadata"];
        for (var i = 0; i < shown.length; ++i) {
            var card = findCard(page, "/media/MAIN", shown[i]);
            verify(card !== null && card.visible, shown[i] + " must not be behind the experimental setting");
            compare(card.experimental, false, shown[i] + " must not be marked experimental");
        }
        var engine = findCard(page, "/media/MAIN", "Create Engine Library");
        verify(engine === null || !engine.visible, "Create Engine Library must stay behind the setting");
    }

    function test_emptyStickOffersCloneFromThePeer() {
        var spare = makeStick({label: "SPARE", mountPoint: "/media/SPARE", devicePath: "/dev/sdc1",
                               hasRekordbox: false, hasEngine: false, rekordboxPath: "", enginePath: ""});
        var advice = {};
        advice["/media/MAIN"] = makeAdvice({state: "current", detail: "Backup is up to date."});
        advice["/media/SPARE"] = makeAdvice({state: "restore", backupPath: "/b/MAIN.zip", backupLabel: "MAIN",
            detail: "The newest backup can be restored onto this empty stick.",
            cloneSource: {kind: "stick", label: "MAIN", mountPoint: "/media/MAIN", backupPath: "", modifiedAt: "2026-09-06T10:00:00",
                          enoughSpace: true, detail: "Copy MAIN's library onto this stick.",
                          rekordboxPath: "/media/MAIN/PIONEER", enginePath: "/media/MAIN/Engine Library"}});
        var page = makePage([makeStick({}), spare], advice);
        // MAIN first: one stick's section is shown at a time, so each
        // card is read while its own stick is the selected one.
        verify(findCard(page, "/media/MAIN", "Create Backup USB Stick").visible === false);
        compare(findCard(page, "/media/MAIN", "Update Stick").visible, false, "no newer copy anywhere");
        // A stick with a library has its own Restore Backup: its full
        // backup put back.
        compare(findCard(page, "/media/MAIN", "Restore Backup").visible, true);
        compare(findCard(page, "/media/MAIN", "Restore Backup").cardSubtitle,
                "Put a full stick backup from this computer onto this stick");
        var card = findCard(page, "/media/SPARE", "Create Backup USB Stick");
        verify(card !== null);
        compare(card.visible, true);
        compare(card.enabled, true);
        compare(card.cardSubtitle, "Copy MAIN's library onto this stick.");
        // Restoring the backup is offered beside the copy, not instead of it.
        var restoreCard = findCard(page, "/media/SPARE", "Restore Backup");
        compare(restoreCard.visible, true);
        compare(restoreCard.cardSubtitle, "Restore MAIN's library onto this stick");
        // The stick's row says the same, under its name.
        verify(findByName(page, "noKnownLibraryLabel").text.indexOf("Copy MAIN's library onto this stick.") >= 0);

        var spy = createTemporaryObject(spyComponent, testCase, {target: page, signalName: "cloneStickRequested"});
        card.clicked();
        compare(spy.count, 1);
        compare(spy.signalArguments[0][0], "MAIN");
        compare(spy.signalArguments[0][1], "/media/MAIN/PIONEER");
        compare(spy.signalArguments[0][2], "/media/MAIN/Engine Library");
        compare(spy.signalArguments[0][3], "/media/SPARE");
        compare(spy.signalArguments[0][4], "SPARE");
        compare(spy.signalArguments[0][5], false);
        saveScreenshot(page, "stick-list-clone");
    }

    function test_cloneCardDisabledWithoutSpace() {
        var spare = makeStick({label: "SPARE", mountPoint: "/media/SPARE", devicePath: "/dev/sdc1",
                               hasRekordbox: false, hasEngine: false, rekordboxPath: "", enginePath: ""});
        var advice = {};
        advice["/media/SPARE"] = makeAdvice({cloneSource: {kind: "stick", label: "MAIN", mountPoint: "/media/MAIN", backupPath: "",
            modifiedAt: "", enoughSpace: false, detail: "Not enough space on this stick for MAIN's library.",
            rekordboxPath: "/media/MAIN/PIONEER", enginePath: ""}});
        var page = makePage([makeStick({}), spare], advice);
        var card = findCard(page, "/media/SPARE", "Create Backup USB Stick");
        compare(card.visible, true);
        compare(card.enabled, false);
    }

    // The full stick backup's cards sit in the Backup group itself, and
    // each one's request reaches the page's own signal with the arguments
    // Main.qml's handlers take.
    function test_theBackupCardsReachThePagesSignals() {
        var advice = {};
        advice["/media/MAIN"] = makeAdvice({state: "outdated", detail: "The library has changed since its last backup.",
            matchedBy: "fingerprint", backupPath: "/b/MAIN.zip",
            updateSource: {kind: "stick", label: "SPARE", mountPoint: "/media/SPARE", backupPath: "", modifiedAt: "",
                           enoughSpace: true, detail: "SPARE holds a newer copy of this library.",
                           rekordboxPath: "/media/SPARE/PIONEER", enginePath: "/media/SPARE/Engine Library"}});
        var page = makePage([makeStick({})], advice);
        compare(findCard(page, "/media/MAIN", "Backups"), null, "the Backups card is gone");
        const full = findCard(page, "/media/MAIN", "Full Stick Backup");
        verify(full !== null && full.visible);
        compare(page.selectedGroup, "backup");
        compare(full.cardSubtitle, "Update the full stick backup: The library has changed since its last backup.");
        const update = findCard(page, "/media/MAIN", "Update Stick");
        compare(update.visible, true);
        compare(update.cardSubtitle, "SPARE holds a newer copy of this library.");
        saveScreenshot(page, "stick-list-update");

        const cases = [
            {title: "Full Stick Backup", signalName: "fullStickBackupRequested",
             args: ["MAIN", "/media/MAIN/PIONEER", "/media/MAIN/Engine Library"]},
            {title: "Update Stick", signalName: "cloneStickRequested",
             args: ["SPARE", "/media/SPARE/PIONEER", "/media/SPARE/Engine Library", "/media/MAIN", "MAIN", true]},
            {title: "Restore Backup", signalName: "restoreStickBackupRequested",
             args: ["/media/MAIN", "/dev/sdb1", "/b/MAIN.zip", "MAIN"]},
            {title: "Manage Backups", signalName: "manageBackupsRequested", args: ["MAIN", "/b/MAIN.zip"]},
        ];
        for (const c of cases) {
            const spy = createTemporaryObject(spyComponent, testCase, {target: page, signalName: c.signalName});
            mouseClick(findCard(page, "/media/MAIN", c.title));
            compare(spy.count, 1, c.title + " must reach " + c.signalName);
            compare(JSON.stringify(Array.prototype.slice.call(spy.signalArguments[0])), JSON.stringify(c.args), c.title);
        }
    }

    // While another instance edits the library, the four are read-only
    // and explain the lock, as they did on the stick's Backups page.
    function test_theBackupCardsAreReadOnlyWhileAnotherInstanceEdits() {
        var advice = {};
        advice["/media/MAIN"] = makeAdvice({updateSource: {kind: "disk-backup", label: "MAIN", mountPoint: "",
            backupPath: "/b/MAIN.zip", modifiedAt: "", enoughSpace: true, detail: "The backup is newer.",
            rekordboxPath: "", enginePath: ""}});
        var page = makePage([makeStick({})], advice, {editRegistry: fakeEditRegistry(["lib-main"])});
        for (const title of ["Full Stick Backup", "Update Stick", "Restore Backup", "Manage Backups"]) {
            const card = findCard(page, "/media/MAIN", title);
            verify(card !== null && card.visible, title + " missing");
            compare(card.readOnly, true, title + " must be read-only while locked");
        }
        compare(findCard(page, "/media/MAIN", "Metadata").readOnly, false, "a copy to this computer is not held up");
        const spy = createTemporaryObject(spyComponent, testCase, {target: page, signalName: "fullStickBackupRequested"});
        mouseClick(findCard(page, "/media/MAIN", "Full Stick Backup"));
        compare(spy.count, 0);
        tryCompare(findChild(page, "lockedDialog"), "opened", true);
    }

    // An advisor whose advice notifies, like the real one: the stick
    // list's line follows it as the second step lands.
    Component {
        id: notifyingAdvisorComponent
        QtObject {
            property var advice: ({})
            property var pending: []
            function assess(l, m, r, e) {}
            function reassessAll() {}
            function forget(m) {}
        }
    }

    // The advisor's verdict as the Full Stick Backup card says it: out of
    // date, up to date, and either with "checking cues" beside it while
    // the cue pass runs.
    function test_fullStickBackupLineFollowsTheAdvisor() {
        const pendingAdvice = {};
        pendingAdvice["/media/MAIN"] = makeAdvice({state: "current", detail: "Backup is up to date.", cuesPending: true});
        const advisor = createTemporaryObject(notifyingAdvisorComponent, testCase, {advice: pendingAdvice});
        const page = makePage([makeStick({})], {}, {backupAdvisor: advisor});
        const card = findCard(page, "/media/MAIN", "Full Stick Backup");
        verify(card !== null);
        compare(card.cardSubtitle, "Full stick backup is up to date (checking cues)");
        saveScreenshot(page, "stick-list-checking-cues");
        const settled = {};
        settled["/media/MAIN"] = makeAdvice({state: "current", detail: "Backup is up to date.", cuesPending: false});
        advisor.advice = settled;
        compare(card.cardSubtitle, "Full stick backup is up to date");
        const outdated = {};
        outdated["/media/MAIN"] = makeAdvice({state: "outdated", detail: "The library has changed since its last backup."});
        advisor.advice = outdated;
        compare(card.cardSubtitle, "Update the full stick backup: The library has changed since its last backup.");
        const outdatedPending = {};
        outdatedPending["/media/MAIN"] = makeAdvice({state: "outdated", cuesPending: true,
                                                     detail: "The library has changed since its last backup."});
        advisor.advice = outdatedPending;
        compare(card.cardSubtitle,
                "Update the full stick backup: The library has changed since its last backup. (checking cues)");
        // Nothing read yet for this stick, and the advisor on it.
        advisor.advice = {};
        advisor.pending = ["/media/MAIN"];
        compare(card.cardSubtitle, "Scanning existing backups...");
    }

    function test_emptyStickRestoreFallsBackToDiskBackupWhenNoPeer() {
        var advice = {};
        advice["/media/MAIN"] = makeAdvice({state: "restore", backupPath: "/b/OLD.zip", backupLabel: "OLD",
            detail: "The newest backup can be restored onto this empty stick."});
        var page = makePage([makeStick({label: "MAIN", hasRekordbox: false, hasEngine: false,
                                        rekordboxPath: "", enginePath: ""})], advice);
        var card = findCard(page, "/media/MAIN", "Restore Backup");
        verify(card !== null);
        compare(card.visible, true);
        compare(card.cardSubtitle, "Restore OLD's library onto this stick");
        // No other stick to copy from: the clone card stays out of the way
        // rather than doing a second, differently named restore.
        compare(findCard(page, "/media/MAIN", "Create Backup USB Stick").visible, false);
        saveScreenshot(page, "stick-list-empty-restore");
        var spy = createTemporaryObject(spyComponent, testCase, {target: page, signalName: "restoreStickBackupRequested"});
        card.clicked();
        compare(spy.count, 1);
        compare(spy.signalArguments[0][0], "/media/MAIN");
        compare(spy.signalArguments[0][2], "/b/OLD.zip");
        // The row's own name, for the restore page's breadcrumb.
        compare(spy.signalArguments[0][3], "MAIN");
    }

    // A stick's own eject/mount button used to go dark while ANY other
    // stick's task was in flight (mediaController.busy is a single
    // app-wide flag) -- a click then did nothing, worst right after
    // auto-mount started running. It only reflects this row's own task
    // now; a click on it while busy elsewhere just queues.
    function test_ejectButtonStaysUsableWhileAnotherStickIsBusy() {
        var sticks = [makeStick({}), makeStick({label: "SPARE", mountPoint: "/media/SPARE", devicePath: "/dev/sdc1"})];
        var page = makePage(sticks, {}, {mediaController: {sticks: sticks, errorMessage: "", busy: true, busyDevicePath: "/dev/sdc1", calls: [],
                                    mountStick: function(d) { this.calls.push("mount:" + d); },
                                    unmountStick: function(d) { this.calls.push("unmount:" + d); }}});
        var mainButton = findRowObject(page, "/media/MAIN", "ejectButton");
        verify(mainButton !== null);
        compare(mainButton.visible, true);
        compare(mainButton.enabled, true);
        mainButton.clicked();
        // Asked of the stick row's own copy: a plain JS object handed on
        // through a binding arrives as a copy, so its calls land there.
        compare(findByName(page, "stickHeader").mediaController.calls.indexOf("unmount:/dev/sdb1") >= 0, true);
    }

    function test_generalBackupsBlockRequestsWithNoStick() {
        // With no stick in, which is when this block is shown at all --
        // see test_theNoStickToolsStepAsideOnceAStickIsIn.
        var page = makePage([], {});
        // Explore, Sync and Maintain have nothing without a stick.
        compare(findByName(page, "noStickNothingHere").visible, true);
        compare(findByName(page, "noStickBackupTools").visible, false);
        waitForRendering(page);
        saveScreenshot(page, "stick-list-no-stick-explore");

        // Prominent: the stick's icon, a title and the hint, centred.
        compare(findByName(page, "noStickIcon").visible, true);
        compare(findByName(page, "noStickTitle").text, "No USB stick");
        compare(findByName(page, "noStickHint").horizontalAlignment, Text.AlignHCenter);

        page.selectGroup("backup");
        compare(findByName(page, "noStickNothingHere").visible, false);
        // No Restore a Stick Backup: with no stick there is no drive to
        // restore onto, and the card led to a page with nothing to pick.
        compare(findChild(page, "generalRestoreCard"), null);
        // Local Cue Backup is gone: Metadata Backup and Restore replaced it.
        compare(findChild(page, "generalLocalCueCard"), null);
        // The header menu's entries as cards, with the menu's enabled rules.
        compare(findByName(page, "browseFullBackupCard").enabled, true);
        compare(findByName(page, "manageBackupsCard").enabled, page.homeBackupsFullCount > 0);
        // About every stick's backups: no stick, no backup to list first.
        const manageSpy = createTemporaryObject(spyComponent, testCase, {target: page, signalName: "manageBackupsRequested"});
        findByName(page, "manageBackupsCard").clicked();
        compare(manageSpy.count, 1);
        compare(JSON.stringify(Array.prototype.slice.call(manageSpy.signalArguments[0])), JSON.stringify(["", ""]));
        const metadataCard = findByName(page, "browseMetadataBackupsCard");
        compare(metadataCard.enabled, page.homeBackupsMetadataCount > 0);
        const metadataSpy = createTemporaryObject(spyComponent, testCase, {target: page, signalName: "metadataBackupRequested"});
        metadataCard.clicked();
        compare(metadataSpy.count, 1);
        for (let i = 0; i < 4; ++i) {
            compare(metadataSpy.signalArguments[0][i], "", "no stick: argument " + i + " is empty");
        }
        waitForRendering(page);
        saveScreenshot(page, "stick-list-no-stick");
    }

    // The card is visible unconditionally for a blank stick (it is also
    // how you restore a backup file that never was in the default
    // directory), so its wording carries the whole burden of being
    // honest about whether one was actually found. Reported as "Seabass
    // offers to restore a backup ... but we don't have one": a blank
    // stick with an empty backup directory used to get the same "Put one
    // of your stick backups onto this empty stick" text as a stick with
    // a real match, phrased as though a backup were known to exist.
    function test_emptyStickWithNoBackupsGetsHonestRestoreCardText() {
        var empty = makeStick({label: "BLANK", mountPoint: "/media/BLANK", devicePath: "/dev/sdd1",
                               hasRekordbox: false, hasEngine: false, rekordboxPath: "", enginePath: ""});
        var advice = {};
        advice["/media/BLANK"] = makeAdvice({});  // default state: "no-backups"
        var page = makePage([empty], advice);
        var card = findCard(page, "/media/BLANK", "Restore Backup");
        verify(card !== null);
        compare(card.visible, true);
        compare(card.cardSubtitle.toLowerCase().indexOf("one of your"), -1);
        compare(card.cardSubtitle, "No known stick backups yet. Browse for a backup file to restore");

        var restore = createTemporaryObject(spyComponent, testCase, {target: page, signalName: "restoreStickBackupRequested"});
        card.clicked();
        compare(restore.count, 1);
        compare(restore.signalArguments[0][0], "/media/BLANK");
        // No specific match: the restore page opens to browse, not to a
        // preselected archive.
        compare(restore.signalArguments[0][2], "");
    }

    // A library opened from an ordinary folder (a restored stick backup,
    // or a copy on an internal disk) is listed like a stick and offers
    // the same library cards -- but the actions that need a real drive
    // behind them are gone, because there is not one.
    function test_folderLibraryListsWithoutDeviceOnlyActions() {
        var folder = makeStick({
            label: "restored-backup", mountPoint: "/home/dj/restored", devicePath: "",
            isFolder: true, libraryId: "folder-abc123",
            rekordboxPath: "/home/dj/restored/PIONEER",
            enginePath: "/home/dj/restored/Engine Library",
        });
        var page = makePage([folder], makeAdvice({state: "no-backups"}),
                            {appSettingsController: fakeAppSettings()});

        // The library is reachable: the ordinary cards are all there.
        verify(findCard(page, "/home/dj/restored", "Browse Library") !== null);
        verify(findCard(page, "/home/dj/restored", "Housekeeping") !== null);
        verify(findCard(page, "/home/dj/restored", "Sync Cue Points") !== null);

        // Formatting would erase a drive this row does not have.
        var format = findCard(page, "/home/dj/restored", "Format USB Stick");
        verify(format === null || !format.visible);

        // Eject is replaced by "remove from this list", which touches
        // nothing on disk.
        var eject = findRowObject(page, "/home/dj/restored", "ejectButton");
        verify(eject === null || !eject.visible);
        var close = findRowObject(page, "/home/dj/restored", "closeFolderButton");
        verify(close !== null);
        compare(close.visible, true);
        close.clicked();
        compare(page.mediaController.calls.indexOf("closeFolder:/home/dj/restored") >= 0, true);

        page.selectGroup("explore");
        waitForRendering(page);
        saveScreenshot(page, "stick-list-folder-library");
        // No stick in, so the Backup group also offers this computer's own
        // tools, under a line saying they are not about the folder.
        page.selectGroup("backup");
        compare(findByName(page, "noStickBackupTools").visible, true);
        compare(findByName(page, "noStickToolsLabel").visible, true);
        waitForRendering(page);
        saveScreenshot(page, "stick-list-folder-library-backup");
    }

    // A browsed stick backup is a folder row that must not be written
    // to: its analysis files are still in the archive and its directory
    // is replaced on the next open. Every card that writes is withheld;
    // the ones that only read stay. The Backups card in particular used
    // to be live here, and from this row it targets the very archive
    // being browsed.
    function test_browsedBackupWithholdsEveryWritingCard() {
        var backup = makeStick({
            label: "TOURSTICK", mountPoint: "/home/dj/Seabass/metadata/browsed-backups/folder-abc",
            devicePath: "", isFolder: true, isBrowsedBackup: true, libraryId: "folder-abc",
            rekordboxPath: "/home/dj/Seabass/metadata/browsed-backups/folder-abc/PIONEER",
            enginePath: "/home/dj/Seabass/metadata/browsed-backups/folder-abc/Engine Library",
        });
        var page = makePage([backup], makeAdvice({state: "no-backups"}),
                            {appSettingsController: fakeAppSettings()});
        var mp = backup.mountPoint;
        var reads = ["Browse Library", "Library Statistics", "Metadata"];
        for (var i = 0; i < reads.length; ++i) {
            var card = findCard(page, mp, reads[i]);
            verify(card !== null, reads[i] + " missing");
            compare(card.visible, true, reads[i] + " should stay");
        }
        var writes = ["Full Stick Backup", "Update Stick", "Restore Backup", "Manage Backups", "Housekeeping", "Library Health", "Restore Metadata",
                      "Create Engine Library", "Sync Cue Points", "Device Profile", "Format USB Stick"];
        for (var j = 0; j < writes.length; ++j) {
            var w = findCard(page, mp, writes[j]);
            verify(w === null || !w.visible, writes[j] + " must be withheld on a browsed backup");
        }
        page.selectGroup("explore");
        waitForRendering(page);
        saveScreenshot(page, "stick-list-browsed-backup");
        // Backup has nothing for a backup: every card in it writes, and
        // Metadata, the one that only reads, is in Sync.
        page.selectGroup("backup");
        compare(findByName(page, "nothingHereLabel").visible, true);
        waitForRendering(page);
        saveScreenshot(page, "stick-list-browsed-backup-backup");
    }

    // Closing a folder row is where its unsaved edits would otherwise
    // vanish unseen: the close is refused while the session is dirty,
    // and a clean session's lock is released before the row goes.
    function test_closingAFolderRowRefusesWhileDirtyAndReleasesWhenClean() {
        var folder = makeStick({
            label: "restored", mountPoint: "/home/dj/restored", devicePath: "",
            isFolder: true, libraryId: "folder-r1",
        });
        var registry = fakeEditRegistry([]);
        registry.session = {dirty: true};
        registry.hasSession = function(id) { this.calls.push("hasSession:" + id); return id === "folder-r1"; };
        registry.sessionFor = function(id) { return this.session; };
        registry.closeSession = function(id) { this.calls.push("closeSession:" + id); };
        var page = makePage([folder], makeAdvice({}), {editRegistry: registry});
        // Read the page's own copy, as every other fake in this file is
        // read (page.mediaController.calls): what the page holds is what
        // the handler talked to.
        var reg = page.editRegistry;
        var close = findRowObject(page, "/home/dj/restored", "closeFolderButton");
        verify(close !== null);

        close.clicked();
        verify(reg.calls.indexOf("hasSession:folder-r1") >= 0, "the registry was consulted");
        compare(page.mediaController.calls.indexOf("closeFolder:/home/dj/restored"), -1,
                "a dirty session must not be dropped");
        compare(reg.calls.indexOf("closeSession:folder-r1"), -1, "a dirty session must not be closed");

        reg.session.dirty = false;
        close.clicked();
        verify(reg.calls.indexOf("closeSession:folder-r1") >= 0, "a clean session is closed");
        verify(page.mediaController.calls.indexOf("closeFolder:/home/dj/restored") >= 0);
    }

    // Only one folder is open at a time, so opening another folder or
    // browsing a backup replaces the current one. With staged edits on it
    // that must be refused, as closing it is: otherwise the replaced row
    // raises the stick-removed dialog, whose only live button discards.
    function test_openingAnotherFolderOrBackupRefusesWhileTheCurrentOneIsDirty() {
        var folder = makeStick({
            label: "restored", mountPoint: "/home/dj/restored", devicePath: "",
            isFolder: true, libraryId: "folder-r1",
        });
        var registry = fakeEditRegistry([]);
        registry.session = {dirty: true};
        registry.hasSession = function(id) { return id === "folder-r1"; };
        registry.sessionFor = function(id) { return this.session; };
        registry.closeSession = function(id) { this.calls.push("closeSession:" + id); };
        var page = makePage([folder], makeAdvice({}), {editRegistry: registry});
        var reg = page.editRegistry;
        var folderDialog = findChild(page, "openFolderDialog");
        var backupDialog = findChild(page, "openBackupDialog");
        var error = findChild(page, "openFolderError");
        verify(folderDialog !== null && backupDialog !== null && error !== null);

        folderDialog.accepted();
        backupDialog.accepted();
        compare(page.mediaController.calls.length, 0,
                "nothing may replace a folder with unsaved changes: " + page.mediaController.calls);
        compare(reg.calls.indexOf("closeSession:folder-r1"), -1, "a dirty session must not be closed");
        tryCompare(error, "visible", true);
        error.close();

        reg.session.dirty = false;
        folderDialog.accepted();
        verify(reg.calls.indexOf("closeSession:folder-r1") >= 0, "a clean session is closed first");
        compare(page.mediaController.calls.length, 1);
        compare(page.mediaController.calls[0].indexOf("openFolder:"), 0);
    }

    // A backup is opened to look at its library: once it is open, the page
    // asks for Browse Library on it, with the row it became. The menu's
    // entry and Manage Backups' Browse both come through here.
    function test_openingABackupGoesOnIntoBrowseLibrary() {
        var backup = makeStick({
            label: "PARTY STICK", mountPoint: "/cache/backups/b1", devicePath: "",
            isFolder: true, isBrowsedBackup: true, libraryId: "folder-b1",
            hasRekordbox: true, hasEngine: true,
            rekordboxPath: "/cache/backups/b1/PIONEER", enginePath: "/cache/backups/b1/Engine Library",
        });
        var page = makePage([backup], makeAdvice({}));
        var spy = createTemporaryObject(spyComponent, testCase, {target: page, signalName: "browseRequested"});
        findChild(page, "openBackupDialog").accepted();
        compare(page.mediaController.calls.filter(function(c) { return c.indexOf("openBackup:") === 0; }).length, 1);
        compare(spy.count, 1, "an opened backup must go on into Browse Library");
        compare(spy.signalArguments[0][0], "PARTY STICK");
        compare(spy.signalArguments[0][1], "/cache/backups/b1/PIONEER");
        compare(spy.signalArguments[0][2], "/cache/backups/b1/Engine Library");

        // One that could not be opened stays on Home, with the reason.
        page.mediaController.openBackup = function(p) { return "That backup could not be read."; };
        page.openBackupArchive("/backups/broken.zip");
        compare(spy.count, 1, "a backup that failed to open must not be browsed");
        tryCompare(findChild(page, "openFolderError"), "visible", true);
    }

    // The window's header row (menu, About, Preferences, Support) lies
    // over this header; the page names its place and keeps the room the
    // row asks for. tst_AppHeaderOverlay covers the row itself.
    function test_thePageNamesItsPlaceAndKeepsRoomForTheHeaderRow() {
        const page = makePage([], {});
        compare(page.appHeaderPlace, "home");
        compare(findChild(page, "homeMenuButton"), null, "the menu button is the window's now");
        compare(findChild(page, "aboutButton"), null);
        const space = findChild(page, "appHeaderSpace");
        verify(space !== null, "the header keeps a space at its right");
        page.appHeaderReserve = 120;
        tryCompare(space, "width", 120);
        const lockup = findByName(page, "brandLockup");
        verify(lockup.mapToItem(page, lockup.width, 0).x <= space.mapToItem(page, 0, 0).x + 0.5,
               "the brand ends before the reserved space");
    }

    // Finds the first descendant with `objectName`, anywhere on the page.
    function findByName(page, objectName) {
        return Live.findByObjectName(page, objectName);
    }

    function test_theNoStickToolsStepAsideOnceAStickIsIn() {
        // A card about this computer's own stick backups. With no stick in
        // it is the only thing to do here; with one in it would sit above
        // the thing the page is actually about, taking the top of the
        // screen for the case that is not happening.
        var empty = makePage([], {});
        empty.selectGroup("backup");
        var tools = findByName(empty, "noStickBackupTools");
        verify(tools !== null, "the no-stick tools must exist");
        compare(tools.visible, true, "and must be shown when no stick is in");

        var withStick = makePage([makeStick({})], {"/media/MAIN": makeAdvice({})});
        withStick.selectGroup("backup");
        compare(findByName(withStick, "noStickBackupTools").visible, false,
                "and must step aside once a stick is in");
    }

    function test_anOpenedFolderIsNotAStick() {
        // The tools are for "no stick plugged in", and a folder someone
        // opened from disk is not one. Hiding them for a folder row
        // would take away the only route to them in exactly the session
        // where there is no stick to offer instead.
        var page = makePage([makeStick({label: "COPY", mountPoint: "/home/sebas/copy", isFolder: true,
                                        devicePath: ""})],
                            {});
        page.selectGroup("backup");
        compare(findByName(page, "noStickBackupTools").visible, true,
                "an opened folder must not count as a stick being in");
    }

    // Every visible item under `item` whose right edge lies past the
    // page's, or whose left edge lies left of it: the walk goes down the
    // visual tree only, and not into what is hidden.
    function itemsPastTheEdges(page, item, out) {
        if (!item || item.visible === false) {
            return out;
        }
        if (item.width > 0 && item !== page) {
            const left = item.mapToItem(page, 0, 0).x;
            if (left + item.width > page.width + 0.5 || left < -0.5) {
                out.push((item.objectName || String(item)) + " spans " + left + " to " + (left + item.width));
            }
        }
        for (let i = 0; i < item.children.length; ++i) {
            itemsPastTheEdges(page, item.children[i], out);
        }
        return out;
    }

    // The home's three forms by the window's width, on both sides of each
    // threshold: the rail a column and the cards two to a row (wide), the
    // rail a column and the cards one to a row (medium), the rail above
    // the pane and the cards one to a row (narrow). The stick's row and
    // the group heading stay what they are, and nothing reaches past the
    // page's right edge in any of them.
    function test_theHomeHasThreeForms_data() {
        return [
            {tag: "wide plus 10", width: Theme.homeWideWidth + 10, form: "wide", railColumn: true, columns: 2},
            {tag: "wide at the threshold", width: Theme.homeWideWidth, form: "wide", railColumn: true, columns: 2},
            {tag: "wide minus 10", width: Theme.homeWideWidth - 10, form: "medium", railColumn: true, columns: 1},
            {tag: "medium", width: 700, form: "medium", railColumn: true, columns: 1},
            {tag: "medium plus 10", width: Theme.homeMediumWidth + 10, form: "medium", railColumn: true, columns: 1},
            {tag: "medium at the threshold", width: Theme.homeMediumWidth, form: "medium", railColumn: true, columns: 1},
            {tag: "medium minus 10", width: Theme.homeMediumWidth - 10, form: "narrow", railColumn: false, columns: 1},
            // Not narrower: below about 490 px the header's own row (the
            // wordmark, the slogan and four buttons) is wider than the
            // window, whatever the form.
            {tag: "narrow", width: 500, form: "narrow", railColumn: false, columns: 1},
        ];
    }
    function test_theHomeHasThreeForms(data) {
        const sticks = [makeStick({hasEngine: false, enginePath: ""}),
                        makeStick({label: "SPARE", mountPoint: "/media/SPARE", devicePath: "/dev/sdc1"})];
        const page = makePage(sticks, {"/media/MAIN": makeAdvice({})}, {width: data.width});
        compare(page.homeForm, data.form);
        compare(page.compact, !data.railColumn);
        const rail = findByName(page, "homeRail");
        const pane = findByName(page, "homePane");
        compare(rail.compact, !data.railColumn);
        const railX = rail.mapToItem(page, 0, 0).x;
        const paneX = pane.mapToItem(page, 0, 0).x;
        if (data.railColumn) {
            compare(rail.width, Theme.homeRailWidth, "the rail is a column of its own width");
            verify(paneX >= railX + rail.width, "the pane stands beside the rail");
        } else {
            compare(Math.round(paneX), Math.round(railX), "the pane starts on the rail's line");
            verify(pane.mapToItem(page, 0, 0).y >= rail.mapToItem(page, 0, 0).y + rail.height,
                   "the pane is under the rail");
        }
        const grid = findByName(page, "actionGrid");
        compare(grid.columns, data.columns);
        // No standard title wraps on a card at the narrowest a form lets it
        // be, and the stick row and the heading are there as ever.
        for (const group of page.groupKeys) {
            page.selectGroup(group);
            waitForRendering(page);
            for (let i = 0; i < grid.children.length; ++i) {
                const card = grid.children[i];
                if (!card.visible || card.cardTitle === undefined) {
                    continue;
                }
                verify(card.width >= Theme.homeCardMinWidth - 0.5 || data.form === "narrow",
                       card.cardTitle + " is " + card.width + " wide");
                if (!card.experimental) {
                    compare(findChild(card, "cardTitleLabel").lineCount, 1, card.cardTitle + " stays on one line");
                }
            }
            compare(findByName(page, "stickHeader").visible, true);
            compare(findByName(page, "groupHeading").visible, true);
            const past = itemsPastTheEdges(page, page, []);
            compare(past.length, 0, group + ": " + past.join("; "));
        }
        page.selectGroup("backup");
        waitForRendering(page);
        const names = {"wide plus 10": "home-form-wide-plus10", "wide minus 10": "home-form-wide-minus10",
                       "medium plus 10": "home-form-medium-plus10", "medium minus 10": "home-form-medium-minus10"};
        if (names[data.tag] !== undefined) {
            saveScreenshot(page, names[data.tag]);
        }
        if (data.tag === "medium") {
            saveScreenshot(page, "home-rail-medium");
        }
        if (data.tag === "narrow") {
            saveScreenshot(page, "stick-list-compact");
        }
    }

    // The order of a stick's cards in each group, left to right and
    // down. Browse Library leads Explore; Sync Cue Points, what most people
    // open Seabass for, leads Sync. Read off the grid as laid out, not off
    // the source: a GridLayout places its visible children in order, so
    // this is what is on screen. A picture of each group, too.
    function test_theCardsComeInTheirOrder() {
        const settings = fakeAppSettings();
        const page = makePage([makeStick({hasEngine: false, enginePath: ""})], {"/media/MAIN": makeAdvice({})},
                              {appSettingsController: settings});
        const grid = findByName(page, "actionGrid");
        verify(grid !== null, "the action grid must exist");
        const expected = {
            explore: ["Browse Library", "Compare Playlists", "Library Statistics", "Device Profile",
                      "USB Stick Performance"],
            sync: ["Sync Cue Points", "Metadata", "Restore Metadata", "Create Engine Library"],
            backup: ["Full Stick Backup", "Restore Backup", "Manage Backups"],
            maintain: ["Housekeeping", "Library Health", "Format USB Stick"],
        };
        for (const group of page.groupKeys) {
            page.selectGroup(group);
            waitForRendering(page);
            const shown = [];
            for (let i = 0; i < grid.children.length; ++i) {
                const child = grid.children[i];
                if (child.visible && child.cardTitle !== undefined) {
                    shown.push(child);
                }
            }
            compare(JSON.stringify(shown.map((card) => card.cardTitle)), JSON.stringify(expected[group]), group);
            // Two to a row: the second card beside the first.
            compare(grid.columns, 2);
            compare(shown[1].y, shown[0].y, group + ": the second card is on the first row");
            verify(shown[1].x > shown[0].x, group + ": to the right of the first");
            saveScreenshot(page, "stick-list-group-" + group);
            if (group === "explore") {
                saveScreenshot(page, "stick-list-card-order");
            }
        }
    }

    function test_theHeaderCarriesTheBrandRatherThanTheWordHome() {
        // This is the one page you arrive at rather than navigate to, so
        // "Home" named the position rather than the thing.
        var page = makePage([], {});
        var name = findByName(page, "brandName");
        var slogan = findByName(page, "brandSlogan");
        verify(name !== null && slogan !== null, "the brand lockup must be in the header");
        compare(name.text, "Seabass");
        compare(slogan.text, "Your DJ Toolbox");
        // The slogan is the subtitle of the pair. At the same size they
        // read as two competing titles.
        verify(slogan.font.pointSize < name.font.pointSize,
               "the slogan must be a clear step smaller than the name");
        // A wordmark: regular weight, larger than a page title.
        compare(name.font.weight, Font.Normal, "the name is not bold");
        compare(name.font.pointSize, Theme.titleLarge);
        // Beside the name, on its baseline, rather than under it.
        verify(slogan.x >= name.x + name.width, "the slogan sits to the right of the name");
        compare(Math.round(slogan.y + slogan.baselineOffset), Math.round(name.y + name.baselineOffset),
                "the slogan shares the name's baseline");
    }

    function test_aLongMountPointElidesInsteadOfPushingTheCardWide() {
        // The label already asked to elide and could not: a Text's
        // Layout.minimumWidth defaults to its implicit width, so it had
        // a floor at full natural size and the row overflowed instead.
        //
        // A narrow page, deliberately: at this file's usual 1080px width
        // there is comfortably more than enough room for this label's
        // implicit width regardless of platform (measured 518px on
        // Windows; whatever it measures elsewhere, it is nowhere near
        // 1080 minus the icon column and the size label beside it), so
        // nothing forces the label to shrink and the assertion below
        // fails not because the elide mechanism is broken but because
        // this test never actually ran it out of room. A page this
        // narrow leaves stickPathLabel's row well under 518px on any
        // reasonable font metrics, which is what actually exercises it.
        var page = makePage([makeStick({
            label: "LONGONE",
            mountPoint: "/run/media/sebas/a-very-long-mount-point-name-that-will-not-fit-on-one-card-line"
        })], {}, {width: 480, height: 980});
        var label = findByName(page, "stickPathLabel");
        verify(label !== null, "the stick path label must exist");
        compare(label.elide, Text.ElideMiddle);
        // The observable, not the layout property that produces it: the
        // label is narrower than the text it was given, which is only
        // possible if it was allowed to shrink and did.
        verify(label.implicitWidth > 0, "the label must have measured its text");
        verify(label.width < label.implicitWidth,
               "a path too long for the card must be elided down (width " + label.width
               + " vs implicit " + label.implicitWidth + ")");
    }

    // What the card says once a stick is unmounted. "(not mounted)"
    // describes the kernel's state; the question the reader actually has
    // at that moment, having just pressed eject, is whether they may pull
    // the stick out.
    function test_anUnmountedStickSaysItIsSafeToUnplug() {
        var page = makePage([makeStick({label: "MAIN", mounted: false, safeToUnplug: true})], {});
        var badge = findByName(page, "okToUnplugBadge");
        verify(badge !== null, "the unplug badge must exist");
        compare(badge.visible, true);
        compare(badge.label, "OK to unplug");
    }

    // The claim is about the DEVICE, not this row's partition. A stick
    // whose other partition is still mounted (and possibly being written)
    // must not invite the user to pull it out; the model works that out
    // across every row, and the card only repeats a proven answer.
    function test_anUnmountedPartitionOfABusyDeviceDoesNotSaySoIsSafe() {
        var page = makePage([makeStick({label: "MAIN", mounted: false, safeToUnplug: false})], {});
        var label = findByName(page, "unmountedLabel");
        verify(label !== null, "the unmounted label must exist");
        compare(label.text, "(not mounted)");
    }

    // A stick that is not mounted has no mount point, so it is named by
    // its device: two unmounted sticks must not share one name, or the
    // rail and the finders would take one for the other.
    function test_anUnmountedStickRowIsNamedByItsDevice() {
        var page = makePage([makeStick({label: "MAIN", mounted: false, mountPoint: "", devicePath: "/dev/sdb1"}),
                             makeStick({label: "SPARE", mounted: false, mountPoint: "", devicePath: "/dev/sdc1"})], {});
        compare(JSON.stringify(Live.stickKeys(page)), JSON.stringify(["/dev/sdb1", "/dev/sdc1"]));
        verify(findByName(page, "railStick:/dev/sdb1") !== null && findByName(page, "railStick:/dev/sdc1") !== null,
               "each unmounted stick has its own rail entry");
        compare(Live.stickRow(page, "/dev/sdb1").label, "MAIN");
        compare(Live.stickRow(page, "/dev/sdc1").label, "SPARE");
        compare(findByName(page, "stickLabel").text, "SPARE");
    }

    function test_aMountedStickSaysNothingAboutUnplugging() {
        var page = makePage([makeStick({label: "MAIN", mounted: true})], {});
        var label = findByName(page, "unmountedLabel");
        verify(label !== null, "the unmounted label must exist");
        compare(label.visible, false, "a mounted stick shows its catalogs in that row instead");
    }

    // What is on the stick is shown as labels, one per catalog, and only
    // for the catalogs that are there.
    function test_theCardLabelsEachCatalogOnTheStick() {
        var page = makePage([makeStick({hasRekordbox: true, hasEngine: false, hasOneLibrary: true})], {});
        var device = findByName(page, "deviceLibraryBadge");
        verify(device !== null, "the DeviceLibrary label must exist");
        compare(device.visible, true);
        compare(device.label, "DeviceLibrary");
        compare(findByName(page, "oneLibraryBadge").visible, true);
        compare(findByName(page, "oneLibraryBadge").label, "OneLibrary");
        compare(findByName(page, "engineBadge").visible, false);
    }

    // A mounted stick with no catalog says so in that row, rather than
    // leaving an empty line where the labels would be.
    function test_aStickWithNoLibrarySaysSo() {
        var page = makePage([makeStick({mounted: true, hasRekordbox: false, hasEngine: false, hasOneLibrary: false})], {});
        var label = findByName(page, "noLibraryLabel");
        verify(label !== null, "the no-library label must exist");
        compare(label.visible, true);
        compare(label.text, "No library");
        compare(findByName(makePage([makeStick({})], {}), "noLibraryLabel").visible, false,
                "and a stick with a catalog shows its labels instead");
    }

    // Ejecting swaps the labels for "OK to unplug" in the same row, at the
    // same place and height, so the card does not jump under the pointer.
    function test_ejectingDoesNotMoveTheRow() {
        var mounted = makePage([makeStick({mounted: true})], {});
        var unmounted = makePage([makeStick({mounted: false, safeToUnplug: true})], {});
        var mountedRow = findByName(mounted, "stickStateRow");
        var unmountedRow = findByName(unmounted, "stickStateRow");
        verify(mountedRow !== null && unmountedRow !== null, "the state row must exist");
        compare(unmountedRow.visible, true, "the row stays when the stick is unmounted");
        compare(findByName(unmounted, "okToUnplugBadge").visible, true);
        compare(unmountedRow.height, mountedRow.height);
        compare(unmountedRow.mapToItem(unmounted, 0, 0).y, mountedRow.mapToItem(mounted, 0, 0).y);
    }

    // The path this card really showed abbreviated. Its natural width is
    // fractional (208.03 at the default font), the layout hands out whole
    // pixels, and a Text a fraction short of its width elides. The short
    // path below happens to measure a whole number of pixels, so it never
    // could catch this.
    function test_aRealMountPointWithAFractionalWidthIsNotElided() {
        var page = makePage([makeStick({label: "WHALESHARK2", mountPoint: "/media/sebas/WHALESHARK2",
                                        devicePath: "/dev/sdb1"})], {});
        var label = findByName(page, "stickPathLabel");
        verify(label !== null, "the stick path label must exist");
        if (Math.floor(label.implicitWidth) === label.implicitWidth) {
            skip("this font measures the path in whole pixels, so this case proves nothing here");
        }
        verify(!label.truncated,
               "a path with room to spare must not be abbreviated (width " + label.width
               + " vs implicit " + label.implicitWidth + ")");
    }

    // The other half of the one above, and the half that was wrong: a
    // path the card has room for must be shown whole.
    function test_aShortMountPointIsNotElided() {
        var page = makePage([makeStick({label: "MAIN", mountPoint: "/media/MAIN"})], {});
        var label = findByName(page, "stickPathLabel");
        verify(label !== null, "the stick path label must exist");
        verify(!label.truncated,
               "a path with room to spare must not be abbreviated (width " + label.width
               + " vs implicit " + label.implicitWidth + ")");
    }

    Component {
        id: spyComponent
        SignalSpy {}
    }

    // What StickListPage reads on the real UpdateChecker.
    function fakeUpdateChecker(overrides) {
        const c = {
            updateAvailable: false, runningWithdrawn: false, runningWithdrawnReason: "",
            latestVersion: "", latestChannel: "", latestNote: "", latestNoteLevel: "",
            currentVersion: "0.7.9", currentChannel: "alpha", currentCommit: "",
            downloadPage: "https://vizzzion.org/seabass/get-it.html", message: "",
        };
        for (const key in (overrides || {})) {
            c[key] = overrides[key];
        }
        return c;
    }

    function test_theBannerTakesNoRoomWhileThereIsNothingToSay() {
        const page = makePage([], {}, {updateChecker: fakeUpdateChecker()});
        const banner = findByName(page, "updateBanner");
        verify(banner !== null, "the banner must exist on the page");
        verify(!banner.visible, "nothing new, nothing withdrawn: no banner");
        compare(banner.implicitHeight, 0, "an invisible banner must not hold its space in the column");
        // And with no checker at all (a page built without one).
        const bare = makePage([], {});
        verify(!findByName(bare, "updateBanner").visible);
    }

    function test_aNewReleaseIsAnnouncedUnderTheTitle() {
        const page = makePage([], {}, {updateChecker: fakeUpdateChecker({
            updateAvailable: true, latestVersion: "0.8.0", latestChannel: "stable",
            latestNote: "Important: this version fixes a bug that could lose hot cues.", latestNoteLevel: "warning",
        })});
        const banner = findByName(page, "updateBanner");
        verify(banner.visible, "a newer release shows the banner");
        const title = findByName(page, "updateBannerTitle");
        const note = findByName(page, "updateBannerNote");
        const link = findByName(page, "updateBannerLink");
        compare(title.text, "Seabass 0.8.0 has been released!");
        verify(note.visible, "the website's note is shown");
        compare(note.text, "Important: this version fixes a bug that could lose hot cues.");
        verify(link.text.indexOf("Download the new version from ") === 0, "link line: " + link.text);
        verify(link.text.indexOf("<a href=\"https://vizzzion.org/seabass/get-it.html\">vizzzion.org/seabass</a>") > 0,
               "the website is a link to the download page: " + link.text);
        // Below the title row, above the rest of the page.
        const name = findByName(page, "brandName");
        const namePos = name.mapToItem(page, 0, name.height);
        const bannerPos = banner.mapToItem(page, 0, 0);
        verify(bannerPos.y >= namePos.y, "the banner sits under the title, not beside it");
        const body = findByName(page, "homeBody");
        verify(body.mapToItem(page, 0, 0).y >= bannerPos.y + banner.height, "the rail and the pane follow the banner");
        // The green of "good news", not the danger colours.
        compare(banner.border.color, Theme.good);
        saveScreenshot(page, "sticklist-update-banner");
    }

    function test_theBannerLeavesOutTheNoteWhenThereIsNone() {
        const page = makePage([], {}, {updateChecker: fakeUpdateChecker({
            updateAvailable: true, latestVersion: "0.7.10", latestChannel: "alpha",
        })});
        const note = findByName(page, "updateBannerNote");
        verify(!note.visible, "no note, no empty line for one");
        compare(findByName(page, "updateBannerTitle").text, "Seabass 0.7.10 has been released!");
    }

    function test_aWithdrawnBuildGetsTheLouderBanner() {
        const page = makePage([], {}, {updateChecker: fakeUpdateChecker({
            runningWithdrawn: true, runningWithdrawnReason: "Cue sync could drop memory cues.",
        })});
        const banner = findByName(page, "updateBanner");
        verify(banner.visible);
        compare(banner.border.color, Theme.dangerBorder);
        compare(findByName(page, "updateBannerTitle").text, "Seabass 0.7.9 has been withdrawn.");
        compare(findByName(page, "updateBannerNote").text, "Cue sync could drop memory cues.");
        verify(findByName(page, "updateBannerLink").text.indexOf("There is no newer release yet.") === 0);
    }

    function test_aWithdrawnBuildIsToldWhichVersionToGetInstead() {
        const page = makePage([], {}, {updateChecker: fakeUpdateChecker({
            runningWithdrawn: true, runningWithdrawnReason: "Cue sync could drop memory cues.",
            updateAvailable: true, latestVersion: "0.7.11", latestChannel: "testing",
        })});
        compare(findByName(page, "updateBannerTitle").text, "Seabass 0.7.9 has been withdrawn.");
        const link = findByName(page, "updateBannerLink").text;
        verify(link.indexOf("Download Seabass 0.7.11 from ") === 0, "names what to get: " + link);
    }

    function test_theNoteCannotInjectMarkup() {
        // The note is the website's text inside a StyledText label. It
        // is ours, but a stray angle bracket must still read as one.
        const page = makePage([], {}, {updateChecker: fakeUpdateChecker({
            updateAvailable: true, latestVersion: "0.8.0", latestChannel: "stable",
            latestNote: "Fixes cues < 1 s & loops.",
        })});
        const note = findByName(page, "updateBannerNote");
        compare(note.text, "Fixes cues < 1 s & loops.");
        compare(note.textFormat, Text.PlainText, "the note is plain text, so the website cannot format the home page");
    }

}
