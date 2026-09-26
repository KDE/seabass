// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtTest
import SeabassGui
import "Breadcrumb.js" as Breadcrumb

// BackupsHubPage.qml headless with a fake advisor: the Update Stick card
// (from a peer stick -> clone page, from the disk backup -> restore page)
// Restore Backup, which puts this stick's own full backup back onto it,
// and Manage Backups, which hands on this stick's own full backup.
TestCase {
    id: testCase
    name: "BackupsHubPage"
    width: 900
    height: 700
    visible: true
    when: windowShown

    Component {
        id: pageComponent
        BackupsHubPage { width: 880; height: 680 }
    }

    Component {
        id: spyComponent
        SignalSpy {}
    }

    // An advisor whose busy, pending and advice notify, unlike a plain
    // object. Like the real one, reassessAll() queues every known stick,
    // and busy holds until the last of them is read.
    Component {
        id: fakeAdvisorComponent
        QtObject {
            property var advice: ({})
            property var known: ["/media/MAIN"]
            property var pending: []
            readonly property bool busy: pending.length > 0
            property int reassessCalls: 0
            function reassessAll() { reassessCalls += 1; pending = known.slice(); }
        }
    }

    Component {
        id: realAdvisorComponent
        BackupAdvisorController {}
    }

    function noSource() {
        return {kind: "none", label: "", mountPoint: "", backupPath: "", modifiedAt: "", enoughSpace: true, detail: "",
                rekordboxPath: "", enginePath: ""};
    }

    function makePage(advice, overrides) {
        var props = {
            stickLabel: "MAIN",
            rekordboxPath: "/media/MAIN/PIONEER",
            enginePath: "/media/MAIN/Engine Library",
            mountPoint: "/media/MAIN",
            devicePath: "/dev/sdb1",
            appSettingsController: {experimentalFeaturesEnabled: true},
            backupAdvisor: {advice: advice},
        };
        for (var key in (overrides || {})) {
            props[key] = overrides[key];
        }
        var page = createTemporaryObject(pageComponent, testCase, props);
        verify(page !== null);
        waitForRendering(page);
        return page;
    }

    function saveScreenshot(page, name) {
        if (!screenshotDir || screenshotDir.length === 0) return;
        grabImage(page).save(screenshotDir + "/" + name + ".png");
    }

    function test_readOnlyWhileAnotherInstanceEdits() {
        var registry = {
            lockedByOther: ["lib-main"], calls: [],
            refreshLocks: function() {},
            removeLock: function(id) { this.calls.push("remove:" + id); },
            lockHolder: function(id) { return {hostname: "studio-pc", pid: 4242, startedAtUtc: ""}; },
            libraryIdForPath: function(p) { return "lib-main"; },
        };
        var page = makePage({}, {editRegistry: registry});
        compare(page.lockedByOther, true);
        compare(findChild(page, "manageBackupsCard").readOnly, true);
        compare(findChild(page, "fullStickBackupCard").readOnly, true);
        var spy = createTemporaryObject(spyComponent, testCase, {target: page, signalName: "fullStickBackupRequested"});
        mouseClick(findChild(page, "fullStickBackupCard"));
        compare(spy.count, 0);
        tryCompare(findChild(page, "lockedDialog"), "opened", true);
        saveScreenshot(page, "backups-hub-read-only");
    }

    // A stick in use can have its own backup put back from here: the card
    // hands on this stick's matched backup, and the drive to restore onto.
    function test_restoreBackupPreselectsThisSticksBackup() {
        var page = makePage({"/media/MAIN": {state: "current", detail: "", backupPath: "/b/MAIN.zip",
                                             matchedBy: "fingerprint", updateSource: noSource()}});
        var card = findChild(page, "restoreBackupCard");
        compare(card.visible, true);
        compare(card.cardTitle, "Restore Backup");
        var spy = createTemporaryObject(spyComponent, testCase, {target: page, signalName: "restoreStickBackupRequested"});
        card.clicked();
        compare(spy.count, 1);
        compare(spy.signalArguments[0][0], "/media/MAIN");
        compare(spy.signalArguments[0][1], "/dev/sdb1");
        compare(spy.signalArguments[0][2], "/b/MAIN.zip");
        saveScreenshot(page, "backups-hub-restore");
    }

    // Matched only by name: not preselected, since two sticks called the
    // same would hand each other's backup over. The page opens to choose.
    function test_restoreBackupDoesNotGuessByName() {
        var page = makePage({"/media/MAIN": {state: "current", detail: "", backupPath: "/b/MAIN.zip",
                                             matchedBy: "label", updateSource: noSource()}});
        var spy = createTemporaryObject(spyComponent, testCase, {target: page, signalName: "restoreStickBackupRequested"});
        findChild(page, "restoreBackupCard").clicked();
        compare(spy.signalArguments[0][2], "");
    }

    // An opened folder library has no drive behind it to restore onto.
    function test_restoreBackupNeedsADrive() {
        var page = makePage({}, {devicePath: ""});
        compare(findChild(page, "restoreBackupCard").visible, false);
    }

    function test_manageBackupsIsNoLongerDeprecatedAndNoUpdateSource() {
        var page = makePage({});
        compare(findChild(page, "manageBackupsCard").deprecated, false);
        compare(findChild(page, "manageBackupsCard").experimental, false);
        compare(findChild(page, "updateStickCard").visible, false);
        compare(findChild(page, "restoreCard"), null);
        compare(findChild(page, "localCueCard"), null);
        saveScreenshot(page, "backups-hub");
    }

    // Manage Backups lists every full backup, this stick's first -- but only
    // a backup the advisor really matched to it, not merely the newest one.
    function test_manageBackupsHandsOnThisSticksBackup() {
        var advice = {};
        advice["/media/MAIN"] = {state: "current", matchedBy: "fingerprint", backupPath: "/home/u/Backups/MAIN.zip",
            detail: "", cloneSource: noSource(), updateSource: noSource(), diverged: false};
        var page = makePage(advice);
        var spy = createTemporaryObject(spyComponent, testCase, {target: page, signalName: "manageBackupsRequested"});
        findChild(page, "manageBackupsCard").clicked();
        compare(spy.count, 1);
        compare(spy.signalArguments[0][0], "MAIN");
        compare(spy.signalArguments[0][1], "/home/u/Backups/MAIN.zip");

        advice["/media/MAIN"].matchedBy = "newest";
        var newest = makePage(advice);
        var newestSpy = createTemporaryObject(spyComponent, testCase, {target: newest, signalName: "manageBackupsRequested"});
        findChild(newest, "manageBackupsCard").clicked();
        compare(newestSpy.signalArguments[0][1], "", "the newest backup of anything is not this stick's");

        // Nor is a backup of another stick that merely has the same label.
        advice["/media/MAIN"].matchedBy = "label";
        var byLabel = makePage(advice);
        var labelSpy = createTemporaryObject(spyComponent, testCase, {target: byLabel, signalName: "manageBackupsRequested"});
        findChild(byLabel, "manageBackupsCard").clicked();
        compare(labelSpy.signalArguments[0][1], "", "a label match is not enough to mark a backup as this stick's");
    }

    function test_updateFromPeerStickOpensTheClonePage() {
        var advice = {};
        advice["/media/MAIN"] = {state: "outdated", detail: "The library has changed since its last backup.",
            cloneSource: noSource(), diverged: true,
            updateSource: {kind: "stick", label: "SPARE", mountPoint: "/media/SPARE", backupPath: "", modifiedAt: "2026-09-06T10:00:00",
                           enoughSpace: true, detail: "SPARE holds a newer copy of this library.",
                           rekordboxPath: "/media/SPARE/PIONEER", enginePath: "/media/SPARE/Engine Library"}};
        var page = makePage(advice);
        var card = findChild(page, "updateStickCard");
        compare(card.visible, true);
        compare(card.cardSubtitleIcon, "dialog-warning");
        compare(card.cardSubtitle, "SPARE holds a newer copy of this library.");
        var spy = createTemporaryObject(spyComponent, testCase, {target: page, signalName: "cloneStickRequested"});
        card.clicked();
        compare(spy.count, 1);
        compare(spy.signalArguments[0][0], "SPARE");
        compare(spy.signalArguments[0][1], "/media/SPARE/PIONEER");
        compare(spy.signalArguments[0][3], "/media/MAIN");
        compare(spy.signalArguments[0][4], "MAIN");
        compare(spy.signalArguments[0][5], true);
        saveScreenshot(page, "backups-hub-update");
    }

    function test_updateFromDiskBackupOpensTheRestorePage() {
        var advice = {};
        advice["/media/MAIN"] = {state: "behind-backup", detail: "The backup holds a newer copy of this library than the stick.",
            cloneSource: noSource(), diverged: false,
            updateSource: {kind: "disk-backup", label: "MAIN", mountPoint: "", backupPath: "/b/MAIN.zip", modifiedAt: "2026-09-06T10:00:00",
                           enoughSpace: true, detail: "The backup holds a newer copy of this library than this stick.",
                           rekordboxPath: "", enginePath: ""}};
        var page = makePage(advice);
        var clone = createTemporaryObject(spyComponent, testCase, {target: page, signalName: "cloneStickRequested"});
        var restore = createTemporaryObject(spyComponent, testCase, {target: page, signalName: "restoreStickBackupRequested"});
        findChild(page, "updateStickCard").clicked();
        compare(clone.count, 0);
        compare(restore.count, 1);
        compare(restore.signalArguments[0][0], "/media/MAIN");
        compare(restore.signalArguments[0][2], "/b/MAIN.zip");
    }

    function upToDateAdvice() {
        const advice = {};
        advice["/media/MAIN"] = {state: "current", detail: "", cloneSource: noSource(), updateSource: noSource(),
                                 diverged: false};
        return advice;
    }

    // Opened before the advisor has read this stick's backups: the cards
    // cannot be decided yet, so the page says it is scanning, as Match
    // Duplicate Cues does, until the advice lands.
    function test_scanningOverlayUntilThisSticksAdviceLands() {
        const advisor = createTemporaryObject(fakeAdvisorComponent, testCase, {pending: ["/media/MAIN"], advice: {}});
        const page = makePage({}, {backupAdvisor: advisor});
        const overlay = findChild(page, "scanOverlay");
        verify(overlay !== null);
        compare(overlay.visible, true);
        compare(overlay.label, "Scanning existing backups...");
        wait(500);  // the sweeping bar starts off to the left of its track
        saveScreenshot(page, "backups-hub-scanning");
        advisor.advice = upToDateAdvice();
        advisor.pending = [];
        compare(overlay.visible, false);
        compare(findChild(page, "fullStickBackupCard").cardSubtitle, "Full stick backup is up to date");
    }

    // The advisor busy with another stick does not hide this one's cards.
    function test_noOverlayWhileOnlyOtherSticksAreRead() {
        const advisor = createTemporaryObject(fakeAdvisorComponent, testCase,
                                              {pending: ["/media/B", "/media/C"], advice: upToDateAdvice()});
        const page = makePage({}, {backupAdvisor: advisor});
        compare(advisor.busy, true);
        compare(findChild(page, "scanOverlay").visible, false);
    }

    // Opened with three sticks queued and none read yet: the overlay is up
    // until THIS stick's advice lands, not until the slowest of the three
    // has been read. The advisor stays busy with the other two meanwhile.
    function test_overlayHidesOnceThisStickLandsWhileOthersRun() {
        const advisor = createTemporaryObject(fakeAdvisorComponent, testCase,
                                              {pending: ["/media/MAIN", "/media/B", "/media/C"], advice: {}});
        const page = makePage({}, {backupAdvisor: advisor});
        const overlay = findChild(page, "scanOverlay");
        compare(overlay.visible, true);
        advisor.advice = upToDateAdvice();
        advisor.pending = ["/media/B", "/media/C"];
        compare(advisor.busy, true, "the other two sticks are still being read");
        compare(overlay.visible, false);
        compare(findChild(page, "fullStickBackupCard").cardSubtitle, "Full stick backup is up to date");
    }

    // The same on coming back: reassessAll() queues every known stick. This
    // one is read second; once it has been, the other two do not hold the
    // page, and one read first does not release it early.
    function test_reassessOverlayFollowsThisStickNotTheQueue() {
        const advisor = createTemporaryObject(fakeAdvisorComponent, testCase,
                                              {known: ["/media/B", "/media/MAIN", "/media/C"], advice: upToDateAdvice()});
        const page = makePage({}, {backupAdvisor: advisor});
        const overlay = findChild(page, "scanOverlay");
        page.activated();
        page.activated();
        compare(overlay.visible, true);
        advisor.pending = ["/media/MAIN", "/media/C"];
        compare(overlay.visible, true, "another stick read first is not this one's advice");
        advisor.pending = ["/media/C"];
        compare(advisor.busy, true);
        compare(overlay.visible, false);
    }

    // Coming back (from Manage Backups, say) re-reads the backups; the
    // advice still held is stale until that lands, so the overlay is up
    // for exactly that long.
    function test_scanningOverlayWhileReassessingOnReturn() {
        const advisor = createTemporaryObject(fakeAdvisorComponent, testCase, {busy: false, advice: upToDateAdvice()});
        const page = makePage({}, {backupAdvisor: advisor});
        const overlay = findChild(page, "scanOverlay");
        page.activated();
        compare(advisor.reassessCalls, 0, "the first activation does not reassess");
        compare(overlay.visible, false);
        page.activated();
        compare(advisor.reassessCalls, 1);
        compare(overlay.visible, true);
        advisor.pending = [];
        compare(overlay.visible, false);
        advisor.pending = ["/media/B"];
        compare(overlay.visible, false, "a later pass for another stick is not this page's wait");
    }

    // Advice whose cues are still being read: the verdict stands, with
    // "checking cues" beside it, and no overlay over it; the advisor has
    // taken this stick out of pending already. Gone once the cues land.
    function test_checkingCuesBesideTheVerdict() {
        const advice = upToDateAdvice();
        advice["/media/MAIN"].cuesPending = true;
        const advisor = createTemporaryObject(fakeAdvisorComponent, testCase, {pending: [], advice: advice});
        const page = makePage({}, {backupAdvisor: advisor});
        const card = findChild(page, "fullStickBackupCard");
        compare(card.cardSubtitle, "Full stick backup is up to date (checking cues)");
        compare(findChild(page, "scanOverlay").visible, false, "a spinner would hide the verdict");
        saveScreenshot(page, "backups-hub-checking-cues");
        advisor.advice = upToDateAdvice();
        compare(card.cardSubtitle, "Full stick backup is up to date");

        const outdated = {};
        outdated["/media/MAIN"] = {state: "outdated", detail: "The library has changed since its last backup.",
                                   cloneSource: noSource(), updateSource: noSource(), diverged: false, cuesPending: true};
        advisor.advice = outdated;
        compare(card.cardSubtitle, "Update the full stick backup: The library has changed since its last backup. (checking cues)");
    }

    // tests/qml/ -> tests/fixtures/anonymized_library/rekordbox, the
    // committed library, read only (the advisor never writes).
    readonly property string fixtureRekordbox: {
        const url = Qt.resolvedUrl("../fixtures/anonymized_library/rekordbox").toString();
        return decodeURIComponent(url.replace(/^file:\/\//, "").replace(/^\/([A-Za-z]:)/, "$1"));
    }

    // The real advisor, two sticks carrying the same rekordbox library:
    // each one's first step publishes advice at once, and while either
    // side's cues are still to come the two only match "so far", so both
    // pieces of advice say cuesPending, with neither stick in pending any
    // more and busy still true. The second steps settle it: both false,
    // busy false. Holds whether the cache's stages are real or every stage
    // is read in full: the steps are the advisor's own.
    function test_realAdvisorTwoStepsOverTheFixture() {
        const advisor = createTemporaryObject(realAdvisorComponent, testCase);
        const a = "/nonexistent/seabass-cues-test/A";
        const b = "/nonexistent/seabass-cues-test/B";
        const seen = [];
        const snapshot = function(signal) {
            const pendingA = advisor.advice[a] ? advisor.advice[a].cuesPending : undefined;
            const pendingB = advisor.advice[b] ? advisor.advice[b].cuesPending : undefined;
            seen.push({signal: signal, a: pendingA, b: pendingB, pending: advisor.pending.slice(), busy: advisor.busy});
        };
        advisor.adviceChanged.connect(function() { snapshot("advice"); });
        advisor.pendingChanged.connect(function() { snapshot("pending"); });
        advisor.assess("A", a, testCase.fixtureRekordbox, "");
        advisor.assess("B", b, testCase.fixtureRekordbox, "");
        tryVerify(function() { return !advisor.busy; }, 60000);
        const trace = JSON.stringify(seen);
        const provisional = seen.filter(function(s) {
            return s.a === true && s.b === true && s.pending.length === 0;
        });
        verify(provisional.length > 0, "both verdicts up, both checking cues, neither pending: " + trace);
        compare(provisional[0].busy, true, "busy covers the second step: " + trace);
        const advice = seen.filter(function(s) { return s.signal === "advice"; });
        compare(advice.length, 4, "two first steps and two second steps: " + trace);
        compare(advice[3].a, false, trace);
        compare(advice[3].b, false, trace);
        compare(advisor.pending.length, 0);
    }

    // The real advisor says it is busy when it says anything: it used to
    // announce the change before the pass was running, so a page asking on
    // the signal heard "not busy" and never heard otherwise.
    function test_realAdvisorAnnouncesBusyOnceItIs() {
        const advisor = createTemporaryObject(realAdvisorComponent, testCase);
        const seen = [];
        advisor.busyChanged.connect(function() { seen.push(advisor.busy); });
        advisor.assess("GHOST", "/nonexistent/seabass-hub-test/GHOST", "", "");
        tryVerify(function() { return seen.length >= 2 && !advisor.busy; }, 10000);
        compare(seen[0], true);
        compare(seen[seen.length - 1], false);
    }

    // The real advisor names the sticks it is reading, one at a time: with
    // three queued, the first leaves the list once it has been read while
    // the other two are still in it, and busy covers all three.
    function test_realAdvisorPendingNamesEachStick() {
        const advisor = createTemporaryObject(realAdvisorComponent, testCase);
        const seen = [];
        advisor.pendingChanged.connect(function() { seen.push({pending: advisor.pending.slice(), busy: advisor.busy}); });
        advisor.assess("A", "/nonexistent/seabass-hub-test/A", "", "");
        advisor.assess("B", "/nonexistent/seabass-hub-test/B", "", "");
        advisor.assess("C", "/nonexistent/seabass-hub-test/C", "", "");
        compare(advisor.pending.slice(), ["/nonexistent/seabass-hub-test/A", "/nonexistent/seabass-hub-test/B",
                                          "/nonexistent/seabass-hub-test/C"]);
        tryVerify(function() { return !advisor.busy; }, 10000);
        compare(advisor.pending.length, 0);
        const firstDone = seen.filter(function(s) {
            return s.pending.indexOf("/nonexistent/seabass-hub-test/A") < 0 && s.pending.length === 2;
        });
        verify(firstDone.length > 0, "A left the list while B and C were still waiting: " + JSON.stringify(seen));
        compare(firstDone[0].busy, true);
        compare(seen[seen.length - 1].pending.length, 0);
    }

    // Busy until the pass's result has been handled, not until its worker
    // thread returns. A stick that is not there is assessed in well under
    // a millisecond; the UI thread is held here without turning the event
    // loop, as a busy frame holds it, so the worker is certainly done
    // before its result is taken. "Not busy" in that window is what made
    // the test above flake: the announcement of a pass said it was over.
    function test_realAdvisorIsBusyUntilItsResultLands() {
        const advisor = createTemporaryObject(realAdvisorComponent, testCase);
        advisor.assess("GHOST", "/nonexistent/seabass-hub-test/GHOST", "", "");
        const until = Date.now() + 200;
        while (Date.now() < until) {
            // Hold the UI thread; the worker finishes meanwhile.
        }
        verify(advisor.busy, "still busy: the result has not been handled yet");
        tryVerify(function() { return !advisor.busy; }, 10000, "and done once it has");
    }

    // Every card on this hub graduated from experimental on 2026-09-17, so
    // none of them answers to the flag any more.
    function test_everyCardIsShownWithoutExperimentalFeatures() {
        var advice = {};
        advice["/media/MAIN"] = {state: "outdated", detail: "The library has changed since its last backup.",
            cloneSource: noSource(), diverged: false,
            updateSource: {kind: "disk", label: "MAIN", mountPoint: "", backupPath: "/b/MAIN.zip",
                           modifiedAt: "2026-09-06T10:00:00", enoughSpace: true,
                           detail: "The newest backup is newer than this stick."}};
        var page = makePage(advice, {appSettingsController: {experimentalFeaturesEnabled: false}});
        compare(findChild(page, "fullStickBackupCard").visible, true);
        compare(findChild(page, "manageBackupsCard").visible, true);
        compare(findChild(page, "updateStickCard").visible, true);
    }

    // The page as the app shows it: inside a StackView, `levelsBelow`
    // pages up from the bottom (1 = pushed from Home, 2 = from another
    // page on top of Home). The breadcrumb reads the stack's depth to
    // decide whether its middle segment is a link.
    Component {
        id: crumbStackComponent
        StackView { width: testCase.width; height: testCase.height }
    }
    Component {
        id: crumbFillerComponent
        Item {}
    }
    function pushOnStack(levelsBelow, props) {
        const stack = createTemporaryObject(crumbStackComponent, testCase);
        for (let i = 0; i < levelsBelow; ++i) {
            stack.push(crumbFillerComponent, {}, StackView.Immediate);
        }
        return stack.push(pageComponent, props, StackView.Immediate);
    }

    // From Home the stick is all that stands between the house and this
    // page; opened from Library Health it names that page too, as the way back,
    // rather than putting the stick's name on a link to it.
    function test_breadcrumb_data() {
        return [
            {tag: "home", below: 1, hubLabel: "", middle: "", link: false},
            {tag: "nested", below: 2, hubLabel: "Library Health", middle: "Library Health", link: true},
        ];
    }

    function test_breadcrumb(data) {
        const props = {
            stickLabel: "MAIN",
            rekordboxPath: "/media/MAIN/PIONEER",
            enginePath: "/media/MAIN/Engine Library",
            mountPoint: "/media/MAIN",
            devicePath: "/dev/sdb1",
            appSettingsController: {experimentalFeaturesEnabled: true},
            backupAdvisor: {advice: {}},
        };
        props.hubLabel = data.hubLabel;
        const page = pushOnStack(data.below, props);
        waitForRendering(page);
        const crumb = Breadcrumb.read(page);
        compare(crumb.stick, "MAIN");
        compare(crumb.middle, data.middle);
        compare(crumb.middleIsLink, data.link);
        compare(crumb.title, "Backups");
    }
}
