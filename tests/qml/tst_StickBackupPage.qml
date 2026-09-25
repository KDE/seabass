// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtTest
import SeabassGui
import "Breadcrumb.js" as Breadcrumb

// StickBackupPage.qml headless, with a plain JS object standing in for
// StickBackupController (same technique as tst_FormatUsbPage.qml): no
// archive, no stick, no background thread -- just the page's own
// decisions about what to enable, show and call.
TestCase {
    id: testCase
    name: "StickBackupPage"
    width: 900
    height: 900
    visible: true
    when: windowShown

    Component {
        id: pageComponent
        StickBackupPage { width: 880; height: 880 }
    }

    function makeFakeController(overrides) {
        var c = {
            stickLabel: "STICK",
            stickRoot: "/media/STICK",
            archivePath: "/home/u/Seabass Backups/STICK.zip",
            busy: false,
            backingUp: false,
            previewing: false,
            activity: "",
            phase: "",
            filesDone: 0,
            filesTotal: 0,
            bytesDone: 0,
            bytesTotal: 0,
            bytesPerSecond: 0,
            etaSeconds: -1,
            currentFile: "",
            lastBackup: {exists: true, status: "complete", createdAt: "2026-09-03T21:14:00",
                         archiveBytes: 25 * 1024 * 1024 * 1024, entries: 1161, identifierMismatch: false},
            sinceLastBackup: {added: 12, changed: 3, removed: 1, unchanged: 1145, databaseChanged: true,
                              bytesToRead: 1.3 * 1024 * 1024 * 1024, stickBytes: 25 * 1024 * 1024 * 1024,
                              entriesOnStick: 1161, freeBytes: 400 * 1024 * 1024 * 1024, enoughFreeSpace: true,
                              uniformShiftSeconds: 0, estimatedSeconds: 240},
            deadSpace: {deadBytes: 4.2 * 1024 * 1024 * 1024, archiveBytes: 25 * 1024 * 1024 * 1024, ratio: 0.18, suggested: false},
            blockedBy: "",
            // The page reads both of these; without them every test in
            // this file logged "Cannot read property 'length' of
            // undefined" and "Unable to assign [undefined] to QString"
            // three times over. Noise, not a page defect -- the real
            // controller always has them -- but noise that buries a real
            // warning when one appears.
            nameCollidedWith: "",
            backupName: "",
            pendingCancelDecision: false,
            errorMessage: "",
            statusMessage: "",
            calls: [],
            preflight: {deadBytes: 4.2 * 1024 * 1024 * 1024, reclaimableBytes: 4.2 * 1024 * 1024 * 1024,
                        archiveBytes: 25 * 1024 * 1024 * 1024, ratio: 0.18, requiredFreeBytes: 19 * 1024 * 1024 * 1024,
                        availableFreeBytes: 400 * 1024 * 1024 * 1024, enoughFreeSpace: true},
            configure: function(label, rb, engine, dir) { this.calls.push("configure:" + label + ":" + dir); },
            refresh: function() { this.calls.push("refresh"); },
            backUp: function() { this.calls.push("backUp"); },
            cancel: function() { this.calls.push("cancel"); },
            keepPartial: function() { this.calls.push("keepPartial"); },
            discardPartial: function() { this.calls.push("discardPartial"); },
            deleteBackup: function() { this.calls.push("deleteBackup"); },
            verify: function() { this.calls.push("verify"); },
            compact: function() { this.calls.push("compact"); },
            compactionPreflight: function() { this.calls.push("compactionPreflight"); return this.preflight; },
            openArchiveFolder: function() { this.calls.push("openArchiveFolder"); },
        };
        for (var key in overrides) {
            c[key] = overrides[key];
        }
        return c;
    }

    function makePage(overrides, experimental) {
        return createTemporaryObject(pageComponent, testCase, {
            stickLabel: "STICK",
            rekordboxPath: "/media/STICK/PIONEER",
            enginePath: "/media/STICK/Engine Library",
            appSettingsController: {stickBackupDirectory: "/home/u/Seabass Backups",
                                    experimentalFeaturesEnabled: experimental !== false},
            controller: makeFakeController(overrides),
        });
    }

    // Backing up and restoring both graduated from experimental
    // (2026-09-17), so this page offers both with the flag off.
    function test_backupAndRestoreAreOfferedWithoutTheExperimentalFlag() {
        var page = makePage({}, false);
        compare(findChild(page, "backUpFrame").visible, true);
        compare(findChild(page, "restoreSection").visible, true);
    }

    // No EXPERIMENTAL pill anywhere on the page: the restore section
    // wore one after both halves had graduated.
    function test_carriesNoExperimentalBadge() {
        const page = makePage({}, false);
        compare(findChild(page, "experimentalBadge"), null);
    }

    // The hint beside the name field, word for word: an example in the
    // shape of a stick label, not a sentence.
    function test_backupNameHintNamesAnExampleLabel() {
        const page = makePage({});
        compare(findChild(page, "backupNameHint").text, "Optional, e.g. MYLIBRARY");
    }

    function calls(page) {
        return page.controller.calls.join(",");
    }

    function test_configuresControllerFromItsPropertiesOnLoad() {
        var page = makePage({});
        verify(page !== null);
        verify(calls(page).indexOf("configure:STICK:/home/u/Seabass Backups") >= 0);
    }

    function test_idleWithExistingBackupOffersEverything() {
        var page = makePage({});
        var backUp = findChild(page, "backUpButton");
        verify(backUp !== null);
        compare(backUp.visible, true);
        compare(backUp.enabled, true);
        compare(findChild(page, "restoreButton").enabled, true);
        compare(findChild(page, "compactButton").visible, true);
        verify(findChild(page, "sinceLabel").text.indexOf("12 added") >= 0);
        verify(findChild(page, "sinceLabel").text.indexOf("database changed") >= 0);
        backUp.clicked();
        verify(calls(page).indexOf("backUp") >= 0);
    }

    function test_firstBackupWording() {
        var page = makePage({lastBackup: {exists: false}, deadSpace: {deadBytes: 0}});
        verify(findChild(page, "lastBackupLabel").text.indexOf("No backup") >= 0);
        verify(findChild(page, "sinceLabel").text.indexOf("reads everything") >= 0);
        compare(findChild(page, "restoreButton").enabled, false);
        compare(findChild(page, "compactButton").visible, false);
        compare(findChild(page, "backUpButton").enabled, true);
    }

    // The refusal is visible before clicking: a banner plus a disabled
    // button, from whichever source (the guard poller or the controller's
    // own refresh) knows about the running software.
    function test_runningDjSoftwareBlocksBackup() {
        var page = makePage({blockedBy: "Engine DJ"});
        compare(findChild(page, "backUpButton").enabled, false);
        var banner = findChild(page, "blockedBanner");
        verify(banner !== null);
        verify(banner.text.indexOf("Engine DJ") >= 0);

        var viaGuard = makePage({});
        viaGuard.conflictingSoftware = "rekordbox";
        compare(findChild(viaGuard, "backUpButton").enabled, false);
    }

    function test_notEnoughFreeSpaceBlocksBackup() {
        var page = makePage({sinceLastBackup: {added: 1, changed: 0, removed: 0, unchanged: 0, databaseChanged: false,
                                               bytesToRead: 10, stickBytes: 10, entriesOnStick: 1, freeBytes: 1,
                                               enoughFreeSpace: false, uniformShiftSeconds: 0, estimatedSeconds: -1}});
        compare(findChild(page, "backUpButton").enabled, false);
    }

    function test_runningBackupShowsCancelInsteadOfBackUp() {
        var page = makePage({busy: true, backingUp: true, activity: "backup", phase: "reading",
                             filesDone: 441, filesTotal: 1161, bytesDone: 500, bytesTotal: 1000});
        compare(findChild(page, "backUpButton").visible, false);
        var cancel = findChild(page, "cancelButton");
        compare(cancel.visible, true);
        // At the card's right edge, like the Back Up Now button it replaces.
        // Measured against the card, not the button's own column: that column
        // was right-aligned all along while the row around it had shrunk.
        waitForRendering(page);
        var cancelRight = cancel.mapToItem(page, cancel.width, 0).x;
        var content = findChild(page, "backUpFrame").contentItem;
        var contentRight = content.mapToItem(page, content.width, 0).x;
        verify(Math.abs(cancelRight - contentRight) <= 1,
               "Cancel must sit at the card's right edge: " + cancelRight + " vs " + contentRight);
        if (screenshotDir && screenshotDir.length > 0) {
            grabImage(page).save(screenshotDir + "/stick-backup-running.png");
        }
        cancel.clicked();
        verify(calls(page).indexOf("cancel") >= 0);
        compare(findChild(page, "restoreButton").enabled, false);
    }

    // Keep for later / discard both route to the controller; the dialog
    // cannot be dismissed any other way.
    function test_cancelDecisionDialogRoutesBothChoices() {
        var page = makePage({pendingCancelDecision: true});
        var dialog = findChild(page, "cancelDecisionDialog");
        verify(dialog !== null);
        dialog.open();
        tryVerify(function() { return dialog.visible; });
        compare(dialog.closePolicy, 0);  // Popup.NoAutoClose
        dialog.accept();
        verify(calls(page).indexOf("keepPartial") >= 0);

        var again = makePage({pendingCancelDecision: true});
        var dialog2 = findChild(again, "cancelDecisionDialog");
        dialog2.open();
        tryVerify(function() { return dialog2.visible; });
        findChild(again, "discardPartialButton").clicked();
        verify(calls(again).indexOf("discardPartial") >= 0);
        tryVerify(function() { return !dialog2.visible; });
        compare(findChild(again, "backUpButton").enabled, false);  // still pending until the controller says otherwise
    }

    // A backup that failed verification is a decision, not a message: its
    // files do not match what was written, and a restore would put them
    // back. Delete is the default -- the highlighted button, and what
    // Return does -- and Keep deletes nothing.
    function test_verifyFailedDialogDefaultsToDeleteAndKeepDeletesNothing() {
        var page = makePage({});
        var dialog = findChild(page, "verifyFailedDialog");
        verify(dialog !== null, "a failed verify must have a dialog, not only a message");
        dialog.detail = "3 files in the backup did not match what was written.";
        dialog.open();
        tryVerify(function() { return dialog.visible; });
        compare(dialog.closePolicy, 0);  // Popup.NoAutoClose: it has to be answered
        verify(dialog.title.indexOf("corrupt") >= 0, "the dialog must say the backup is likely corrupt");
        // Waited for rather than read at once: the highlight is assigned
        // when the dialog opens, not while it is being built.
        tryCompare(findChild(page, "deleteFailedBackupButton"), "highlighted", true);
        tryCompare(findChild(page, "keepFailedBackupButton"), "highlighted", false);
        dialog.accept();
        verify(calls(page).indexOf("deleteBackup") >= 0, "the default must delete the backup");

        var again = makePage({});
        var dialog2 = findChild(again, "verifyFailedDialog");
        dialog2.open();
        tryVerify(function() { return dialog2.visible; });
        findChild(again, "keepFailedBackupButton").clicked();
        tryVerify(function() { return !dialog2.visible; });
        compare(calls(again).indexOf("deleteBackup"), -1, "keeping the backup must not delete it");
    }

    function test_compactDialogShowsPreflightAndOnlyProceedsWithSpace() {
        var page = makePage({});
        findChild(page, "compactButton").clicked();
        var dialog = findChild(page, "compactDialog");
        tryVerify(function() { return dialog.visible; });
        verify(calls(page).indexOf("compactionPreflight") >= 0);
        compare(findChild(page, "compactAcceptButton").enabled, true);
        dialog.accept();
        verify(calls(page).indexOf("compact") >= 0);

        var cramped = makePage({preflight: {deadBytes: 4 * 1024 * 1024 * 1024, reclaimableBytes: 4 * 1024 * 1024 * 1024,
                                            archiveBytes: 25 * 1024 * 1024 * 1024, ratio: 0.18,
                                            requiredFreeBytes: 19 * 1024 * 1024 * 1024,
                                            availableFreeBytes: 8 * 1024 * 1024 * 1024, enoughFreeSpace: false}});
        findChild(cramped, "compactButton").clicked();
        var dialog2 = findChild(cramped, "compactDialog");
        tryVerify(function() { return dialog2.visible; });
        compare(findChild(cramped, "compactAcceptButton").enabled, false);
    }

    function test_compactDialogPromisesWhatCompactingFrees() {
        // Dead bytes and what compacting frees differ when entries move
        // across 4 GiB. A sliver of dead space that frees nothing does not
        // offer to compact, and the dialog shows what compacting frees --
        // 5 of 25 GiB, 20% -- not the 18% of dead space.
        var nothing = makePage({preflight: {deadBytes: 1000, reclaimableBytes: 0, archiveBytes: 25 * 1024 * 1024 * 1024,
                                            ratio: 0.0, requiredFreeBytes: 20 * 1024 * 1024 * 1024,
                                            availableFreeBytes: 400 * 1024 * 1024 * 1024, enoughFreeSpace: true}});
        findChild(nothing, "compactButton").clicked();
        var dialog = findChild(nothing, "compactDialog");
        tryVerify(function() { return dialog.visible; });
        compare(findChild(nothing, "compactAcceptButton").enabled, false, "nothing to free, nothing to compact");

        var page = makePage({preflight: {deadBytes: 4.5 * 1024 * 1024 * 1024, reclaimableBytes: 5 * 1024 * 1024 * 1024,
                                         archiveBytes: 25 * 1024 * 1024 * 1024, ratio: 0.18,
                                         requiredFreeBytes: 20 * 1024 * 1024 * 1024,
                                         availableFreeBytes: 400 * 1024 * 1024 * 1024, enoughFreeSpace: true}});
        findChild(page, "compactButton").clicked();
        var dialog2 = findChild(page, "compactDialog");
        tryVerify(function() { return dialog2.visible; });
        var reclaims = findChild(page, "compactReclaimsLabel");
        verify(reclaims !== null);
        verify(reclaims.text.endsWith("(20%)"), reclaims.text);
    }

    // Return answers these dialogs, and answers them with the button the
    // dialog is showing as its default.
    //
    // The three dialogs here supply their own footers, and SeabassDialog's
    // key handling lives on the footer buttons, not on the dialog -- a
    // Dialog is a Popup, and Keys cannot attach to one. A footer that
    // forgets it leaves a dialog that looks answerable from the keyboard,
    // highlight and all, and silently is not.
    //
    // Waits for the HIGHLIGHTED button to hold focus, not for any button.
    // The default is focused by a Qt.callLater when the dialog opens, so
    // a test that presses as soon as something has focus presses before
    // that has run, and measures the box's own initial focus instead of
    // the dialog's answer. That version of this test passed while the
    // shipped behaviour was the opposite one.
    function defaultButtonOf(dialog) {
        var list = dialog.footerButtons();
        for (var i = 0; i < list.length; ++i) {
            if (list[i].highlighted) {
                return list[i];
            }
        }
        return list.length > 0 ? list[list.length - 1] : null;
    }

    function pressReturnOn(dialog) {
        tryVerify(function() { return dialog.visible; });
        tryVerify(function() {
            var target = defaultButtonOf(dialog);
            return target !== null && target.activeFocus;
        }, 5000, "the dialog must put focus on the button it is showing as the default");
        keyClick(Qt.Key_Return);
    }

    // Exact, not a substring of the joined list: "compactionPreflight"
    // contains "compact", so a substring check on this page passes
    // whether or not the compaction ever started.
    function called(page, name) {
        return page.controller.calls.indexOf(name) >= 0;
    }

    // The one dialog in the app where Return performs a destructive
    // action, deliberately: a backup whose contents do not match what was
    // written is worse than no backup, and nothing records the failed
    // verification, so a kept archive goes on showing VERIFIED and is
    // offered as a good backup later. See the dialog's own comment.
    function test_returnTakesTheDefaultOnTheVerifyFailedDialog() {
        var page = makePage({});
        var dialog = findChild(page, "verifyFailedDialog");
        dialog.detail = "3 files in the backup did not match what was written.";
        dialog.open();
        pressReturnOn(dialog);
        verify(called(page, "deleteBackup"), "Return must delete the backup, the marked default");
    }

    // Keeping it is one arrow key away, and deletes nothing.
    function test_theVerifyFailedDialogCanStillKeepFromTheKeyboard() {
        var page = makePage({});
        var dialog = findChild(page, "verifyFailedDialog");
        dialog.detail = "x";
        dialog.open();
        tryVerify(function() { return dialog.visible; });
        var keep = findChild(page, "keepFailedBackupButton");
        tryVerify(function() { return findChild(page, "deleteFailedBackupButton").activeFocus; }, 5000);
        keyClick(Qt.Key_Right);
        tryVerify(function() { return keep.activeFocus; }, 5000, "Right must reach Keep Failed Backup");
        compare(keep.highlighted, true, "and the highlight must follow it");
        keyClick(Qt.Key_Return);
        verify(!called(page, "deleteBackup"), "Return on Keep Failed Backup must delete nothing");
    }

    // The one that matters most: the way out is declared LAST in this
    // footer, and SeabassDialog's fallback marks the last button when a
    // dialog names no default. Here the last button is the destructive
    // answer, so the dialog has to name its default, or Return throws the
    // partial backup away.
    function test_returnKeepsThePartialBackupRatherThanDiscardingIt() {
        var page = makePage({pendingCancelDecision: true});
        var dialog = findChild(page, "cancelDecisionDialog");
        dialog.open();
        tryVerify(function() { return dialog.visible; });
        tryCompare(findChild(page, "keepPartialButton"), "highlighted", true);
        tryCompare(findChild(page, "discardPartialButton"), "highlighted", false);
        pressReturnOn(dialog);
        verify(called(page, "keepPartial"), "Return must keep the partial backup");
        verify(!called(page, "discardPartial"), "Return must never reach the destructive button");
    }

    // Compact is the default here because the button says so with
    // `focus: true`, not because anything highlights it for free --
    // DialogButtonBox never touches `highlighted`. This is also the one
    // of the three with no applyDefaultButton() behind it, so dropping
    // that one line hands Return to Cancel in silence.
    function test_returnOnTheCompactDialogConfirms() {
        var page = makePage({});
        findChild(page, "compactButton").clicked();
        var dialog = findChild(page, "compactDialog");
        tryVerify(function() { return dialog.visible; });
        // Both halves. Highlighting the accept button is the claim; the
        // other button NOT being highlighted is what tells a real default
        // apart from SeabassDialog's take-the-last-button fallback, which
        // is what Windows was getting and which lands on Cancel.
        tryCompare(findChild(page, "compactAcceptButton"), "highlighted", true);
        tryCompare(findChild(page, "compactCancelButton"), "highlighted", false);
        pressReturnOn(dialog);
        verify(called(page, "compact"), "Return must start the compaction it is showing as the default");
    }

    // ...but never when the button itself refuses. footerButtons() skips a
    // disabled button, so the default falls to Cancel and Return cannot
    // start a compaction there is no room for.
    function test_returnCannotCompactWhenTheButtonIsDisabled() {
        var page = makePage({preflight: {deadBytes: 4.2 * 1024 * 1024 * 1024,
                                         reclaimableBytes: 4.2 * 1024 * 1024 * 1024,
                                         archiveBytes: 25 * 1024 * 1024 * 1024, ratio: 0.18,
                                         requiredFreeBytes: 19 * 1024 * 1024 * 1024,
                                         availableFreeBytes: 1 * 1024 * 1024 * 1024, enoughFreeSpace: false}});
        findChild(page, "compactButton").clicked();
        var dialog = findChild(page, "compactDialog");
        tryVerify(function() { return dialog.visible; });
        tryCompare(findChild(page, "compactAcceptButton"), "enabled", false);
        // And the default moves with it, rather than pointing at a button
        // nobody can press.
        tryCompare(findChild(page, "compactCancelButton"), "highlighted", true);
        pressReturnOn(dialog);
        verify(!called(page, "compact"), "Return must not start a compaction the button refuses");
    }

    // The highlight is what the user reads, and focus is what Return
    // acts on, so the two must never disagree. The footer's own contents
    // are a ListView, which will move focus on an arrow key without
    // touching the highlight if it is allowed to see the key first.
    function test_arrowKeysMoveTheHighlightWithTheFocus() {
        var page = makePage({});
        var dialog = findChild(page, "verifyFailedDialog");
        dialog.detail = "x";
        dialog.open();
        tryVerify(function() { return dialog.visible; });
        var del = findChild(page, "deleteFailedBackupButton");
        var keep = findChild(page, "keepFailedBackupButton");
        tryVerify(function() { return del.activeFocus; }, 5000);
        keyClick(Qt.Key_Right);
        tryVerify(function() { return keep.activeFocus; }, 5000, "Right must move the focus");
        compare(keep.highlighted, true, "and the highlight must come with it");
        compare(del.highlighted, false, "the old default must stop looking like the default");
    }

    function test_restoreHandsOffWithStickRootAndArchive() {
        var page = makePage({});
        var got = null;
        page.restoreRequested.connect(function(label, root, archive) { got = {label: label, root: root, archive: archive}; });
        findChild(page, "restoreButton").clicked();
        verify(got !== null);
        compare(got.label, "STICK");
        compare(got.root, "/media/STICK");
        compare(got.archive, "/home/u/Seabass Backups/STICK.zip");
    }

    // The page as the app shows it: inside a StackView, `levelsBelow`
    // pages up from the bottom (1 = pushed from Home, 2 = from a hub on
    // top of Home). The breadcrumb reads the stack's depth to decide
    // whether its middle segment is a link, so a page on its own cannot
    // show that.
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

    function saveCrumbShot(page, name) {
        if (screenshotDir && screenshotDir.length > 0) {
            waitForRendering(page);
            grabImage(page).save(screenshotDir + "/crumb-" + name + ".png");
        }
    }

    // Only ever opened from a stick's Backups page: Home > STICK >
    // Backups > Full Stick Backup, with Backups the way back.
    function test_breadcrumbNamesTheStickAndTheBackupsHub() {
        const page = pushOnStack(2, {
            stickLabel: "STICK",
            rekordboxPath: "/media/STICK/PIONEER",
            enginePath: "/media/STICK/Engine Library",
            appSettingsController: {stickBackupDirectory: "/home/u/Seabass Backups",
                                    experimentalFeaturesEnabled: true},
            controller: makeFakeController({}),
        });
        waitForRendering(page);
        const crumb = Breadcrumb.read(page);
        compare(crumb.stick, "STICK");
        compare(crumb.middle, "Backups");
        verify(crumb.middleIsLink, "Backups is a real level back");
        compare(crumb.title, "Full Stick Backup");
        saveCrumbShot(page, "stick-backup");
    }
}
