// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtTest
import SeabassGui

// The shared dialog frame and its ordinary-case preset, plus their first
// real user: the gate that warns, on entering edit mode, that this stick
// cannot hold a backup and undo will stop being portable.
TestCase {
    id: testCase
    name: "MessageDialog"
    width: 900
    height: 700
    visible: true
    when: windowShown

    readonly property real gb: 1073741824
    readonly property real mb: 1048576

    Component {
        id: messageComponent
        MessageDialog {}
    }

    Component {
        id: sessionComponent
        QtObject {
            property string libraryId: "EB9F-F032"
            property string stickLabel: "WHALESHARK"
            property bool dirty: false
            property bool writing: false
            property int pendingCount: 0
            property string editorOwner: ""
            // What LibraryEditSession reports after measuring the stick.
            property bool backupGoesLocal: false
            property real stickBytesFree: 0
            property real stickBytesCapacity: 0
            property real backupBytesWorstCase: 0
            property string writeLabel: ""
            property int writeCurrent: 0
            property int writeTotal: 0
            property bool cancelRequested: false
            property string state: "idle"
            signal saveFinished(var summary)
            signal lockRefused(var holder)
            // Counted, so a test can tell which way a decline went.
            property int saveCalls: 0
            property int discardCalls: 0
            function save() { saveCalls += 1; }
            function discard() { discardCalls += 1; dirty = false; pendingCount = 0; }
        }
    }

    Component {
        id: registryComponent
        QtObject {
            property var session: null
            property bool quitAfterSave: false
            function openSession(id, label, rb, engine) { return session; }
            function closeSession(id) {}
            function removeLock(id) {}
        }
    }

    Component {
        id: hostComponent
        EditSessionHost { width: 800; height: 600 }
    }

    function findByObjectName(item, name) {
        if (!item) return null;
        if (item.objectName === name) return item;
        var kids = [];
        if (item.contentItem) kids.push(item.contentItem);
        if (item.footer) kids.push(item.footer);
        var children = item.children ? item.children : [];
        for (var i = 0; i < children.length; ++i) kids.push(children[i]);
        var resources = item.resources ? item.resources : [];
        for (var r = 0; r < resources.length; ++r) kids.push(resources[r]);
        for (var j = 0; j < kids.length; ++j) {
            var found = findByObjectName(kids[j], name);
            if (found) return found;
        }
        return null;
    }

    // The space numbers now live on the session, and the decision is made
    // in C++ -- the host only displays it. So the fake session carries
    // both, exactly as LibraryEditSession does.
    function makeHost(props) {
        var session = createTemporaryObject(sessionComponent, testCase, props);
        var registry = createTemporaryObject(registryComponent, testCase, {session: session});
        var host = createTemporaryObject(hostComponent, testCase,
                                         {registry: registry, libraryId: "EB9F-F032",
                                          stickLabel: "WHALESHARK"});
        waitForRendering(host);
        return host;
    }

    // --- the frame --------------------------------------------------------

    function test_buttonsAreForcedToTheRight() {
        // Every Seabass dialog puts its buttons on the right, and the base
        // enforces it rather than trusting each footer to remember.
        var dialog = createTemporaryObject(messageComponent, testCase, {title: "x"});
        verify(dialog.footer !== null);
        compare(dialog.footer.alignment & Qt.AlignRight, Qt.AlignRight);
    }

    function test_frameAndTextShareOnePalette() {
        // The message is drawn in Theme colours, so the frame behind it
        // must be too. Left to the style, the frame followed the system
        // colour scheme while the text stayed Kelp's light grey: on a light
        // scheme the headline was all but invisible, and under Material
        // offscreen the body had no background at all and the text sat on
        // top of whatever the dialog covered.
        var dialog = createTemporaryObject(messageComponent, testCase,
                                           {title: "Delete This Backup?", headline: "It cannot be undone."});
        dialog.open();
        tryVerify(function() { return dialog.opened; });
        verify(dialog.background !== null);
        verify(Qt.colorEqual(dialog.background.color, Theme.surface), "the frame is Theme.surface");
        verify(dialog.header !== null);
        compare(dialog.header.text, "Delete This Backup?");
        verify(Qt.colorEqual(dialog.header.color, Theme.text), "the title is Theme.text");
        verify(Qt.colorEqual(findByObjectName(dialog, "messageLabel").color, Theme.text));
        verify(!dialog.footer.background || !dialog.footer.background.visible,
               "the button row draws no band of its own");
        dialog.close();
    }

    function test_severityPicksTheBadge_data() {
        return [
            {tag: "info", severity: SeabassDialog.Info, glyph: "i"},
            {tag: "warning", severity: SeabassDialog.Warning, glyph: "!"},
            {tag: "error", severity: SeabassDialog.Error, glyph: "✕"},
            {tag: "question", severity: SeabassDialog.Question, glyph: "?"},
        ];
    }

    function test_severityPicksTheBadge(data) {
        var dialog = createTemporaryObject(messageComponent, testCase,
                                           {title: "x", severity: data.severity});
        compare(dialog.severityGlyph, data.glyph);
        verify(findByObjectName(dialog, "severityBadge") !== null);
    }

    function test_destructiveMovesTheDefaultToCancel() {
        // Several of the dialogs this replaces had Delete on AcceptRole,
        // which made Return delete. The safe button takes the default.
        var dialog = createTemporaryObject(messageComponent, testCase, {
            title: "Delete This Backup?",
            headline: "This permanently deletes this one backup copy.",
            acceptText: "Delete",
            destructive: true
        });
        var accept = findByObjectName(dialog.footer, "acceptButton");
        var reject = findByObjectName(dialog.footer, "rejectButton");
        compare(accept.text, "Delete");
        compare(accept.highlighted, false);
        compare(reject.highlighted, true);

        // And it has to survive open(), which is when it matters -- the
        // value is assigned imperatively, so nothing re-establishes it.
        dialog.open();
        tryCompare(dialog, "opened", true);
        compare(accept.highlighted, false);
        compare(reject.highlighted, true);

        // And the other way round for an ordinary confirmation.
        var plain = createTemporaryObject(messageComponent, testCase, {title: "y"});
        compare(findByObjectName(plain.footer, "acceptButton").highlighted, true);
        compare(findByObjectName(plain.footer, "rejectButton").highlighted, false);
    }

    function test_destructiveStillAccepts() {
        // DialogButtonBox turns DestructiveRole into rejected(), so the
        // preset has to raise accepted() itself. Guard that it does.
        var dialog = createTemporaryObject(messageComponent, testCase,
                                           {title: "x", acceptText: "Delete", destructive: true});
        var spy = signalSpy.createObject(testCase, {target: dialog, signalName: "accepted"});
        dialog.open();
        tryCompare(dialog, "opened", true);
        findByObjectName(dialog.footer, "acceptButton").clicked();
        compare(spy.count, 1);
    }

    // A second way to go ahead, beside the default: shown only when it has a
    // label, and it closes the dialog without counting as an accept.
    function test_alternateActionIsASecondWayForward() {
        var plain = createTemporaryObject(messageComponent, testCase, {title: "Stage?", acceptText: "Stage 3"});
        plain.open();
        tryCompare(plain, "opened", true);
        compare(findChild(plain, "alternateButton").visible, false, "no alternate without a label");
        plain.close();

        var dialog = createTemporaryObject(messageComponent, testCase, {
            title: "Stage?", acceptText: "Stage 3 Matching", alternateText: "Stage All 7 Selected"});
        var alternates = 0;
        var accepts = 0;
        dialog.alternateRequested.connect(function() { alternates++; });
        dialog.accepted.connect(function() { accepts++; });
        dialog.open();
        tryCompare(dialog, "opened", true);
        var alternate = findChild(dialog, "alternateButton");
        verify(alternate !== null && alternate.visible, "the alternate button must be shown");
        compare(alternate.text, "Stage All 7 Selected");
        mouseClick(alternate);
        compare(alternates, 1, "clicking it asks for the alternate");
        compare(accepts, 0, "and does not count as accepting");
        tryCompare(dialog, "visible", false);

        // The arrow keys walk all three buttons, the alternate included.
        dialog.open();
        tryCompare(dialog, "opened", true);
        var accept = findChild(dialog, "acceptButton");
        var reject = findChild(dialog, "rejectButton");
        accept.forceActiveFocus();
        keyClick(Qt.Key_Right);
        verify(alternate.activeFocus, "Right from the default must reach the alternate");
        keyClick(Qt.Key_Right);
        verify(reject.activeFocus, "and Right again Cancel");
        keyClick(Qt.Key_Left);
        verify(alternate.activeFocus, "Left from Cancel must come back to the alternate");
        dialog.close();

        dialog.acceptEnabled = false;
        compare(findChild(dialog, "acceptButton").enabled, false, "the default can be switched off");
    }

    // The shared dialog is answered by Return, not only by clicking.
    //
    // The arrow keys were covered here and Return was not, which is how
    // three dialogs on the backup page came to ignore it unnoticed. These
    // press the key rather than calling accept()/reject(), so they fail
    // if the handling on the footer buttons is ever dropped.
    //
    // Waits for the HIGHLIGHTED button to hold focus, not for any button:
    // the default is focused by a Qt.callLater when the dialog opens, so
    // pressing as soon as something has focus measures the box's initial
    // focus instead of the dialog's answer. On a non-destructive dialog
    // the two coincide and the weaker wait passes for the wrong reason.
    function pressReturnOnDialog(dialog) {
        tryVerify(function() { return dialog.opened; });
        tryVerify(function() {
            var list = dialog.footerButtons();
            var target = null;
            for (var i = 0; i < list.length; ++i) {
                if (list[i].highlighted) {
                    target = list[i];
                }
            }
            if (target === null) {
                target = list.length > 0 ? list[list.length - 1] : null;
            }
            return target !== null && target.activeFocus;
        }, 5000, "the dialog must put focus on the button it is showing as the default");
        keyClick(Qt.Key_Return);
    }

    function test_returnTakesTheDefault() {
        var dialog = createTemporaryObject(messageComponent, testCase,
                                           {title: "Stage?", acceptText: "Stage 3"});
        var accepts = 0;
        dialog.accepted.connect(function() { accepts++; });
        dialog.open();
        pressReturnOnDialog(dialog);
        compare(accepts, 1, "Return must take the default");
    }

    // The case the default logic exists for. `destructive` moves the
    // default to Cancel, and until now nothing checked that Return went
    // with it -- only that the highlight did. A dialog that highlights
    // Cancel and deletes on Return is worse than one that never moved the
    // highlight at all.
    function test_returnOnADestructiveDialogRejects() {
        var dialog = createTemporaryObject(messageComponent, testCase, {
            title: "Delete This Backup?",
            headline: "This permanently deletes this one backup copy.",
            acceptText: "Delete",
            destructive: true
        });
        var accepts = 0;
        var rejects = 0;
        dialog.accepted.connect(function() { accepts++; });
        dialog.rejected.connect(function() { rejects++; });
        dialog.open();
        pressReturnOnDialog(dialog);
        compare(accepts, 0, "Return must not delete");
        compare(rejects, 1, "Return must take Cancel, the default a destructive dialog moves to");
    }

    function test_acknowledgementHidesCancel() {
        // Opened first: `visible` is inherited, so every button of a closed
        // dialog reports false and the assertion would pass for free.
        var dialog = createTemporaryObject(messageComponent, testCase,
                                           {title: "x", showReject: false});
        dialog.open();
        tryCompare(dialog, "opened", true);
        compare(findByObjectName(dialog.footer, "acceptButton").visible, true);
        compare(findByObjectName(dialog.footer, "rejectButton").visible, false);
    }

    Component {
        id: signalSpy
        SignalSpy {}
    }

    function test_screenshotEverySeverity() {
        if (!screenshotDir || screenshotDir.length === 0) {
            skip("SEABASS_SCREENSHOT_DIR not set");
        }
        var shots = [
            {name: "dialog-destructive", props: {
                severity: SeabassDialog.Warning, destructive: true,
                title: "Delete This Backup?",
                headline: "This permanently deletes this one backup copy.",
                detailText: "It never touches the stick's live data.",
                acceptText: "Delete"}},
            {name: "dialog-question", props: {
                severity: SeabassDialog.Question,
                title: "Stage all changes?",
                headline: "Copy cues to Engine from DeviceLibrary for 201 track(s).",
                detailText: "Nothing is written yet: Save writes them.",
                acceptText: "Stage All"}},
            {name: "dialog-error", props: {
                severity: SeabassDialog.Error, showReject: false,
                title: "Could not read the library",
                headline: "The database on WHALESHARK could not be opened.",
                detailText: "The stick may have been removed, or the file may be damaged."}},
            {name: "dialog-lowspace", props: {
                severity: SeabassDialog.Warning,
                title: "Not enough room on WHALESHARK",
                headline: "Editing this library needs to back up 812 MB before anything changes, "
                    + "and WHALESHARK has 1.2 GB free.",
                detailText: "The backup will be written to this computer instead. Undo will then "
                    + "work only here, not from another machine with the stick.",
                acceptText: "Back up here and edit"}}
        ];
        for (var i = 0; i < shots.length; ++i) {
            var dialog = createTemporaryObject(messageComponent, testCase, shots[i].props);
            dialog.open();
            tryCompare(dialog, "opened", true);
            waitForRendering(testCase);
            var image = grabImage(testCase);
            image.save(screenshotDir + "/" + shots[i].name + ".png");
            dialog.close();
            tryCompare(dialog, "opened", false);
        }
    }

    // --- its first user: the low-space gate -------------------------------

    function test_silentWhenTheNumbersAreUnknown() {
        // Every page that has not been taught to supply them yet.
        var host = makeHost({});
        compare(host.backupWouldGoLocal, false);
        compare(findByObjectName(host, "lowSpaceDialog").opened, false);
    }

    function test_silentWhenTheStickHasRoom() {
        var host = makeHost({
            backupGoesLocal: false,
            stickBytesCapacity: 30 * testCase.gb,
            stickBytesFree: 20 * testCase.gb,
            backupBytesWorstCase: 318 * testCase.mb
        });
        compare(host.backupWouldGoLocal, false);
        compare(findByObjectName(host, "lowSpaceDialog").opened, false);
    }

    function test_asksOnEnteringEditModeWhenTheStickIsTight() {
        var host = makeHost({
            backupGoesLocal: true,
            stickBytesCapacity: 30 * testCase.gb,
            stickBytesFree: 1.2 * testCase.gb,
            backupBytesWorstCase: 812 * testCase.mb
        });
        compare(host.backupWouldGoLocal, true);
        var dialog = findByObjectName(host, "lowSpaceDialog");
        tryCompare(dialog, "opened", true);

        // The numbers have to be in the message: "low on space" alone does
        // not let anyone decide.
        compare(dialog.title, "Not enough room on WHALESHARK");
        verify(dialog.headline.indexOf("812 MB") >= 0);
        verify(dialog.headline.indexOf("1.2 GB") >= 0);
        verify(dialog.detailText.indexOf("only") >= 0);
    }

    // The measurement arrives late now -- it walks the analysis tree on a
    // worker thread rather than freezing the page's construction for
    // seconds -- so the gate has to open on the answer landing, not only
    // on the page being built.
    function test_asksWhenTheMeasurementArrivesAfterThePageIsUp() {
        var host = makeHost({
            backupGoesLocal: false,
            stickBytesCapacity: 0,
            stickBytesFree: 0,
            backupBytesWorstCase: 0
        });
        var dialog = findByObjectName(host, "lowSpaceDialog");
        compare(dialog.opened, false, "nothing to ask about until the numbers are in");

        // What the worker finishing looks like from QML.
        host.registry.session.stickBytesCapacity = 30 * testCase.gb;
        host.registry.session.stickBytesFree = 1.2 * testCase.gb;
        host.registry.session.backupBytesWorstCase = 812 * testCase.mb;
        host.registry.session.backupGoesLocal = true;

        tryCompare(dialog, "opened", true);
        verify(dialog.headline.indexOf("812 MB") >= 0);
    }

    // And asked once. The property can settle more than once on one
    // session; a question already answered must not be put again.
    function test_doesNotAskTwiceIfTheAnswerSettlesAgain() {
        var host = makeHost({
            backupGoesLocal: true,
            stickBytesCapacity: 30 * testCase.gb,
            stickBytesFree: 1.2 * testCase.gb,
            backupBytesWorstCase: 812 * testCase.mb
        });
        var dialog = findByObjectName(host, "lowSpaceDialog");
        tryCompare(dialog, "opened", true);
        dialog.close();
        tryCompare(dialog, "opened", false);

        host.registry.session.backupGoesLocal = false;
        host.registry.session.backupGoesLocal = true;
        wait(50);
        compare(dialog.opened, false, "the gate must not re-ask once it has been answered");
    }

    // The threshold itself is C++'s, and tests/stick_space_test.cpp covers
    // it -- including that the backup's own size is in the comparison and
    // that headroom scales with the device. What matters here is only that
    // the host shows whatever the session decided.
    function test_theHostShowsWhatTheSessionDecided() {
        var host = makeHost({
            backupGoesLocal: true,
            stickBytesCapacity: 256 * testCase.gb,
            stickBytesFree: 3 * testCase.gb,
            backupBytesWorstCase: 100 * testCase.mb
        });
        compare(host.backupWouldGoLocal, true);
        tryCompare(findByObjectName(host, "lowSpaceDialog"), "opened", true);
    }

    function test_acceptingEditsAnyway() {
        var host = makeHost({
            backupGoesLocal: true,
            stickBytesCapacity: 30 * testCase.gb,
            stickBytesFree: 1.2 * testCase.gb,
            backupBytesWorstCase: 812 * testCase.mb
        });
        var spy = signalSpy.createObject(testCase, {target: host, signalName: "backupLocationAccepted"});
        var dialog = findByObjectName(host, "lowSpaceDialog");
        tryCompare(dialog, "opened", true);
        findByObjectName(dialog.footer, "acceptButton").clicked();
        tryCompare(spy, "count", 1);
    }

    function test_cancellingBeforeAnythingIsStaged() {
        var host = makeHost({
            backupGoesLocal: true,
            stickBytesCapacity: 30 * testCase.gb,
            stickBytesFree: 1.2 * testCase.gb,
            backupBytesWorstCase: 812 * testCase.mb
        });
        var spy = signalSpy.createObject(testCase, {target: host, signalName: "backupLocationDeclined"});
        var dialog = findByObjectName(host, "lowSpaceDialog");
        tryCompare(dialog, "opened", true);
        findByObjectName(dialog.footer, "rejectButton").clicked();
        tryCompare(spy, "count", 1);
        compare(host.dirty, false);
    }

    // --- declining, and when the question may be put ---------------------

    Component {
        id: declineSpyComponent
        SignalSpy {}
    }

    Component {
        id: stackComponent
        StackView { width: 800; height: 600 }
    }

    Component {
        id: coverComponent
        Rectangle { color: "black" }
    }

    // Declining with nothing staged costs nothing: no discard, no save, and
    // the page is asked to leave.
    function test_decliningWithNothingStagedJustLeaves() {
        var host = makeHost({
            backupGoesLocal: true,
            stickBytesCapacity: 30 * testCase.gb,
            stickBytesFree: 1.2 * testCase.gb,
            backupBytesWorstCase: 812 * testCase.mb
        });
        var session = host.registry.session;
        var dialog = findByObjectName(host, "lowSpaceDialog");
        tryCompare(dialog, "opened", true);
        compare(dialog.rejectText, "Cancel");
        var spy = createTemporaryObject(declineSpyComponent, testCase,
                                        {target: host, signalName: "backupLocationDeclined"});
        findByObjectName(dialog.footer, "rejectButton").clicked();
        compare(spy.count, 1);
        compare(session.discardCalls, 0);
        compare(session.saveCalls, 0);
    }

    // The answer can land after a cue was staged. Then declining cannot be
    // free: those changes only save with the backup being declined. It used
    // to send the user to the unsaved-changes dialog, whose default is Save
    // -- the very backup they had just said no to. Now it is named for what
    // it does, it discards, and it never saves.
    function test_decliningWithStagedChangesDiscardsInsteadOfSaving() {
        var host = makeHost({
            backupGoesLocal: true,
            dirty: true,
            pendingCount: 2,
            stickBytesCapacity: 30 * testCase.gb,
            stickBytesFree: 1.2 * testCase.gb,
            backupBytesWorstCase: 812 * testCase.mb
        });
        var session = host.registry.session;
        var dialog = findByObjectName(host, "lowSpaceDialog");
        tryCompare(dialog, "opened", true);
        compare(dialog.rejectText, "Discard changes and leave");
        verify(dialog.detailText.indexOf("2 changes already staged") >= 0);
        var spy = createTemporaryObject(declineSpyComponent, testCase,
                                        {target: host, signalName: "backupLocationDeclined"});
        findByObjectName(dialog.footer, "rejectButton").clicked();
        compare(session.discardCalls, 1);
        compare(session.saveCalls, 0);
        compare(spy.count, 1);
        // Discarded before the page is asked to leave, so its requestLeave()
        // finds nothing unsaved to put a Save button in front of.
        compare(host.dirty, false);
    }

    // The answer arrives from a worker thread, so it can land while another
    // page covers this one -- a track's details over Browse Library. The
    // question belongs to the page that owns the session; asked over the
    // other page, its decline would leave the wrong one.
    function test_waitsWhileItsPageIsCoveredByAnother() {
        var session = createTemporaryObject(sessionComponent, testCase, {
            backupGoesLocal: false,
            stickBytesCapacity: 30 * testCase.gb,
            stickBytesFree: 1.2 * testCase.gb,
            backupBytesWorstCase: 812 * testCase.mb
        });
        var registry = createTemporaryObject(registryComponent, testCase, {session: session});
        var stack = createTemporaryObject(stackComponent, testCase);
        var host = stack.push(hostComponent, {registry: registry, libraryId: "EB9F-F032",
                                              stickLabel: "WHALESHARK"}, StackView.Immediate);
        verify(host !== null);
        stack.push(coverComponent, {}, StackView.Immediate);
        // The premise the host relies on: StackView hides what is underneath.
        tryCompare(host, "visible", false);

        var dialog = findByObjectName(host, "lowSpaceDialog");
        session.backupGoesLocal = true;  // the worker's answer, landing while covered
        // visible, not just opened: opened only turns true once the enter
        // transition has finished, so checking it after a short wait passes
        // even for a dialog that is already on its way up. And long enough
        // for that transition to have run, had it started.
        wait(400);
        compare(dialog.visible, false, "no question over a page that is not showing");
        compare(dialog.opened, false, "no question over a page that is not showing");

        stack.pop(StackView.Immediate);
        tryCompare(host, "visible", true);
        tryCompare(dialog, "opened", true);
    }

    // The session is shared by every page on the library, so what is staged
    // can be another page's. Declining must not discard it, and must leave
    // without stopping to offer to save it.
    function test_decliningLeavesAnotherPagesStagedChangesAlone() {
        var session = createTemporaryObject(sessionComponent, testCase, {
            backupGoesLocal: true,
            dirty: true,
            pendingCount: 3,
            editorOwner: "addcue",
            stickBytesCapacity: 30 * testCase.gb,
            stickBytesFree: 1.2 * testCase.gb,
            backupBytesWorstCase: 812 * testCase.mb
        });
        var registry = createTemporaryObject(registryComponent, testCase, {session: session});
        var host = createTemporaryObject(hostComponent, testCase,
                                         {registry: registry, libraryId: "EB9F-F032",
                                          stickLabel: "WHALESHARK", feature: "sync"});
        waitForRendering(host);
        var dialog = findByObjectName(host, "lowSpaceDialog");
        tryCompare(dialog, "opened", true);
        compare(dialog.rejectText, "Cancel", "another page's changes are not this page's to discard");
        var left = false;
        host.backupLocationDeclined.connect(function() {
            host.requestLeave(function() { left = true; });
        });
        findByObjectName(dialog.footer, "rejectButton").clicked();
        compare(session.discardCalls, 0);
        compare(session.saveCalls, 0);
        compare(left, true, "declining leaves without asking about changes this page does not own");
        compare(findByObjectName(host, "unsavedDialog").visible, false);
    }

    // Asked while the page was showing, then covered -- a push that began
    // before the answer landed and finished after. The question must not
    // stay up over the new top page; it goes away, without that counting
    // as a decline, and comes back when its page does.
    function test_questionGoesAwayIfItsPageIsCoveredWhileItIsUp() {
        var session = createTemporaryObject(sessionComponent, testCase, {
            backupGoesLocal: true,
            stickBytesCapacity: 30 * testCase.gb,
            stickBytesFree: 1.2 * testCase.gb,
            backupBytesWorstCase: 812 * testCase.mb
        });
        var registry = createTemporaryObject(registryComponent, testCase, {session: session});
        var stack = createTemporaryObject(stackComponent, testCase);
        var host = stack.push(hostComponent, {registry: registry, libraryId: "EB9F-F032",
                                              stickLabel: "WHALESHARK"}, StackView.Immediate);
        verify(host !== null);
        var dialog = findByObjectName(host, "lowSpaceDialog");
        tryCompare(dialog, "opened", true);
        var spy = createTemporaryObject(declineSpyComponent, testCase,
                                        {target: host, signalName: "backupLocationDeclined"});

        stack.push(coverComponent, {}, StackView.Immediate);
        tryCompare(host, "visible", false);
        tryCompare(dialog, "visible", false);
        compare(spy.count, 0, "putting the question away is not declining it");

        stack.pop(StackView.Immediate);
        tryCompare(dialog, "opened", true);
    }
}
