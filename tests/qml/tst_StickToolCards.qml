// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui
import "../qml-live/LiveHelpers.js" as Live

// StickToolCards.qml with fake controllers: which cards each tool group
// shows for a stick, when they are read-only and what they say then, what
// each one asks for, and the order they come in. The rules are the ones
// the home list's cards had (tst_StickListPage.qml), ported group by
// group. Also a screenshot of each group when SEABASS_SCREENSHOT_DIR is set.
TestCase {
    id: testCase
    name: "StickToolCards"
    width: 1000
    height: 700
    visible: true
    when: windowShown

    property int grounds: 0

    // The pane's ground, so a screenshot shows the cards as the page does.
    Component {
        id: groundComponent
        Rectangle {
            width: 860
            height: 420
            color: Theme.background
        }
    }
    Component {
        id: cardsComponent
        StickToolCards {
            x: Theme.pageMargin
            y: Theme.pageMargin
            width: 820
        }
    }
    Component {
        id: spyComponent
        SignalSpy {}
    }
    Component {
        id: plainCardComponent
        ActionCard {
            cardTitle: "Plain"
            cardSubtitle: "A card as every other page has it"
            cardIcon: "backup"
        }
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

    function emptyStick(overrides) {
        const s = makeStick({label: "SPARE", mountPoint: "/media/SPARE", devicePath: "/dev/sdc1",
                             hasRekordbox: false, hasEngine: false, rekordboxPath: "", enginePath: "",
                             libraryId: ""});
        for (const key in (overrides || {})) {
            s[key] = overrides[key];
        }
        return s;
    }

    function noSource() {
        return {kind: "none", label: "", mountPoint: "", backupPath: "", modifiedAt: "", enoughSpace: true, detail: "",
                rekordboxPath: "", enginePath: ""};
    }

    function makeAdvice(overrides) {
        const a = {state: "no-backups", matchedBy: "none", backupPath: "", backupLabel: "", backupCreatedAt: "",
                   trackOverlap: -1, cueOverlap: -1, detail: "No backup of this library yet.",
                   cloneSource: noSource(), updateSource: noSource(), diverged: false};
        for (const key in overrides) {
            a[key] = overrides[key];
        }
        return a;
    }

    // A newer copy of MAIN's library on the mounted stick SPARE.
    function spareUpdateSource() {
        return {kind: "stick", label: "SPARE", mountPoint: "/media/SPARE", backupPath: "", modifiedAt: "2026-09-06T10:00:00",
                enoughSpace: true, detail: "SPARE holds a newer copy of this library.",
                rekordboxPath: "/media/SPARE/PIONEER", enginePath: "/media/SPARE/Engine Library"};
    }

    function mainCloneSource(enoughSpace) {
        return {kind: "stick", label: "MAIN", mountPoint: "/media/MAIN", backupPath: "", modifiedAt: "2026-09-06T10:00:00",
                enoughSpace: enoughSpace,
                detail: enoughSpace ? "Copy MAIN's library onto this stick."
                                    : "Not enough space on this stick for MAIN's library.",
                rekordboxPath: "/media/MAIN/PIONEER", enginePath: "/media/MAIN/Engine Library"};
    }

    // What StickToolCards reads on the real AppSettingsController.
    function fakeAppSettings() {
        return {experimentalFeaturesEnabled: true, stickBackupDirectory: "/tmp"};
    }

    function fakeMediaController(overrides) {
        const m = {busy: false, busyDevicePath: ""};
        for (const key in (overrides || {})) {
            m[key] = overrides[key];
        }
        return m;
    }

    function fakeEditRegistry(lockedIds) {
        return {lockedByOther: lockedIds};
    }

    // An advisor whose advice notifies, like the real one.
    Component {
        id: notifyingAdvisorComponent
        QtObject {
            property var advice: ({})
            property var pending: []
        }
    }

    function makeCards(stick, group, advice, overrides) {
        // The newest on top: a test that builds several must have its
        // clicks land on the cards it built last.
        const ground = createTemporaryObject(groundComponent, testCase, {z: ++testCase.grounds});
        verify(ground !== null);
        const props = {
            row: stick, group: group,
            mediaController: fakeMediaController(),
            appSettingsController: fakeAppSettings(),
            backupAdvisor: {advice: advice || {}},
        };
        for (const key in (overrides || {})) {
            props[key] = overrides[key];
        }
        const cards = createTemporaryObject(cardsComponent, ground, props);
        verify(cards !== null);
        waitForRendering(cards);
        return cards;
    }

    // A spy's arguments as a plain array, for one comparison of them all.
    function argsOf(spy, i) {
        return JSON.stringify(Array.prototype.slice.call(spy.signalArguments[i]));
    }

    function card(cards, title) {
        return Live.cardIn(cards, title);
    }

    // The cards a group shows, in the order the grid lays them out.
    function shownCards(cards) {
        const grid = Live.findByObjectName(cards, "actionGrid");
        verify(grid !== null, "the action grid must exist");
        const shown = [];
        for (let i = 0; i < grid.children.length; ++i) {
            const child = grid.children[i];
            if (child.visible && child.cardTitle !== undefined) {
                shown.push(child);
            }
        }
        return shown;
    }
    function shownTitles(cards) {
        return shownCards(cards).map((c) => c.cardTitle);
    }

    function saveScreenshot(item, name) {
        if (!screenshotDir || screenshotDir.length === 0) {
            return;
        }
        const ground = item.parent;
        ground.height = item.y + item.implicitHeight + Theme.pageMargin;
        waitForRendering(ground);
        grabImage(ground).save(screenshotDir + "/" + name + ".png");
    }

    // Each group's cards, in its order, for a stick with a library.
    function test_eachGroupShowsItsCardsInOrder_data() {
        return [
            {tag: "explore", group: "explore", stick: {},
             expected: ["Browse Library", "Compare Playlists", "Library Statistics", "Device Profile",
                        "USB Stick Performance"]},
            {tag: "sync", group: "sync", stick: {},
             expected: ["Sync Cue Points", "Metadata", "Restore Metadata"]},
            {tag: "sync without Engine", group: "sync", stick: {hasEngine: false, enginePath: ""},
             expected: ["Sync Cue Points", "Metadata", "Restore Metadata", "Create Engine Library"]},
            {tag: "backup", group: "backup", stick: {},
             expected: ["Full Stick Backup", "Restore Backup", "Manage Backups"]},
            {tag: "backup with a newer copy elsewhere", group: "backup", stick: {},
             advice: {updateSource: spareUpdateSource()},
             expected: ["Full Stick Backup", "Update Stick", "Restore Backup", "Manage Backups"]},
            {tag: "backup of an empty stick", group: "backup", stick: {hasRekordbox: false, hasEngine: false},
             advice: {cloneSource: mainCloneSource(true)},
             expected: ["Restore Backup", "Create Backup USB Stick"]},
            {tag: "maintain", group: "maintain", stick: {},
             expected: ["Clean Up Duplicates", "Cues on Duplicate Copies", "Clean Up Stray Cues", "Clean Up Recordings",
                 "Delete Orphaned Files", "Library Health", "Format USB Stick"]},
        ];
    }
    function test_eachGroupShowsItsCardsInOrder(data) {
        const cards = makeCards(makeStick(data.stick), data.group, {"/media/MAIN": makeAdvice(data.advice || {})});
        compare(JSON.stringify(shownTitles(cards)), JSON.stringify(data.expected));
        compare(cards.empty, false);
        compare(Live.findByObjectName(cards, "nothingHereLabel").visible, false);
    }

    // Two to a row, the second beside the first, one column when asked;
    // every column the same width whatever the cards' text asks for.
    function test_twoCardsToARowOrOne() {
        const cards = makeCards(makeStick({}), "explore", {});
        const shown = shownCards(cards);
        compare(shown.length, 5);
        compare(shown[1].y, shown[0].y, "the second card is on the first row");
        verify(shown[1].x > shown[0].x, "beside the first");
        compare(shown[2].x, shown[0].x, "the third starts the second row");
        verify(shown[2].y > shown[0].y);
        compare(shown[0].width, shown[1].width, "both columns are one width");
        verify(Math.abs(shown[1].x + shown[1].width - cards.width) < 1, "and the two fill the pane");
        saveScreenshot(cards, "stick-tools-explore");

        cards.columns = 1;
        waitForRendering(cards);
        for (let i = 1; i < shown.length; ++i) {
            compare(shown[i].x, shown[0].x, shown[i].cardTitle + " sits under the one before");
            verify(shown[i].y > shown[i - 1].y);
        }
        verify(Math.abs(shown[0].width - cards.width) < 1, "one card fills the row");
    }

    // A card's text on one line with the pane's: textInset is where it is
    // actually drawn, so the page can line the stick's name up with it.
    function test_textInsetIsWhereTheCardTextIs() {
        const cards = makeCards(makeStick({}), "explore", {});
        const first = shownCards(cards)[0];
        const title = Live.findByObjectName(first, "cardTitleLabel");
        verify(title !== null, "the card's title label must be named");
        compare(Math.round(title.mapToItem(cards, 0, 0).x), Math.round(cards.textInset));
    }

    // The home's cards are the large ones; every other page's are not,
    // and a card that is not large is exactly the card it always was.
    function test_largeCardsAreBiggerAndOrdinaryOnesUnchanged() {
        const cards = makeCards(makeStick({}), "explore", {});
        const big = shownCards(cards)[0];
        compare(big.large, true);
        compare(Live.findByObjectName(big, "cardIcon").size, Theme.iconSizeNormal);
        compare(Live.findByObjectName(big, "cardTitleLabel").font.pointSize, Theme.cardTitleSize * 1.1);
        compare(big.leftPadding, Theme.cardPadding);
        compare(big.topPadding, Theme.cardPadding);
        verify(big.height >= 84, "a large card is at least 84 px tall, got " + big.height);

        const plain = createTemporaryObject(plainCardComponent, testCase);
        const small = createTemporaryObject(plainCardComponent, testCase, {large: true});
        verify(plain !== null && small !== null);
        const styleLeft = plain.leftPadding;
        const styleTop = plain.topPadding;
        small.large = false;
        compare(small.leftPadding, styleLeft, "turning large off gives the style's padding back");
        compare(small.topPadding, styleTop);
        compare(Live.findByObjectName(plain, "cardIcon").size, Theme.iconSizeSmall);
        compare(Live.findByObjectName(plain, "cardTitleLabel").font.pointSize, Theme.cardTitleSize);

        cards.large = false;
        compare(big.large, false, "the group passes large on to its cards");
        compare(Live.findByObjectName(big, "cardIcon").size, Theme.iconSizeSmall);
    }

    // Every card asks for its page with the arguments the home list's
    // card did; Main.qml's handlers depend on them.
    function test_everyCardAsksForItsPage_data() {
        const main = ["MAIN", "/media/MAIN/PIONEER", "/media/MAIN/Engine Library"];
        return [
            {tag: "Browse Library", group: "explore", signalName: "browseRequested", args: main},
            {tag: "Compare Playlists", group: "explore", signalName: "playlistDiffRequested", args: main},
            {tag: "Library Statistics", group: "explore", signalName: "stickStatisticsRequested", args: main},
            {tag: "Device Profile", group: "explore", signalName: "settingsRequested",
             args: ["MAIN", "/media/MAIN/PIONEER"]},
            {tag: "USB Stick Performance", group: "explore", signalName: "stickPerformanceRequested",
             args: main.concat(["/media/MAIN"])},
            {tag: "Sync Cue Points", group: "sync", signalName: "syncRequested", args: main},
            {tag: "Create Engine Library", group: "sync", noEngine: true, signalName: "engineLibraryCreatorRequested",
             args: ["MAIN", "/media/MAIN/PIONEER"]},
            {tag: "Full Stick Backup", group: "backup", signalName: "fullStickBackupRequested", args: main},
            {tag: "Restore Backup", group: "backup", signalName: "restoreStickBackupRequested",
             args: ["/media/MAIN", "/dev/sdb1", "", "MAIN"]},
            {tag: "Manage Backups", group: "backup", signalName: "manageBackupsRequested", args: ["MAIN", ""]},
            {tag: "Metadata", group: "sync", signalName: "metadataBackupRequested",
             args: main.concat(["lib-main"])},
            {tag: "Restore Metadata", group: "sync", signalName: "metadataRestoreRequested",
             args: main.concat(["lib-main"])},
            {tag: "Clean Up Duplicates", group: "maintain", signalName: "cleanupRequested", args: main},
            {tag: "Library Health", group: "maintain", signalName: "libraryHealthRequested", args: main},
            {tag: "Format USB Stick", group: "maintain", signalName: "formatUsbRequested", args: []},
        ];
    }
    function test_everyCardAsksForItsPage(data) {
        const stick = data.noEngine ? makeStick({hasEngine: false, enginePath: "/media/MAIN/Engine Library"})
                                    : makeStick({});
        const cards = makeCards(stick, data.group, {});
        const target = card(cards, data.tag);
        verify(target !== null && target.visible, data.tag + " must be shown");
        compare(target.enabled, true);
        const spy = createTemporaryObject(spyComponent, testCase, {target: cards, signalName: data.signalName});
        mouseClick(target);
        compare(spy.count, 1, data.tag + " must ask for its page");
        compare(argsOf(spy, 0), JSON.stringify(data.args));
    }

    // Another instance holds the library's edit lock: every card that
    // would change the library is read-only and asks the page to explain
    // when clicked; the cards that only read stay plain.
    function test_readOnlyCardsWhileAnotherInstanceEdits() {
        const registry = fakeEditRegistry(["lib-main"]);
        const locked = {
            "explore": ["Device Profile"],
            "sync": ["Sync Cue Points", "Restore Metadata"],
            "backup": ["Full Stick Backup", "Update Stick", "Restore Backup", "Manage Backups"],
            "maintain": ["Clean Up Duplicates", "Cues on Duplicate Copies", "Clean Up Stray Cues", "Clean Up Recordings",
                 "Delete Orphaned Files", "Library Health"],
        };
        const plain = {
            "explore": ["Browse Library", "Compare Playlists", "Library Statistics", "USB Stick Performance"],
            "sync": ["Metadata"],
            "maintain": ["Format USB Stick"],
        };
        // A newer copy elsewhere, so Update Stick is among the cards.
        const advice = {"/media/MAIN": makeAdvice({updateSource: spareUpdateSource()})};
        for (const group in locked) {
            const cards = makeCards(makeStick({}), group, advice, {editRegistry: registry});
            for (const title of locked[group]) {
                const c = card(cards, title);
                compare(c.readOnly, true, title + " must be read-only while locked");
                compare(c.readOnlyReason, "Another Seabass instance is editing this library");
                compare(Live.findByObjectName(c, "readOnlyBadge").visible, true);
            }
            for (const title of (plain[group] || [])) {
                compare(card(cards, title).readOnly, false, title + " only reads, or writes only here");
            }
        }

        const cards = makeCards(makeStick({}), "maintain", {}, {editRegistry: registry});
        const cleanUp = card(cards, "Clean Up Duplicates");
        const opened = createTemporaryObject(spyComponent, testCase,
            {target: cards, signalName: "cleanupRequested"});
        const explain = createTemporaryObject(spyComponent, testCase, {target: cards, signalName: "explainLockRequested"});
        // Before the click, whose hover would put the card's tooltip in it.
        saveScreenshot(cards, "stick-tools-read-only");
        mouseClick(cleanUp);
        compare(opened.count, 0);
        compare(explain.count, 1);
        compare(explain.signalArguments[0][0], "lib-main");

        // No lock: the same card opens the feature.
        cards.editRegistry = fakeEditRegistry([]);
        compare(cleanUp.readOnly, false);
        mouseClick(cleanUp);
        compare(opened.count, 1);
    }

    // A stick the kernel mounted read-only takes no writes at all: every
    // writing card goes read-only and a click sends the user to Library
    // Health, where the repair lives; Library Health itself and the cards
    // that only read stay ordinary.
    function test_readOnlyStickGreysOutEveryWritingCard() {
        const writers = {
            "explore": ["USB Stick Performance"],
            "sync": ["Sync Cue Points", "Restore Metadata", "Create Engine Library"],
            "backup": [],
            "maintain": ["Clean Up Duplicates", "Cues on Duplicate Copies", "Clean Up Stray Cues", "Clean Up Recordings",
                 "Delete Orphaned Files"],
        };
        for (const group in writers) {
            const cards = makeCards(makeStick({readOnly: true, hasEngine: false}), group, {});
            for (const title of writers[group]) {
                const c = card(cards, title);
                verify(c !== null && c.visible, title + " missing");
                compare(c.readOnly, true, title + " should be read-only");
                verify(c.readOnlyReason.indexOf("Library Health") >= 0, title + " should point at Library Health");
            }
        }
        // An empty stick's own two cards write too.
        const empty = makeCards(emptyStick({readOnly: true}), "backup",
                                {"/media/SPARE": makeAdvice({cloneSource: mainCloneSource(true)})});
        for (const title of ["Create Backup USB Stick", "Restore Backup"]) {
            const c = card(empty, title);
            verify(c.visible, title + " missing");
            compare(c.readOnly, true, title + " should be read-only");
            verify(c.readOnlyReason.indexOf("Library Health") >= 0);
        }

        // The four full stick backup cards keep the rule the stick's
        // Backups page gave them: the lock alone. Backing up only reads.
        const backup = makeCards(makeStick({readOnly: true}), "backup",
                                 {"/media/MAIN": makeAdvice({updateSource: spareUpdateSource()})});
        for (const title of ["Full Stick Backup", "Update Stick", "Restore Backup", "Manage Backups"]) {
            const c = card(backup, title);
            verify(c.visible, title + " missing");
            compare(c.readOnly, false, title + " is not read-only on a read-only stick");
        }

        const explore = makeCards(makeStick({readOnly: true}), "explore", {});
        compare(card(explore, "Browse Library").readOnly, false);
        compare(card(explore, "Library Statistics").readOnly, false);
        const maintain = makeCards(makeStick({readOnly: true}), "maintain", {});
        compare(card(maintain, "Library Health").readOnly, false);

        // Clicking a greyed card does not start the feature; it opens the
        // page that can fix the stick. (The maintain cards are the last
        // ones built, so they are the ones on top to take the click.)
        const opened = createTemporaryObject(spyComponent, testCase,
            {target: maintain, signalName: "cleanupRequested"});
        const health = createTemporaryObject(spyComponent, testCase, {target: maintain, signalName: "libraryHealthRequested"});
        const explain = createTemporaryObject(spyComponent, testCase, {target: maintain, signalName: "explainLockRequested"});
        mouseClick(card(maintain, "Clean Up Duplicates"));
        compare(opened.count, 0);
        compare(explain.count, 0);
        compare(health.count, 1);
        compare(argsOf(health, 0),
                JSON.stringify(["MAIN", "/media/MAIN/PIONEER", "/media/MAIN/Engine Library"]));
    }

    // USB Stick Performance needs no library: an empty mounted stick gets
    // the card, and it carries the mount point the page measures at.
    // A group with a single card keeps that card at one column's width:
    // an only card stretched across the pane read as a different thing.
    function test_aLoneCardKeepsItsColumnWidth() {
        const two = makeCards(makeStick({}), "explore", {});
        const columnWidth = shownCards(two)[0].width;
        const cards = makeCards(emptyStick(), "explore", {"/media/SPARE": makeAdvice({})});
        const shown = shownCards(cards);
        compare(shown.length, 1);
        verify(Math.abs(shown[0].width - columnWidth) < 1,
               "one card, one column: " + shown[0].width + " against " + columnWidth);
        verify(shown[0].width * 2 < cards.width + 1, "never the whole pane");
    }

    function test_emptyStickCanBeMeasured() {
        const cards = makeCards(emptyStick(), "explore", {"/media/SPARE": makeAdvice({})});
        compare(JSON.stringify(shownTitles(cards)), JSON.stringify(["USB Stick Performance"]));
        const c = card(cards, "USB Stick Performance");
        compare(c.enabled, true);
        const spy = createTemporaryObject(spyComponent, testCase, {target: cards, signalName: "stickPerformanceRequested"});
        c.clicked();
        compare(spy.count, 1);
        compare(spy.signalArguments[0][0], "SPARE");
        compare(spy.signalArguments[0][3], "/media/SPARE");
    }

    // Only Create Engine Library still answers to the experimental
    // setting; every card that used to be gated is there with it off.
    function test_onlyCreateEngineLibraryAnswersToTheExperimentalSetting() {
        const settings = fakeAppSettings();
        settings.experimentalFeaturesEnabled = false;
        const stick = makeStick({hasEngine: false, enginePath: ""});
        const shown = {"explore": ["USB Stick Performance"], "maintain": ["Format USB Stick"],
                       "sync": ["Metadata", "Restore Metadata"],
                       "backup": ["Full Stick Backup", "Update Stick", "Restore Backup", "Manage Backups"]};
        const advice = {"/media/MAIN": makeAdvice({updateSource: spareUpdateSource()})};
        for (const group in shown) {
            const cards = makeCards(stick, group, advice, {appSettingsController: settings});
            for (const title of shown[group]) {
                const c = card(cards, title);
                verify(c !== null && c.visible, title + " must not be behind the experimental setting");
                compare(c.experimental, false, title + " must not be marked experimental");
            }
        }
        const off = makeCards(stick, "sync", {}, {appSettingsController: settings});
        compare(card(off, "Create Engine Library").visible, false, "Create Engine Library stays behind the setting");
        compare(JSON.stringify(shownTitles(off)), JSON.stringify(["Sync Cue Points", "Metadata", "Restore Metadata"]));

        const on = makeCards(stick, "sync", {});
        compare(card(on, "Create Engine Library").visible, true);
        compare(card(on, "Create Engine Library").experimental, true);
        saveScreenshot(on, "stick-tools-sync");
        // And never once the stick has an Engine Library to overwrite.
        const withEngine = makeCards(makeStick({}), "sync", {});
        compare(card(withEngine, "Create Engine Library").visible, false);
    }

    function test_emptyStickOffersCloneFromThePeer() {
        const advice = {};
        advice["/media/MAIN"] = makeAdvice({state: "current", detail: "Backup is up to date."});
        advice["/media/SPARE"] = makeAdvice({state: "restore", backupPath: "/b/MAIN.zip", backupLabel: "MAIN",
            detail: "The newest backup can be restored onto this empty stick.", cloneSource: mainCloneSource(true)});
        const cards = makeCards(emptyStick(), "backup", advice);
        compare(JSON.stringify(shownTitles(cards)), JSON.stringify(["Restore Backup", "Create Backup USB Stick"]));
        const clone = card(cards, "Create Backup USB Stick");
        compare(clone.enabled, true);
        compare(clone.cardSubtitle, "Copy MAIN's library onto this stick.");
        // Restoring the backup is offered beside the copy, not instead of it.
        const restore = card(cards, "Restore Backup");
        compare(restore.cardSubtitle, "Restore MAIN's library onto this stick");
        saveScreenshot(cards, "stick-tools-backup-empty-stick");

        const spy = createTemporaryObject(spyComponent, testCase, {target: cards, signalName: "cloneStickRequested"});
        clone.clicked();
        compare(spy.count, 1);
        compare(argsOf(spy, 0),
                JSON.stringify(["MAIN", "/media/MAIN/PIONEER", "/media/MAIN/Engine Library", "/media/SPARE", "SPARE", false]));

        // A stick with a library is never a clone target; its Restore
        // Backup puts its own full backup back, and with no newer copy
        // anywhere there is nothing to update it from.
        const main = makeCards(makeStick({}), "backup", advice);
        compare(card(main, "Create Backup USB Stick").visible, false);
        compare(card(main, "Restore Backup").visible, true);
        compare(card(main, "Restore Backup").cardSubtitle, "Put a full stick backup from this computer onto this stick");
        compare(card(main, "Update Stick").visible, false);
    }

    function test_cloneCardDisabledWithoutSpace() {
        const advice = {"/media/SPARE": makeAdvice({cloneSource: mainCloneSource(false)})};
        const cards = makeCards(emptyStick(), "backup", advice);
        const clone = card(cards, "Create Backup USB Stick");
        compare(clone.visible, true);
        compare(clone.enabled, false);
        // Nor while the stick is not mounted, or while its own task runs.
        const unmounted = makeCards(emptyStick({mounted: false}), "backup",
                                    {"/media/SPARE": makeAdvice({cloneSource: mainCloneSource(true)})});
        compare(card(unmounted, "Create Backup USB Stick").enabled, false);
        const busy = makeCards(emptyStick(), "backup", {"/media/SPARE": makeAdvice({cloneSource: mainCloneSource(true)})},
                               {mediaController: fakeMediaController({busy: true, busyDevicePath: "/dev/sdc1"})});
        compare(card(busy, "Create Backup USB Stick").enabled, false);
        compare(card(busy, "Restore Backup").enabled, false);
    }

    // A newer copy of this stick's library on another mounted stick:
    // Update Stick copies it over, through the clone page, with the
    // warning sign when the two have diverged.
    function test_updateFromPeerStickOpensTheClonePage() {
        const advice = {"/media/MAIN": makeAdvice({state: "outdated", detail: "The library has changed since its last backup.",
                                                   diverged: true, updateSource: spareUpdateSource()})};
        const cards = makeCards(makeStick({}), "backup", advice);
        const update = card(cards, "Update Stick");
        compare(update.visible, true);
        compare(update.objectName, "updateStickCard");
        compare(update.cardSubtitle, "SPARE holds a newer copy of this library.");
        compare(update.cardSubtitleIcon, "dialog-warning");
        compare(update.enabled, true);
        saveScreenshot(cards, "stick-tools-backup");
        const clone = createTemporaryObject(spyComponent, testCase, {target: cards, signalName: "cloneStickRequested"});
        const restore = createTemporaryObject(spyComponent, testCase, {target: cards, signalName: "restoreStickBackupRequested"});
        mouseClick(update);
        compare(clone.count, 1);
        compare(restore.count, 0);
        compare(argsOf(clone, 0), JSON.stringify(["SPARE", "/media/SPARE/PIONEER", "/media/SPARE/Engine Library",
                                                  "/media/MAIN", "MAIN", true]));

        // Not diverged: no warning. No room for it: offered, not usable.
        const plain = spareUpdateSource();
        plain.enoughSpace = false;
        const tight = makeCards(makeStick({}), "backup", {"/media/MAIN": makeAdvice({updateSource: plain})});
        compare(card(tight, "Update Stick").cardSubtitleIcon, "");
        compare(card(tight, "Update Stick").enabled, false);
    }

    // The newer copy is the disk backup itself: Update Stick restores it.
    function test_updateFromDiskBackupOpensTheRestorePage() {
        const advice = {"/media/MAIN": makeAdvice({state: "behind-backup",
            detail: "The backup holds a newer copy of this library than the stick.",
            updateSource: {kind: "disk-backup", label: "MAIN", mountPoint: "", backupPath: "/b/MAIN.zip",
                           modifiedAt: "2026-09-06T10:00:00", enoughSpace: true,
                           detail: "The backup holds a newer copy of this library than this stick.",
                           rekordboxPath: "", enginePath: ""}})};
        const cards = makeCards(makeStick({}), "backup", advice);
        const clone = createTemporaryObject(spyComponent, testCase, {target: cards, signalName: "cloneStickRequested"});
        const restore = createTemporaryObject(spyComponent, testCase, {target: cards, signalName: "restoreStickBackupRequested"});
        card(cards, "Update Stick").clicked();
        compare(clone.count, 0);
        compare(restore.count, 1);
        compare(argsOf(restore, 0), JSON.stringify(["/media/MAIN", "/dev/sdb1", "/b/MAIN.zip", "MAIN"]));
    }

    // A stick in use can have its own backup put back: Restore Backup
    // hands on the backup the advisor matched to this stick, by library
    // or hardware, and never one that merely shares its name.
    function test_restoreBackupPreselectsThisSticksBackup_data() {
        return [
            {tag: "fingerprint", matchedBy: "fingerprint", archive: "/b/MAIN.zip",
             subtitle: "Put this stick's full backup back onto it, or another one"},
            {tag: "identifier", matchedBy: "identifier", archive: "/b/MAIN.zip",
             subtitle: "Put this stick's full backup back onto it, or another one"},
            {tag: "label", matchedBy: "label", archive: "",
             subtitle: "Put a full stick backup from this computer onto this stick"},
            {tag: "newest", matchedBy: "newest", archive: "",
             subtitle: "Put a full stick backup from this computer onto this stick"},
        ];
    }
    function test_restoreBackupPreselectsThisSticksBackup(data) {
        const advice = {"/media/MAIN": makeAdvice({state: "current", backupPath: "/b/MAIN.zip", matchedBy: data.matchedBy})};
        const cards = makeCards(makeStick({}), "backup", advice);
        const restore = card(cards, "Restore Backup");
        compare(restore.visible, true);
        compare(restore.cardSubtitle, data.subtitle);
        const spy = createTemporaryObject(spyComponent, testCase, {target: cards, signalName: "restoreStickBackupRequested"});
        mouseClick(restore);
        compare(spy.count, 1);
        compare(argsOf(spy, 0), JSON.stringify(["/media/MAIN", "/dev/sdb1", data.archive, "MAIN"]));
    }

    // Manage Backups lists every full backup, this stick's first: but only
    // a backup the advisor really matched to it, not merely the newest
    // one, nor another stick's that has the same label.
    function test_manageBackupsHandsOnThisSticksBackup_data() {
        return [
            {tag: "fingerprint", matchedBy: "fingerprint", archive: "/home/u/Backups/MAIN.zip"},
            {tag: "newest", matchedBy: "newest", archive: ""},
            {tag: "label", matchedBy: "label", archive: ""},
        ];
    }
    function test_manageBackupsHandsOnThisSticksBackup(data) {
        const advice = {"/media/MAIN": makeAdvice({state: "current", matchedBy: data.matchedBy,
                                                   backupPath: "/home/u/Backups/MAIN.zip"})};
        const cards = makeCards(makeStick({}), "backup", advice);
        const manage = card(cards, "Manage Backups");
        compare(manage.objectName, "stickManageBackupsCard");
        compare(manage.cardSubtitle, "Browse and delete the full stick backups on this computer");
        compare(manage.deprecated, false);
        const spy = createTemporaryObject(spyComponent, testCase, {target: cards, signalName: "manageBackupsRequested"});
        mouseClick(manage);
        compare(spy.count, 1);
        compare(argsOf(spy, 0), JSON.stringify(["MAIN", data.archive]));
    }

    // Each of the advisor's verdicts, as the Full Stick Backup card says
    // it: the words the stick's Backups page had.
    function test_fullStickBackupLineFollowsTheVerdict_data() {
        return [
            {tag: "outdated", advice: {state: "outdated", detail: "The library has changed."},
             expected: "Update the full stick backup: The library has changed."},
            {tag: "current", advice: {state: "current"}, expected: "Full stick backup is up to date"},
            {tag: "behind-backup", advice: {state: "behind-backup", detail: "The backup is newer than the stick."},
             expected: "Back up the whole stick into one file on this computer"},
            {tag: "no-backups", advice: {state: "no-backups"},
             expected: "Back up the whole stick into one file on this computer"},
            {tag: "not assessed", advice: null,
             expected: "Back up the whole stick into one file on this computer"},
        ];
    }
    function test_fullStickBackupLineFollowsTheVerdict(data) {
        const advice = data.advice === null ? {} : {"/media/MAIN": makeAdvice(data.advice)};
        const cards = makeCards(makeStick({}), "backup", advice);
        compare(card(cards, "Full Stick Backup").cardSubtitle, data.expected);
    }

    // The verdict with "checking cues" beside it while the cue pass runs,
    // the plain verdict once it has landed, in both states that have one.
    function test_fullStickBackupLineSaysCheckingCuesWhilePending() {
        const pendingAdvice = {"/media/MAIN": makeAdvice({state: "current", detail: "Backup is up to date.", cuesPending: true})};
        const advisor = createTemporaryObject(notifyingAdvisorComponent, testCase, {advice: pendingAdvice});
        const cards = makeCards(makeStick({}), "backup", {}, {backupAdvisor: advisor});
        const full = card(cards, "Full Stick Backup");
        compare(full.cardSubtitle, "Full stick backup is up to date (checking cues)");
        saveScreenshot(cards, "stick-tools-checking-cues");
        advisor.advice = {"/media/MAIN": makeAdvice({state: "current", detail: "Backup is up to date.", cuesPending: false})};
        compare(full.cardSubtitle, "Full stick backup is up to date");
        advisor.advice = {"/media/MAIN": makeAdvice({state: "outdated", cuesPending: true,
                                                     detail: "The library has changed since its last backup."})};
        compare(full.cardSubtitle, "Update the full stick backup: The library has changed since its last backup. (checking cues)");
    }

    // Before the advisor has read this stick's backups the card cannot say
    // anything true about them, so it says it is scanning, until this
    // stick's advice lands; other sticks still being read do not hold it.
    function test_fullStickBackupSaysScanningUntilThisSticksAdviceLands() {
        const advisor = createTemporaryObject(notifyingAdvisorComponent, testCase,
                                              {advice: {}, pending: ["/media/MAIN", "/media/B"]});
        const cards = makeCards(makeStick({}), "backup", {}, {backupAdvisor: advisor});
        const full = card(cards, "Full Stick Backup");
        compare(full.cardSubtitle, "Scanning existing backups...");
        advisor.advice = {"/media/MAIN": makeAdvice({state: "current"})};
        advisor.pending = ["/media/B"];
        compare(full.cardSubtitle, "Full stick backup is up to date");
        // Another stick pending, and none for this one: the plain line.
        advisor.advice = {};
        compare(full.cardSubtitle, "Back up the whole stick into one file on this computer");
    }

    function test_emptyStickRestoreFallsBackToDiskBackupWhenNoPeer() {
        const advice = {"/media/MAIN": makeAdvice({state: "restore", backupPath: "/b/OLD.zip", backupLabel: "OLD",
            detail: "The newest backup can be restored onto this empty stick."})};
        const cards = makeCards(makeStick({hasRekordbox: false, hasEngine: false, rekordboxPath: "", enginePath: ""}),
                                "backup", advice);
        const restore = card(cards, "Restore Backup");
        compare(restore.visible, true);
        compare(restore.cardSubtitle, "Restore OLD's library onto this stick");
        // No other stick to copy from: the clone card stays out of the way.
        compare(card(cards, "Create Backup USB Stick").visible, false);
        const spy = createTemporaryObject(spyComponent, testCase, {target: cards, signalName: "restoreStickBackupRequested"});
        restore.clicked();
        compare(spy.count, 1);
        compare(argsOf(spy, 0), JSON.stringify(["/media/MAIN", "/dev/sdb1", "/b/OLD.zip", "MAIN"]));
    }

    // Worded so it never presupposes a backup exists: "no-backups" is a
    // common state for a freshly formatted stick.
    function test_emptyStickWithNoBackupsGetsHonestRestoreCardText() {
        const blank = emptyStick({label: "BLANK", mountPoint: "/media/BLANK", devicePath: "/dev/sdd1"});
        const cards = makeCards(blank, "backup", {"/media/BLANK": makeAdvice({})});
        const restore = card(cards, "Restore Backup");
        compare(restore.visible, true);
        compare(restore.cardSubtitle.toLowerCase().indexOf("one of your"), -1);
        compare(restore.cardSubtitle, "No known stick backups yet. Browse for a backup file to restore");
        const spy = createTemporaryObject(spyComponent, testCase, {target: cards, signalName: "restoreStickBackupRequested"});
        restore.clicked();
        compare(spy.count, 1);
        compare(spy.signalArguments[0][0], "/media/BLANK");
        compare(spy.signalArguments[0][2], "", "no match: the restore page opens to browse");
        // Offered on an unmounted stick too: one fresh out of Format is
        // not remounted, and the restore page mounts it itself.
        const unmounted = makeCards(emptyStick({mounted: false, mountPoint: ""}), "backup", {});
        compare(card(unmounted, "Restore Backup").visible, true);
        compare(card(unmounted, "Restore Backup").enabled, true);
    }

    // A library opened from an ordinary folder offers the library cards,
    // but not the actions that need a real drive behind them.
    function test_folderLibraryHasNoDeviceOnlyActions() {
        const folder = makeStick({label: "restored-backup", mountPoint: "/home/dj/restored", devicePath: "",
                                  isFolder: true, libraryId: "folder-abc123",
                                  rekordboxPath: "/home/dj/restored/PIONEER",
                                  enginePath: "/home/dj/restored/Engine Library"});
        compare(card(makeCards(folder, "explore", {}), "Browse Library").visible, true);
        compare(card(makeCards(folder, "explore", {}), "USB Stick Performance").visible, false);
        compare(card(makeCards(folder, "sync", {}), "Sync Cue Points").visible, true);
        const maintain = makeCards(folder, "maintain", {});
        compare(card(maintain, "Clean Up Duplicates").visible, true);
        compare(card(maintain, "Format USB Stick").visible, false, "no drive to erase behind a folder");
        // Its backups can be made and listed, but nothing restores or
        // updates a drive it does not have.
        const backup = makeCards(folder, "backup", {"/home/dj/restored": makeAdvice({updateSource: spareUpdateSource()})});
        compare(JSON.stringify(shownTitles(backup)),
                JSON.stringify(["Full Stick Backup", "Manage Backups"]));
        // A folder with no library has nothing to restore through either.
        const bare = makeCards(makeStick({isFolder: true, devicePath: "", hasRekordbox: false, hasEngine: false}),
                               "backup", {});
        compare(card(bare, "Restore Backup").visible, false);
        compare(bare.empty, true);
    }

    // A browsed stick backup must not be written to: every card that
    // writes is withheld, the ones that only read stay.
    function test_browsedBackupWithholdsEveryWritingCard() {
        const backup = makeStick({label: "TOURSTICK", mountPoint: "/home/dj/Seabass/metadata/browsed-backups/folder-abc",
                                  devicePath: "", isFolder: true, isBrowsedBackup: true, libraryId: "folder-abc",
                                  hasEngine: false,
                                  rekordboxPath: "/home/dj/Seabass/metadata/browsed-backups/folder-abc/PIONEER",
                                  enginePath: ""});
        const expected = {
            "explore": ["Browse Library", "Compare Playlists", "Library Statistics"],
            "sync": ["Metadata"],
            "backup": [],
            "maintain": [],
        };
        for (const group in expected) {
            const cards = makeCards(backup, group, {});
            compare(JSON.stringify(shownTitles(cards)), JSON.stringify(expected[group]), group);
            compare(cards.empty, expected[group].length === 0, group + " empty");
            if (group === "backup") {
                saveScreenshot(cards, "stick-tools-nothing-here");
            }
        }
    }

    // A group with nothing to show for this stick says so, in one muted
    // line, instead of an empty grid.
    function test_aGroupWithNothingForThisStickSaysSo() {
        const cases = [
            {stick: emptyStick(), group: "sync", empty: true},
            {stick: emptyStick(), group: "explore", empty: false},  // Performance needs no library
            {stick: emptyStick({mounted: false}), group: "explore", empty: true},
            {stick: emptyStick(), group: "backup", empty: false},  // Restore Backup
            {stick: emptyStick(), group: "maintain", empty: false},  // Format
            {stick: makeStick({}), group: "nonsense", empty: true},
        ];
        for (const c of cases) {
            const cards = makeCards(c.stick, c.group, {});
            const what = c.group + (c.stick.mounted ? "" : " unmounted");
            compare(cards.empty, c.empty, what);
            const line = Live.findByObjectName(cards, "nothingHereLabel");
            compare(line.visible, c.empty, what + " line");
            compare(line.text, "Nothing here for this stick.");
            compare(String(line.color), String(Theme.textMuted), what + " line is muted");
            if (c.empty) {
                compare(line.mapToItem(cards, 0, 0).x, cards.textInset, what + " line starts on the pane's text edge");
            }
            compare(Live.findByObjectName(cards, "actionGrid").visible, !c.empty, what + " grid");
            compare(shownTitles(cards).length === 0, c.empty, what + " cards");
        }
        // It follows the row: a library appearing fills the group.
        const cards = makeCards(emptyStick(), "sync", {});
        compare(cards.empty, true);
        cards.row = makeStick({});
        compare(cards.empty, false);
        compare(JSON.stringify(shownTitles(cards)), JSON.stringify(["Sync Cue Points", "Metadata", "Restore Metadata"]));
    }

    // Format is the one card the app-wide busy flag disables: erasing a
    // drive has to wait for whatever else is running.
    function test_formatWaitsForTheAppToBeIdle() {
        const cards = makeCards(makeStick({}), "maintain", {},
                                {mediaController: fakeMediaController({busy: true, busyDevicePath: "/dev/sdz1"})});
        compare(card(cards, "Format USB Stick").enabled, false);
        compare(card(cards, "Clean Up Duplicates").enabled, true, "the other cards are not held up");
        saveScreenshot(cards, "stick-tools-maintain");
    }

    // Enabled only when the catalog each card works on is there.
    function test_cardsNeedTheirCatalog() {
        const rekordboxOnly = makeStick({hasEngine: false, enginePath: ""});
        compare(card(makeCards(rekordboxOnly, "sync", {}), "Sync Cue Points").enabled, false,
                "Sync needs both catalogs");
        const engineOnly = makeStick({hasRekordbox: false, rekordboxPath: ""});
        compare(card(makeCards(engineOnly, "explore", {}), "Device Profile").enabled, false,
                "Device Profile reads the rekordbox settings");
        compare(card(makeCards(engineOnly, "explore", {}), "Browse Library").enabled, true);
    }

    // ---- Maintain's clean-up cards (they had a Housekeeping page) -----

    // Stand-ins for the two quick probes the group takes when it shows.
    Component {
        id: recordingsProbeComponent
        QtObject {
            property var summary: ({count: 0, bytes: 0, sources: []})
            property int asked: 0
            function summarize(rekordbox, engine) { asked++; return summary; }
        }
    }
    Component {
        id: pendingProbeComponent
        QtObject {
            property int perFormat: 0
            property int pendingDeletionsIncludedCount: 0
            function loadPendingDeletionsOnly(format, path) { pendingDeletionsIncludedCount = perFormat; }
            function hasOneLibrary(path) { return false; }
        }
    }

    function makeMaintain(summary, pendingPerFormat, lockedByOther) {
        const recordings = createTemporaryObject(recordingsProbeComponent, testCase, {summary: summary});
        const pending = createTemporaryObject(pendingProbeComponent, testCase, {perFormat: pendingPerFormat || 0});
        const overrides = {recordingsProbe: recordings, pendingProbe: pending};
        if (lockedByOther) {
            overrides.editRegistry = fakeEditRegistry(lockedByOther);
        }
        const cards = makeCards(makeStick({}), "maintain", {}, overrides);
        verify(recordings.asked > 0, "Maintain asks for the recordings when it shows");
        return cards;
    }

    function test_recordingsCardSaysWhatIsThere_data() {
        const g = 1024 * 1024 * 1024;
        return [
            {tag: "engine and pioneer", summary: {count: 3, bytes: 6.2 * g, sources: ["engine", "pioneer"]},
             subtitle: "3 recordings, " + Theme.humanBytes(6.2 * g) + ", from Engine OS and a Pioneer deck", enabled: true},
            {tag: "one alphatheta", summary: {count: 1, bytes: 0.5 * g, sources: ["alphatheta"]},
             subtitle: "1 recording, " + Theme.humanBytes(0.5 * g) + ", from an AlphaTheta deck", enabled: true},
            {tag: "all three", summary: {count: 4, bytes: 2 * g, sources: ["engine", "pioneer", "alphatheta"]},
             subtitle: "4 recordings, " + Theme.humanBytes(2 * g)
                       + ", from Engine OS, a Pioneer deck and an AlphaTheta deck", enabled: true},
            {tag: "none", summary: {count: 0, bytes: 0, sources: []},
             subtitle: "No recordings on this stick", enabled: false},
            {tag: "unreadable", summary: {count: 0, bytes: 0, sources: [], unreadable: true},
             subtitle: "Could not read the recording folders on this stick", enabled: false},
        ];
    }
    function test_recordingsCardSaysWhatIsThere(data) {
        const cards = makeMaintain(data.summary);
        const c = card(cards, "Clean Up Recordings");
        verify(c !== null && c.visible);
        compare(c.cardSubtitle, data.subtitle);
        compare(c.enabled, data.enabled);
        compare(c.readOnly, false);
        if (data.tag === "engine and pioneer") {
            saveScreenshot(cards, "stick-tools-maintain-recordings");
        }
    }

    // At start-up the row comes before its library does: the cards are
    // made for a stick whose library is not found yet, and the row is
    // replaced once it is. The recordings are counted for that row, not
    // left at the "nothing here" of the one before it.
    function test_recordingsAreCountedOnceTheLibraryIsFound() {
        const recordings = createTemporaryObject(recordingsProbeComponent, testCase,
                                                 {summary: {count: 2, bytes: 2 * 1024 * 1024 * 1024, sources: ["engine"]}});
        const pending = createTemporaryObject(pendingProbeComponent, testCase, {perFormat: 0});
        const cards = makeCards(emptyStick({label: "MAIN", mountPoint: "/media/MAIN", devicePath: "/dev/sdb1"}),
                                "maintain", {}, {recordingsProbe: recordings, pendingProbe: pending});
        compare(card(cards, "Clean Up Recordings").visible, false, "no library yet, so no clean-up cards");
        cards.row = makeStick({});
        tryVerify(() => card(cards, "Clean Up Recordings").enabled, 2000,
                  "the recordings are counted once the library is found: " + card(cards, "Clean Up Recordings").cardSubtitle);
        verify(card(cards, "Clean Up Recordings").cardSubtitle.indexOf("2 recordings") === 0,
               card(cards, "Clean Up Recordings").cardSubtitle);
    }

    function test_recordingsCardOpensThePage() {
        const cards = makeMaintain({count: 2, bytes: 1000, sources: ["engine"]});
        const spy = createTemporaryObject(spyComponent, testCase, {target: cards, signalName: "recordingsRequested"});
        mouseClick(card(cards, "Clean Up Recordings"));
        compare(spy.count, 1);
        compare(spy.signalArguments[0][0], "MAIN");
    }

    function test_recordingsCardIsReadOnlyWhileAnotherInstanceEdits() {
        const cards = makeMaintain({count: 2, bytes: 1000, sources: ["engine"]}, 0, ["lib-main"]);
        const c = card(cards, "Clean Up Recordings");
        compare(c.readOnly, true);
        const spy = createTemporaryObject(spyComponent, testCase, {target: cards, signalName: "recordingsRequested"});
        mouseClick(c);
        compare(spy.count, 0, "a read-only card does not open the page");
    }

    // Delete Orphaned Files is offered only when earlier cleanups left
    // something, counted over both catalogs.
    function test_orphanedFilesCardOnlyWithSomethingToDelete_data() {
        return [
            {tag: "nothing", perFormat: 0, enabled: false, text: "Nothing orphaned right now"},
            {tag: "some", perFormat: 2, enabled: true, text: "Free disk space"},
        ];
    }
    function test_orphanedFilesCardOnlyWithSomethingToDelete(data) {
        const cards = makeMaintain({count: 0, bytes: 0, sources: []}, data.perFormat);
        compare(cards.pendingCount, 2 * data.perFormat, "both catalogs counted");
        const c = card(cards, "Delete Orphaned Files");
        compare(c.enabled, data.enabled);
        verify(c.cardSubtitle.indexOf(data.text) === 0, c.cardSubtitle);
    }
}
