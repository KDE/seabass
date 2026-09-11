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
            function save() {}
            function discard() {}
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
}
