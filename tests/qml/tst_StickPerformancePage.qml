import QtQuick
import QtTest
import SeabassGui

// StickPerformancePage.qml against a fake controller carrying the numbers
// measured on a real stick (WHALESHARK2, 2026-09-11): the score block,
// the three advisory rows and the write-test results all render from
// the controller's maps, and the page asks the controller to measure on
// open. Also saves a screenshot when SEABASS_SCREENSHOT_DIR is set.
TestCase {
    id: testCase
    name: "StickPerformancePage"
    width: 1100
    height: 1200
    visible: true
    when: windowShown

    Component {
        id: pageComponent
        StickPerformancePage { width: 1080; height: 1180 }
    }

    function fakeController(withResults, withWrites) {
        var c = {
            busy: false, errorMessage: "", measuredAt: "11 Sep 2026, 16:52", calls: [],
            writeBusy: false, writeErrorMessage: "", needsScratchFiles: false,
            filesystemInfo: {}, measurement: {}, score: {}, advisories: [], facts: {},
            writeMeasurement: {}, writeEstimate: {},
            measure: function(label, rb, en, mp) { this.calls.push("measure:" + label + ":" + mp); },
            measureWithScratchFiles: function(label, mp) { this.calls.push("scratch:" + label + ":" + mp); },
            measureWrites: function(rb, en, mp) { this.calls.push("writes:" + mp); },
            cancel: function() { this.calls.push("cancel"); },
        };
        if (withResults) {
            c.filesystemInfo = {filesystem: "vfat", displayName: "FAT32", recommendedForDjHardware: true,
                                totalBytes: 125800000000, freeBytes: 95000000000, clusterBytes: 32768,
                                usbSpeedLabel: "480 Mbps (USB 2.0 High-Speed)", usbSpeedMbps: 480, stickIdentifier: "x"};
            c.measurement = {streamingBytesPerSecond: 40.0e6, randomReadMedianMs: 1.08, randomReadP95Ms: 1.43,
                             randomReads: 300, smallFileOpensPerSecond: 858, smallFileMedianMs: 1.11,
                             smallFilesRead: 150, catalogBytes: 5009408};
            c.score = {score: 49, speedClass: "Average", browseScore: 28, trackLoadScore: 96, mountScore: 30,
                       browseActionSeconds: 0.0216, trackLoadSeconds: 0.0557, mountSeconds: 0.665,
                       setWaitSeconds: 7.2, referenceSetWaitSeconds: 3.5,
                       setWaitText: "About 7.2 s of waiting across a two-hour set; a current stick on the same port would wait 3.5 s.",
                       browseVerdict: "fine", trackLoadVerdict: "fine", mountVerdict: "fine", streamingVerdict: "fine"};
            c.advisories = [
                {group: "Device Library players", players: "CDJ-2000 · NXS · NXS2 · XDJ-1000MK2 · XDJ-XZ · XDJ-RX3 · CDJ-3000",
                 verdict: "fine", verdictLabel: "FINE",
                 summary: "Reads the database in 4 KiB pages straight off the stick as you browse, the same size as the small random read, and one analysis file per track load. 1.1 ms a page keeps up with the jog wheel."},
                {group: "OneLibrary players", players: "CDJ-3000X · OPUS-QUAD · OMNIS-DUO · XDJ-AZ · rekordbox 7",
                 verdict: "fine", verdictLabel: "FINE",
                 summary: "Runs every list, sort and search as a SQLite query on the stick. Small random reads decide how snappy that feels; this stick answers in 1.1 ms."},
                {group: "Engine OS players", players: "SC5000 · SC6000 · Prime 4 / 4+ · Prime 2 · Prime GO · SC Live",
                 verdict: "slower", verdictLabel: "SLOWER",
                 summary: "Same SQLite-on-the-stick model. Browsing at 1.1 ms per read is noticeably behind a current stick."},
            ];
            c.facts = {clusterBytes: 32768, analysisFiles: 12312, analysisFolders: 2204, audioFiles: 2100, sampleKind: "library"};
        }
        if (withWrites) {
            c.writeMeasurement = {streamingWriteBytesPerSecond: 12.5e6, smallFileWritesPerSecond: 60,
                                  smallFileWriteMedianMs: 15.2, smallFilesWritten: 100, inPlaceUpdateMedianMs: 9.8,
                                  inPlaceUpdates: 100, bytesWritten: 23068672};
            c.writeEstimate = {cueSaveSeconds: 0.07, cueSaveVerdict: "fine", cueSaveVerdictLabel: "FINE",
                               exportTrackSeconds: 0.71, exportHundredTracksSeconds: 71, exportVerdict: "fine",
                               exportVerdictLabel: "FINE"};
        }
        return c;
    }

    function makePage(controller) {
        var page = createTemporaryObject(pageComponent, testCase, {
            stickLabel: "WHALESHARK2", rekordboxPath: "/media/WHALESHARK2/PIONEER",
            enginePath: "/media/WHALESHARK2/Engine Library", mountPoint: "/media/WHALESHARK2", controller: controller,
        });
        verify(page !== null);
        waitForRendering(page);
        return page;
    }

    // Walks children and contentItem; a Control lists its contentItem
    // among its children too, so the same node is seen twice and must
    // only be counted once.
    function findAll(item, objectName) {
        var found = [];
        var seen = [];
        function walk(node) {
            if (seen.indexOf(node) >= 0) return;
            seen.push(node);
            if (node.objectName === objectName) found.push(node);
            for (var i = 0; i < node.children.length; ++i) walk(node.children[i]);
            if (node.contentItem !== undefined && node.contentItem !== null) walk(node.contentItem);
        }
        walk(item);
        return found;
    }

    function findOne(item, objectName) {
        var all = findAll(item, objectName);
        verify(all.length >= 1, "no item named " + objectName);
        return all[0];
    }

    function test_measuresOnOpen() {
        var c = fakeController(false, false);
        var page = makePage(c);
        compare(page.controller.calls.length, 1);
        compare(page.controller.calls[0], "measure:WHALESHARK2:/media/WHALESHARK2");
        compare(findOne(page, "scratchNotice").visible, false);
        compare(findOne(page, "speedClass").text, "Not measured yet");
        compare(findOne(page, "measureButton").text, "Measure");
    }

    function test_rendersScoreAndAdvisories() {
        var c = fakeController(true, false);
        var page = makePage(c);
        compare(findOne(page, "scoreValue").text, "49");
        compare(findOne(page, "speedClass").text, "Average by today's standards");
        verify(findOne(page, "setWaitText").text.indexOf("7.2 s") >= 0);
        compare(findOne(page, "measureButton").text, "Measure Again");
        var rows = findAll(page, "advisoryRow");
        compare(rows.length, 3);
    }

    function test_writeTestRequestsAndRenders() {
        var c = fakeController(true, false);
        var page = makePage(c);
        var button = findOne(page, "writeTestButton");
        compare(button.text, "Run Write Test");
        button.clicked();
        var calls = page.controller.calls;
        compare(calls[calls.length - 1], "writes:/media/WHALESHARK2");

        var withWrites = fakeController(true, true);
        var page2 = makePage(withWrites);
        compare(findOne(page2, "writeTestButton").text, "Run Write Test Again");
        verify(findOne(page2, "cueSaveEstimate").text.indexOf("0.07 s") >= 0);
        verify(findOne(page2, "exportEstimate").text.indexOf("71 s") >= 0);
    }

    // A blank stick: the page says there is nothing to read and offers the
    // throwaway-file measurement, which asks the controller for exactly
    // that with the mount point.
    function test_blankStickOffersThrowawayFiles() {
        var c = fakeController(false, false);
        c.needsScratchFiles = true;
        var page = makePage(c);
        var notice = findOne(page, "scratchNotice");
        compare(notice.visible, true);
        findOne(page, "scratchMeasureButton").clicked();
        var calls = page.controller.calls;
        compare(calls[calls.length - 1], "scratch:WHALESHARK2:/media/WHALESHARK2");
    }

    function test_screenshot() {
        if (!screenshotDir || screenshotDir.length === 0) {
            skip("SEABASS_SCREENSHOT_DIR not set");
        }
        var page = makePage(fakeController(true, true));
        grabImage(page).save(screenshotDir + "/stick-performance-page.png");
    }
}
