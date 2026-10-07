// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtTest
import SeabassGui
import "Breadcrumb.js" as Breadcrumb

// The Library Health hub: one card per check, each saying what it found in
// a sentence. Drives HealthCheckCard directly -- the hub itself needs a
// real LibraryConsistencyController, which needs a library on disk -- so
// what is covered here is the part that decides what the user reads.
TestCase {
    id: testCase
    name: "LibraryHealthHub"
    width: 800
    height: 600
    visible: true
    when: windowShown

    Component {
        id: cardComponent
        HealthCheckCard {}
    }

    // The hub page itself. Pointed at a stick that is not there, the same
    // way tst_PagesCompile does it: nothing scans, but the page still
    // builds and lays out, which is all the margin check needs.
    // The page requires a playback controller; the real one, unpointed at
    // any stick, the way tst_PagesCompile builds its pages.
    PlaybackController { id: realPlayback }

    Component {
        id: pageComponent
        LibraryHealthHubPage {
            playbackController: realPlayback
            stickLabel: "TESTSTICK"
            rekordboxPath: "/nonexistent/TESTSTICK/PIONEER"
            enginePath: "/nonexistent/TESTSTICK/Engine Library"
        }
    }

    readonly property string fixtureEngineRoot: {
        const url = Qt.resolvedUrl("../fixtures/anonymized_library/engine").toString();
        return decodeURIComponent(url.replace(/^file:\/\//, "").replace(/^\/([A-Za-z]:)/, "$1"));
    }
    property string uncheckedLibrary: ""

    Component {
        id: uncheckedPageComponent
        LibraryHealthHubPage {
            playbackController: realPlayback
            stickLabel: "UNCHECKED"
            rekordboxPath: ""
            enginePath: testCase.uncheckedLibrary
        }
    }

    function findByObjectName(item, name) {
        if (!item) return null;
        if (item.objectName === name) return item;
        var kids = item.children ? item.children : [];
        for (var i = 0; i < kids.length; ++i) {
            var found = findByObjectName(kids[i], name);
            if (found) return found;
        }
        return null;
    }

    // The repair takes the stick away and brings it back on purpose, and
    // the window asks the page in front whether that is expected before it
    // says "USB stick removed" -- a scary message to get for the middle of
    // a job the user just pressed for. The repair below finds no device
    // behind this made-up path and gives up straight away, which is all
    // this needs: the flag has to be up for as long as it runs.
    function test_theStickIsNotAnnouncedAsGoneWhileItIsBeingRepaired() {
        var page = createTemporaryObject(pageComponent, testCase);
        tryCompare(page.consistencyController, "busy", false);
        compare(page.stickAwayExpected, false);
        page.consistencyController.repairStickFilesystem();
        // Set before the worker starts and nothing can run a queued slot
        // inside this call, so this is the state during the repair.
        compare(page.stickAwayExpected, true);
        tryCompare(page.consistencyController, "repairingFilesystem", false);
        compare(page.stickAwayExpected, false);
    }

    // Nor can the page be left while the repair runs: it cannot be
    // stopped, the controller would wait for it with the window frozen,
    // and the page is where it says how it went. Same synchronous window
    // as above.
    function test_backWaitsForTheFilesystemRepair() {
        const page = createTemporaryObject(pageComponent, testCase);
        tryCompare(page.consistencyController, "busy", false);
        const home = findChild(page, "homeCrumb");
        verify(home !== null, "the breadcrumb is on the page");
        compare(home.enabled, true);
        page.consistencyController.repairStickFilesystem();
        compare(home.enabled, false, "Back is disabled while the stick is being repaired");
        tryCompare(page.consistencyController, "repairingFilesystem", false);
        compare(home.enabled, true);
    }

    SignalSpy {
        id: repairFinishedSpy
        signalName: "filesystemRepairFinished"
    }

    // A repair that THROWS says why. The result was taken without asking
    // for the exception, which hands back a default result -- not
    // repaired, not declined, no message -- so the page showed an empty
    // error and the user was told nothing. Every other result in the
    // controller was taken with it; this one was not.
    function test_aRepairThatThrowsSaysWhy() {
        var page = createTemporaryObject(pageComponent, testCase);
        var controller = page.consistencyController;
        tryCompare(controller, "busy", false);
        controllerFixture.makeFilesystemRepairThrow("the repair helper stopped responding");
        repairFinishedSpy.clear();
        repairFinishedSpy.target = controller;
        controller.repairStickFilesystem();
        repairFinishedSpy.wait(5000);
        controllerFixture.restoreFilesystemRepair();
        compare(repairFinishedSpy.count, 1);
        const repaired = repairFinishedSpy.signalArguments[0][0];
        const declined = repairFinishedSpy.signalArguments[0][1];
        const message = repairFinishedSpy.signalArguments[0][2];
        compare(repaired, false);
        compare(declined, false, "a crash is not the user saying no");
        verify(message.indexOf("the repair helper stopped responding") !== -1,
               "the reason reaches the page, got: '" + message + "'");
        verify(controller.errorMessage.indexOf("the repair helper stopped responding") !== -1,
               "and is shown as the error, got: '" + controller.errorMessage + "'");
    }

    SignalSpy {
        id: detailSpy
        signalName: "detailRequested"
    }

    // Every card that offers a review asks for its OWN check's page. They
    // all used to open one shared page with every check's repair on it, so
    // a press on "Review sample rates" landed on a list of missing files;
    // a section repeated between two cards would put that back for them.
    // Main maps each of these to a page (healthCheckPage()).
    function test_eachCardAsksForItsOwnChecksPage() {
        var page = createTemporaryObject(pageComponent, testCase);
        tryCompare(page.consistencyController, "busy", false);
        detailSpy.clear();
        detailSpy.target = page;
        var expected = [
            {card: "brokenFilesCard", section: "broken"},
            {card: "junkCuesCard", section: "junkcues"},
            {card: "importPromptCard", section: "import"},
            {card: "sampleRateCard", section: "samplerates"},
            {card: "coverArtCard", section: "artwork"},
            {card: "playlistSyncCard", section: "playlists"},
            {card: "noPlaylistCard", section: "noplaylist"},
            {card: "cueTableCard", section: "cuetables"},
        ];
        for (var i = 0; i < expected.length; ++i) {
            var card = findByObjectName(page, expected[i].card);
            verify(card !== null, expected[i].card + " exists");
            card.actionRequested();
            compare(detailSpy.count, i + 1, expected[i].card + " asks for a page");
            compare(detailSpy.signalArguments[i][0], expected[i].section, expected[i].card);
        }

        // And from the page itself rather than from the table above: every
        // card in the column, pressed in turn, including any added after
        // this was written. No two may ask for the same page. The
        // filesystem card opens its own dialog instead, and track analysis
        // only reports, so neither asks for anything.
        detailSpy.clear();
        var column = findByObjectName(page, "healthColumn");
        verify(column !== null);
        var askedBy = {};
        var cards = 0;
        for (var c = 0; c < column.children.length; ++c) {
            var item = column.children[c];
            if (item.actionRequested === undefined || item.hasTally === undefined) {
                continue;
            }
            ++cards;
            var before = detailSpy.count;
            item.actionRequested();
            if (detailSpy.count === before) {
                continue;
            }
            var asked = detailSpy.signalArguments[before][0];
            verify(askedBy[asked] === undefined,
                   item.objectName + " asks for \"" + asked + "\", which " + askedBy[asked] + " already does");
            askedBy[asked] = item.objectName;
        }
        // The filesystem card's press opened its advice dialog.
        var advice = findChild(page, "repairAdviceDialog");
        if (advice !== null) {
            advice.close();
        }
        compare(cards, expected.length + 2, "every card on the page was pressed");
        compare(detailSpy.count, expected.length, "and all but the two without a page asked for one");
    }

    function test_aCleanCheckStillGetsACard() {
        // "Nothing wrong here" is a result. A page that only lists problems
        // cannot distinguish a clean library from a check that never ran.
        var card = createTemporaryObject(cardComponent, testCase, {
            title: "Tracks and their files",
            summary: "Every track in every catalog on this stick points at a file that is really there.",
            ok: true
        });
        compare(findByObjectName(card, "checkTitle").text, "Tracks and their files");
        verify(findByObjectName(card, "checkSummary").text.length > 0);
        // No action offered when there is nothing to act on.
        compare(findByObjectName(card, "checkAction").visible, false);
    }

    // #38. A finding with no action and no tally is a shape nothing else
    // on this page has, and both halves of it are easy to undo by
    // accident: someone adds a fixableCount "for consistency" and the
    // card starts claiming Seabass can fix none of them, or someone adds
    // an actionLabel and it offers a button for work it must not do.
    function test_aReportOnlyFindingOffersNoActionAndNoTally() {
        var card = createTemporaryObject(cardComponent, testCase, {
            title: "Track analysis",
            summary: "1214 of 1564 tracks have not been analysed for Engine players. Seabass does not do this "
                   + "itself: the analysis is the player's own beatgrid, waveform and key detection.",
            ok: false
        });
        verify(findByObjectName(card, "checkSummary").text.indexOf("1214 of 1564") >= 0);
        // The number is in the sentence, not drawn as "fixable / found".
        compare(card.hasTally, false);
        // And nothing offers to do the work.
        compare(card.actionLabel, "");
        compare(findByObjectName(card, "checkAction").visible, false);
    }

    // The other half: the page must not say "every track is analysed"
    // about a library that simply cannot say. An Engine 1.x library and a
    // fully analysed one both report zero, and they are not the same.
    function test_aLibraryThatCannotSayIsNotReportedAsClean() {
        var page = createTemporaryObject(pageComponent, testCase);
        tryCompare(page.consistencyController, "busy", false);
        // No stick, so nothing was read and nothing is known.
        compare(page.consistencyController.analysisKnown, false);
        verify(page.analysisSummary.indexOf("Every Engine track has been analysed") < 0);
    }

    // Covers the audit could not read to check are not a clean result: the
    // card is not green, and it offers the page that says what they are.
    function test_coversThatCouldNotBeCheckedKeepTheCardOpen() {
        testCase.uncheckedLibrary = artworkFixture.libraryWithUncheckedCovers(testCase.fixtureEngineRoot);
        verify(testCase.uncheckedLibrary.length > 0, "the fixture copy must be made");
        const page = createTemporaryObject(uncheckedPageComponent, testCase);
        tryVerify(function() { return page.consistencyController.busy === false; }, 300000, "the scan must finish");
        compare(page.consistencyController.artworkUnreadableCount, 0);
        verify(page.consistencyController.artworkUncheckedCount > 0);
        const card = findByObjectName(page, "coverArtCard");
        verify(card !== null, "the hub has a cover art card");
        compare(card.ok, false, "a cover that could not be checked is not healthy");
        compare(card.actionLabel, "Review cover art");
        verify(card.summary.indexOf("can find it") < 0, "and it does not say every cover is found: " + card.summary);
    }

    function test_aFindingOffersItsOneAction() {
        var card = createTemporaryObject(cardComponent, testCase, {
            title: "Memory cues at 0:00",
            summary: "27 memory cues sit at 0:00.",
            ok: false,
            actionLabel: "Review these cues"
        });
        var action = findByObjectName(card, "checkAction");
        compare(action.text, "Review these cues");
        compare(action.enabled, true);

        var fired = 0;
        card.actionRequested.connect(function() { fired++; });
        action.clicked();
        compare(fired, 1);
    }

    function test_aBlockedActionSaysWhy() {
        // Rather than a button that silently does nothing -- the case the
        // format-divergence spec runs into, where Seabass can remove a row
        // but cannot yet add one.
        var card = createTemporaryObject(cardComponent, testCase, {
            title: "Library formats in step",
            summary: "483 tracks exist in OneLibrary but not in rekordbox.",
            ok: false,
            actionLabel: "Add them to rekordbox",
            actionEnabled: false,
            actionDisabledReason: "Seabass cannot add rows to export.pdb yet."
        });
        compare(findByObjectName(card, "checkAction").enabled, false);
        var reason = findByObjectName(card, "checkActionReason");
        compare(reason.visible, true);
        verify(reason.text.indexOf("cannot add rows") >= 0);
    }

    // The action sits on the card's right-hand side, beside the text,
    // rather than in a row of its own under it. It used to take a whole
    // line at the bottom of every card that had one, left-aligned, so a
    // page of findings read as a column of buttons interleaved with the
    // prose. Checked at widths from a narrow window to a wide one, and
    // on every shape a card takes: with a tally, without, blocked with a
    // reason, and passing with no action at all.
    function test_theActionSitsOnTheRightOfTheText_data() {
        const shapes = [
            {shape: "tally", props: {title: "Cover art", summary: "29 of 1271 Engine tracks have cover art no "
                + "player can show. 27 of them can be rebuilt from the rekordbox art on this stick.",
                ok: false, fixableCount: 27, foundCount: 29, actionLabel: "Review cover art"}},
            {shape: "noTally", props: {title: "The stick itself", summary: "The stick's filesystem is marked "
                + "as needing a check, so the system mounted it read-only.",
                ok: false, failed: true, actionLabel: "Check and Repair"}},
            {shape: "blocked", props: {title: "Cues at 0:00", summary: "27 cues sit at 0:00.",
                ok: false, fixableCount: 27, foundCount: 27, actionLabel: "Review these cues",
                actionEnabled: false, actionDisabledReason: "The stick is read-only."}},
            {shape: "clean", props: {title: "Tracks and their files", summary: "Every track in every "
                + "catalog on this stick points at a file that is really there.", ok: true}},
        ];
        const rows = [];
        const widths = [380, 640, 900];
        for (let w = 0; w < widths.length; ++w) {
            for (let s = 0; s < shapes.length; ++s) {
                rows.push({tag: shapes[s].shape + "@" + widths[w], width: widths[w], props: shapes[s].props});
            }
        }
        return rows;
    }

    function test_theActionSitsOnTheRightOfTheText(row) {
        const card = createTemporaryObject(cardComponent, testCase, row.props);
        card.width = row.width;
        waitForRendering(card);
        const title = findByObjectName(card, "checkTitle");
        const summary = findByObjectName(card, "checkSummary");
        const action = findByObjectName(card, "checkAction");
        const tx = title.mapToItem(card, 0, 0).x;
        // One left line for everything that is text.
        compare(summary.mapToItem(card, 0, 0).x, tx, "summary and title share a left edge");
        const reason = findByObjectName(card, "checkActionReason");
        if (reason.visible) {
            compare(reason.mapToItem(card, 0, 0).x, tx, "the reason is on the text's left line too");
        }
        // The card's own right inset: the tally or the tick sits against
        // it, and so must the action, so every card shares a right edge.
        const marker = card.hasTally ? findByObjectName(card, "checkTally")
                                     : findByObjectName(card, "checkPassedMark");
        const inner = card.width - card.contentInset;
        if (marker.visible) {
            fuzzyCompare(marker.mapToItem(card, marker.width, 0).x, inner, 0.5,
                         "the tally or tick ends on the card's right inset");
        }
        if (row.props.actionLabel === undefined) {
            compare(action.visible, false, "nothing to act on, no action");
            return;
        }
        verify(action.visible, "the action is shown");
        const a = action.mapToItem(card, 0, 0);
        fuzzyCompare(a.x + action.width, inner, 0.5, "the action ends on the card's right inset");
        // Beside the text, not under it: it starts right of where the
        // summary ends, and its top is above the summary's bottom.
        const s = summary.mapToItem(card, 0, 0);
        verify(a.x >= s.x + summary.width,
               "action at x=" + a.x + " overlaps the summary ending at " + (s.x + summary.width));
        verify(a.y < s.y + summary.height,
               "action at y=" + a.y + " sits below the summary ending at " + (s.y + summary.height));
        verify(a.x > card.width / 2, "the action is on the right-hand side, x=" + a.x);
    }

    function test_screenshotOfTheCardStates() {
        if (!screenshotDir || screenshotDir.length === 0) {
            skip("SEABASS_SCREENSHOT_DIR not set");
        }
        var states = [
            {name: "health-card-clean", props: {title: "Tracks and their files",
                summary: "Every track in every catalog on this stick points at a file that is really there.",
                ok: true}},
            {name: "health-card-finding", props: {title: "Memory cues at 0:00",
                summary: "27 cues sit at 0:00. These are almost always accidental: a stray press "
                       + "while the track was at the start, rather than something you placed on purpose.",
                ok: false, actionLabel: "Review these cues"}},
            {name: "health-card-tally-some", props: {title: "Cover art",
                summary: "29 of 1271 Engine tracks have cover art no player can show. 27 of them can be rebuilt "
                       + "from the rekordbox art on this stick, the tracks' own tags, or a backup on this computer.",
                ok: false, fixableCount: 27, foundCount: 29, actionLabel: "Review cover art"}},
            {name: "health-card-tally-none", props: {title: "Cover art",
                summary: "2 of 1271 Engine tracks have cover art no player can show. No copy of them was found on "
                       + "this stick, in the tracks themselves, or in a backup.",
                ok: false, fixableCount: 0, foundCount: 2, actionLabel: "Review cover art"}},
            {name: "health-card-tally-all", props: {title: "Memory cues at 0:00",
                summary: "27 cues sit at 0:00, and every one of them can be taken out.",
                ok: false, fixableCount: 27, foundCount: 27, actionLabel: "Review these cues"}},
            // #38: a finding that reports and offers nothing. Worth a
            // frame of its own because it is the only card with neither
            // a tally nor an action, and "looks unfinished" is a real
            // risk for that shape.
            {name: "health-card-report-only", props: {title: "Track analysis",
                summary: "1214 of 1564 tracks have not been analysed for Engine players. The player analyses each "
                       + "one the first time it is loaded, which takes a moment, happens once, and is written back "
                       + "to the stick, but that moment is spent on the deck. Load them once before the gig, or "
                       + "let Engine DJ analyse the library, and the first load at the gig is instant. Seabass does "
                       + "not do this itself: the analysis is the player's own beatgrid, waveform and key "
                       + "detection.",
                ok: false}},
            {name: "health-card-running", props: {title: "Tracks and their files",
                summary: "Checking every row in every catalog against the files on the stick (rekordbox)...",
                running: true}}
        ];
        for (var i = 0; i < states.length; ++i) {
            var card = createTemporaryObject(cardComponent, testCase, states[i].props);
            card.width = 640;
            waitForRendering(card);
            grabImage(card).save(screenshotDir + "/" + states[i].name + ".png");
        }
        // Every state stacked, the way the hub shows them, at a narrow, a
        // middling and a wide width: whether the cards agree with each
        // other (one left line, one right edge) is only visible with
        // them side by side.
        // The window widened too: an item wider than it is clipped.
        const wasHeight = testCase.height;
        const wasWidth = testCase.width;
        testCase.height = 1500;
        const widths = [380, 640, 900];
        for (let w = 0; w < widths.length; ++w) {
            testCase.width = Math.max(wasWidth, widths[w]);
            const stack = createTemporaryObject(stackComponent, testCase, {width: widths[w]});
            for (let i = 0; i < states.length; ++i) {
                const props = Object.assign({}, states[i].props);
                cardComponent.createObject(stack.column, props);
            }
            waitForRendering(stack);
            grabImage(stack).save(screenshotDir + "/health-cards-stack-" + widths[w] + ".png");
        }
        testCase.height = wasHeight;
        testCase.width = wasWidth;
    }

    Component {
        id: stackComponent
        Rectangle {
            color: Theme.background
            implicitHeight: stackColumn.implicitHeight + 2 * Theme.pageMargin
            height: implicitHeight
            readonly property alias column: stackColumn
            ColumnLayout {
                id: stackColumn
                x: Theme.pageMargin
                y: Theme.pageMargin
                width: parent.width - 2 * Theme.pageMargin
                spacing: Theme.sectionSpacing
            }
        }
    }
    // The whole page, not one card at a time. The card screenshots above
    // each render a card on its own at a fixed width, so they cannot show
    // the thing alignment actually is: whether the cards agree with each
    // other and with the page's own left line. A card that is correct in
    // isolation and inset differently from its neighbours looks fine in
    // every frame above.
    function test_screenshotOfTheWholePage() {
        if (!screenshotDir || screenshotDir.length === 0) {
            skip("SEABASS_SCREENSHOT_DIR not set");
        }
        // Tall enough for every card at once. At the TestCase's own 600
        // the grab stops after the fifth card, so the frame that was
        // supposed to show the whole page showed most of it -- and the
        // card added most recently is the one off the bottom.
        var wasHeight = testCase.height;
        testCase.height = 1500;
        var page = createTemporaryObject(pageComponent, testCase);
        page.width = testCase.width;
        page.height = testCase.height;
        tryCompare(page.consistencyController, "busy", false);
        waitForRendering(page);
        grabImage(page).save(screenshotDir + "/health-hub-page.png");

        // Left edges, mapped into the page, printed rather than asserted:
        // this case exists to LOOK at the page, and a number beside the
        // picture is what makes "that card is out" checkable instead of a
        // feeling.
        var names = ["stickFilesystemCard", "brokenFilesCard", "junkCuesCard", "importPromptCard",
                     "sampleRateCard", "analysisStateCard", "coverArtCard", "playlistSyncCard"];
        for (var i = 0; i < names.length; ++i) {
            var card = findByObjectName(page, names[i]);
            if (card) {
                var p = card.mapToItem(page, 0, 0);
                console.log("  left edge  " + names[i] + " x=" + p.x + " width=" + card.width);
            } else {
                console.log("  left edge  " + names[i] + " NOT FOUND");
            }
        }
        // The status line, the button, and the button's own label. A
        // Button's item can sit on the left line while its TEXT does not,
        // because the control carries horizontal padding -- and on a flat
        // button with no visible background, the text IS the left edge as
        // far as anyone looking at the page is concerned.
        var status = findByObjectName(page, "errorLabel");
        var button = findByObjectName(page, "recheckButton");
        if (button) {
            var bp = button.mapToItem(page, 0, 0);
            console.log("  left edge  recheckButton item x=" + bp.x + " width=" + button.width
                        + " leftPadding=" + button.leftPadding);
            if (button.contentItem) {
                var cp = button.contentItem.mapToItem(page, 0, 0);
                console.log("  left edge  recheckButton TEXT x=" + cp.x);
            }
        }
        if (status) {
            var sp = status.mapToItem(page, 0, 0);
            console.log("  left edge  errorLabel x=" + sp.x);
        }
        // TEXT left edges, which is what the eye reads as the left line.
        // Item edges are not the same thing: a card sits at 16 and draws
        // its title at 16 + its own padding, so anything that aligns to
        // the CARD rather than to the card's TEXT is out by that padding.
        function textEdges(item, depth, out) {
            if (!item) return;
            var kids = item.children ? item.children : [];
            for (var i = 0; i < kids.length; ++i) {
                var k = kids[i];
                if (k.text !== undefined && String(k.text).length > 0 && k.visible) {
                    var pt = k.mapToItem(page, 0, 0);
                    out.push({x: Math.round(pt.x), y: Math.round(pt.y),
                              t: String(k.text).substring(0, 46)});
                }
                textEdges(k, depth + 1, out);
            }
        }
        var found = [];
        textEdges(page, 0, found);
        found.sort(function (a, b) { return a.y - b.y; });
        for (var j = 0; j < found.length; ++j) {
            console.log("  TEXT x=" + found[j].x + "  y=" + found[j].y + "  " + found[j].t);
        }
        testCase.height = wasHeight;
    }

    // The page used to fill its parent with no margins at all, so every
    // card ran into the window edge while every sibling page inset its
    // content by 16. Asserted on both sides: a left-only anchor would
    // satisfy a check that only looked at x.
    // The breadcrumb was the page's bare header, so it stretched across
    // the full width and its text started at the header's own inset
    // rather than the body's. It is a ToolBar now, like every other
    // section page, and this asserts the thing that was actually wrong:
    // the two left edges agree.
    function test_header_text_lines_up_with_the_body() {
        var page = createTemporaryObject(pageComponent, testCase, {width: 800, height: 600});
        waitForRendering(page);
        var scroll = findByObjectName(page, "healthScroll");
        verify(scroll !== null);
        var crumbText = null;
        function walk(item) {
            for (var i = 0; i < item.children.length; ++i) {
                var child = item.children[i];
                // The header's edge on the page's line is the round
                // Back button's ring, so the button itself is what has
                // to line up with the body.
                if (child.objectName === "backButton") {
                    crumbText = child;
                }
                walk(child);
            }
        }
        walk(page);
        verify(crumbText !== null, "the Home crumb's content item was not found");
        compare(crumbText.mapToItem(page, 0, 0).x, scroll.x,
                "breadcrumb text and page content must share a left edge");
    }

    function test_content_is_inset_from_both_edges() {
        var page = createTemporaryObject(pageComponent, testCase, {width: 800, height: 600});
        waitForRendering(page);
        var scroll = findByObjectName(page, "healthScroll");
        var column = findByObjectName(page, "healthColumn");
        verify(scroll !== null);
        verify(column !== null);
        compare(scroll.x, 16);
        compare(page.width - (scroll.x + scroll.width), 16);
        // And the content inside it stays within that inset box.
        verify(column.width > 0);
        verify(column.width <= scroll.width);
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
    // page; opened from Restore a Stick Backup it names that page too, as the way back,
    // rather than putting the stick's name on a link to it.
    function test_breadcrumb_data() {
        return [
            {tag: "home", below: 1, hubLabel: "", middle: "", link: false},
            {tag: "nested", below: 2, hubLabel: "Restore a Stick Backup", middle: "Restore a Stick Backup", link: true},
        ];
    }

    function test_breadcrumb(data) {
        const props = {};
        props.hubLabel = data.hubLabel;
        const page = pushOnStack(data.below, props);
        waitForRendering(page);
        const crumb = Breadcrumb.read(page);
        compare(crumb.stick, "TESTSTICK");
        compare(crumb.middle, data.middle);
        compare(crumb.middleIsLink, data.link);
        compare(crumb.title, "Library Health");
    }

    // A check that finishes with something to offer shows its button beside
    // the summary, and the layout never finds the button's cell taken on the
    // way ("QGridLayoutEngine::addItem: Can't add ..."). The storm saw that
    // warning each time the hub came back from under a check page: the
    // summary's span followed the button's visibility, the parents' included,
    // one step behind it. The last step below is the one that went red.
    function test_a_check_that_ends_with_an_action_lays_out_cleanly() {
        failOnWarning(/QGridLayoutEngine/);
        const card = createTemporaryObject(cardComponent, testCase, {
            title: "Missing files", summary: "Checking...", running: true, width: 700,
        });
        verify(card);
        waitForRendering(card);
        card.actionLabel = "Review";
        card.summary = "3 tracks point at files that are not on the stick.";
        card.running = false;
        waitForRendering(card);
        const action = findChild(card, "checkAction");
        verify(action.visible, "the button shows once the check is over");
        const summary = findChild(card, "checkSummary");
        verify(summary.x + summary.width <= action.x, "and the summary stays left of it");
        card.running = true;
        waitForRendering(card);
        card.running = false;
        waitForRendering(card);
        // And a card that is built with its action already there, as a
        // page opened after the scan builds its cards.
        const built = createTemporaryObject(cardComponent, testCase, {
            title: "Cues at 0:00", summary: "2 cues sit at 0:00.", actionLabel: "Remove", width: 700,
        });
        verify(built);
        waitForRendering(built);
        verify(findChild(built, "checkAction").visible);
        // A page under another one in the StackView is hidden and shown
        // again, and an item's visible is its parents' as well: the button
        // goes and comes back with the page.
        built.visible = false;
        waitForRendering(testCase);
        built.visible = true;
        waitForRendering(built);
        verify(findChild(built, "checkAction").visible);
    }

    readonly property string fixtureRoot: {
        const url = Qt.resolvedUrl("../fixtures/anonymized_library").toString();
        return decodeURIComponent(url.replace(/^file:\/\//, "").replace(/^\/([A-Za-z]:)/, "$1"));
    }

    Component {
        id: stickPageComponent
        LibraryHealthHubPage {
            playbackController: realPlayback
            stickLabel: "TESTSTICK"
            rekordboxPath: ""
            enginePath: ""
        }
    }

    // Every step of the check counts. It reads each catalog and then runs
    // its audits: the cover-art audit, the
    // analysis count and the sample-rate audit, which on a full USB stick
    // opens an audio file per Engine row without a rate. None of those
    // used to report, so the bar swept (or sat full) for minutes and the
    // check looked hung. Each now starts its own total and counts, and the
    // hub's progress shows the step and its count.
    //
    // Recorded as each count changes, on the real check of the committed
    // fixture; the bindings under test are re-evaluated before a handler
    // connected here runs.
    function test_everyStepOfTheCheckIsCounted() {
        const stick = stickFixture.stickCopy(testCase.fixtureRoot);
        verify(stick.length > 0, "the fixture must copy");
        const page = createTemporaryObject(stickPageComponent, testCase,
                                           {rekordboxPath: stick + "/PIONEER", enginePath: stick + "/Engine Library"});
        const controller = page.consistencyController;
        const report = findChild(page, "healthScanProgress");
        verify(report !== null, "the hub shows the check's progress");
        const seen = {};
        // #58: one check, one bar. Every total announced and every count
        // shown, in order, so the test can say the total never changed
        // and the count never went back.
        const totals = [];
        const counts = [];
        const record = () => {
            // Where the bar stood before this step's first tick: the
            // count the step before it reached (a one-unit step ticks
            // once, so its own first tick is already its last).
            const before = counts.length > 0 ? counts[counts.length - 1] : 0;
            if (controller.scanTotal > 0) {
                totals.push(controller.scanTotal);
                counts.push(controller.scanCurrent);
            }
            if (controller.scanPhase.length === 0 || controller.scanTotal <= 0) {
                return;
            }
            const entry = seen[controller.scanPhase] ?? {counted: false, shown: false, first: before,
                                                         last: controller.scanCurrent};
            entry.last = Math.max(entry.last, controller.scanCurrent);
            entry.counted = entry.counted || (report.visible && !report.indeterminate
                                              && report.unitsTotal === controller.scanTotal);
            entry.shown = entry.shown || findChild(report, "currentItemLabel").text === controller.scanPhase;
            seen[controller.scanPhase] = entry;
        };
        controller.scanProgressChanged.connect(record);
        verify(controller.busy, "precondition: the check is running");
        tryVerify(() => !controller.busy, 300000, "the check finishes");
        controller.scanProgressChanged.disconnect(record);
        compare(controller.errorMessage, "", "the check read everything");
        verify(!report.visible, "the progress goes once the check is done");

        // Not "Reading cover images": this fixture keeps its covers as
        // files, so that pass has no rows (engine_artwork_test counts it).
        const steps = ["Checking cover art", "Counting tracks the player will analyse", "Checking sample rates"];
        for (const step of steps) {
            const entry = seen[step];
            verify(entry !== undefined, "\"" + step + "\" was a step of the bar; seen: " + JSON.stringify(seen));
            verify(entry.last > entry.first, "\"" + step + "\" moved the bar: " + JSON.stringify(entry));
            verify(entry.counted, "\"" + step + "\" showed a counted bar, not a sweeping one");
            verify(entry.shown, "\"" + step + "\" was named under the bar");
        }
        verify(totals.length > 0, "the bar had a total");
        for (const total of totals) {
            compare(total, totals[0], "one total for the whole check, never replaced");
        }
        for (let i = 1; i < counts.length; ++i) {
            verify(counts[i] >= counts[i - 1], "the count never goes back: " + JSON.stringify(counts));
        }
        compare(counts[counts.length - 1], totals[0], "and reaches the end");
    }

    Component {
        id: bareControllerComponent
        LibraryConsistencyController {}
    }

    // #61. Playlists across the libraries: a real scan of a fixture copy
    // finds playlists that differ, following one library stages a change
    // that a real save writes, and the rescan finds that playlist in
    // agreement. Following rekordbox writes OneLibrary; following
    // OneLibrary takes entries out of export.pdb.
    function test_aPlaylistFollowsTheLibraryPicked_data() {
        // Not Engine: the fixture's Engine library was anonymized apart
        // from the other two and shares no file path with them, so no
        // playlist there can be compared. align_playlist_change_test
        // covers Engine's writes on a library of its own.
        return [{tag: "rekordbox", reference: "rekordbox"}, {tag: "onelibrary", reference: "onelibrary"}];
    }
    function test_aPlaylistFollowsTheLibraryPicked(data) {
        const stick = stickFixture.stickCopy(testCase.fixtureRoot);
        verify(stick.length > 0, "the fixture must copy");
        const page = createTemporaryObject(stickPageComponent, testCase,
                                           {rekordboxPath: stick + "/PIONEER", enginePath: stick + "/Engine Library"});
        const controller = page.consistencyController;
        tryVerify(() => !controller.busy, 300000, "the check finishes");
        compare(controller.playlistsError, "");
        verify(controller.playlistsChecked, "the playlists were compared");
        verify(controller.playlistDifferenceCount > 0, "precondition: the fixture's playlists differ");
        const card = findByObjectName(page, "playlistSyncCard");
        verify(card !== null && card.visible && !card.ok, "the hub says so");

        // A playlist whose chosen library has something to hand the others.
        // The name, copied: the list a property hands QML reads through to
        // the controller's current one, so an element held across the save
        // would read whichever playlist sits at its index afterwards.
        let name = "";
        for (const d of controller.playlistDifferences) {
            const r = d.references.find((x) => x.format === data.reference && x.adds + x.removes + x.swaps > 0);
            if (r && !d.missingSomewhere) {
                name = String(d.name);
                break;
            }
        }
        if (name.length === 0) {
            skip("the fixture has no playlist " + data.reference + " can hand the others");
        }
        controller.alignPlaylist(name, data.reference);
        const staged = controller.playlistDifferences.find((d) => d.name === name);
        compare(staged.staged, data.reference, "staged, and the page says which library");

        const libraryId = EditSessionRegistry.libraryIdForPath(stick + "/PIONEER");
        const session = EditSessionRegistry.sessionFor(libraryId, "TESTSTICK");
        verify(session !== null && session.pendingCount === 1);
        let summary = null;
        session.saveFinished.connect((result) => { summary = result; });
        session.save();
        tryVerify(() => summary !== null, 120000, "the save finishes");
        compare(summary.error, "", "the save wrote it: " + JSON.stringify(summary));
        tryVerify(() => !controller.busy && controller.playlistsChecked, 300000, "the rescan finishes");
        const after = controller.playlistDifferences.find((d) => d.name === name);
        if (after !== undefined) {
            // What is left is only what OneLibrary keeps out to stay level
            // with rekordbox.
            const again = after.references.find((x) => x.format === data.reference);
            verify(again === undefined || again.adds + again.removes + again.swaps === 0,
                   "following " + data.reference + " again has nothing left to do: " + JSON.stringify(after));
        }
    }

    // The stray-cue headline counts tracks, each audio file once, and
    // says what the rules are: a cue a library mirrors is a row in each,
    // and "214 cues sit at 0:00" read as 214 separate problems while
    // the rows also held cues up to two seconds in (Library Health
    // audit, 2026-10-04).
    // #57: rows the cue table check could not compare are not a clean
    // result. Content_id 392 given a cue kind Seabass does not know is
    // left alone, so nothing is listed, and the card still says so and
    // opens the page.
    function test_cueTablesNotComparedKeepTheCardOpen() {
        const stick = stickFixture.stickCopy(testCase.fixtureRoot);
        verify(stick.length > 0, "the fixture must copy");
        verify(stickFixture.plantCueTableRow(stick, 392, 12, 1000).contentId === "392");
        const page = createTemporaryObject(stickPageComponent, testCase,
                                           {rekordboxPath: stick + "/PIONEER", enginePath: stick + "/Engine Library"});
        const controller = page.consistencyController;
        tryVerify(() => !controller.busy && controller.cueTablesChecked, 300000, "the check finishes");
        compare(controller.cueTableRowCount, 0);
        compare(controller.cueTableLeftAloneCount, 1);
        const card = findByObjectName(page, "cueTableCard");
        verify(card !== null && card.visible);
        compare(card.ok, false, "tables not compared are not healthy");
        verify(card.summary.indexOf("not compared") >= 0 && card.summary.indexOf("Every OneLibrary") < 0, card.summary);
        compare(card.actionLabel, "Review cue tables");
    }

    function test_theStrayCueHeadlineCountsTracks() {
        const stick = stickFixture.stickCopy(testCase.fixtureRoot);
        verify(stick.length > 0, "the fixture must copy");
        const page = createTemporaryObject(stickPageComponent, testCase,
                                           {rekordboxPath: stick + "/PIONEER", enginePath: stick + "/Engine Library"});
        const controller = page.consistencyController;
        tryVerify(() => !controller.busy, 300000, "the check finishes");
        const rows = controller.junkCues.count;
        verify(rows > 0, "precondition: the fixture has stray cues");
        const tracks = controller.junkCues.trackCount();
        verify(tracks > 0 && tracks <= rows, "tracks " + tracks + " of " + rows + " rows");
        const card = findByObjectName(page, "junkCuesCard");
        verify(card.summary.indexOf(tracks + (tracks === 1 ? " track has" : " tracks have")) === 0, card.summary);
        verify(card.summary.indexOf(rows + (rows === 1 ? " cue" : " cues")) > 0, card.summary);
        verify(card.summary.indexOf("at 0:00") < 0, "not \"at 0:00\": the rules reach two seconds in");
    }

    // The legs, one per catalog, share the one bar (#58): a leg begins
    // with the chain's total and the count the legs before it reached,
    // and a leg answered from the catalog cache (the second check of the
    // same stick) does not reset either. Only the step is its own: the
    // last leg's step is not shown over the next one's first read.
    function test_eachCatalogContinuesTheOneBar() {
        const stick = stickFixture.stickCopy(testCase.fixtureRoot);
        verify(stick.length > 0, "the fixture must copy");
        const controller = createTemporaryObject(bareControllerComponent, testCase);
        controller.scan(stick + "/PIONEER", stick + "/Engine Library", "");
        tryVerify(() => !controller.busy, 300000, "the first check finishes");

        {
            const run = "warm";
            const legs = [];
            const record = () => legs.push({format: controller.scanningFormat, phase: controller.scanPhase,
                                            current: controller.scanCurrent, total: controller.scanTotal});
            controller.scanningFormatChanged.connect(record);
            controller.scan(stick + "/PIONEER", stick + "/Engine Library", "");
            tryVerify(() => !controller.busy, 300000, "the " + run + " check finishes");
            controller.scanningFormatChanged.disconnect(record);

            const started = legs.filter((leg) => leg.format.length > 0);
            compare(started.map((leg) => leg.format), ["rekordbox", "engine", "onelibrary"],
                    run + ": precondition: every leg ran");
            // The first leg counts the chain on its own thread, so at its
            // start the total is not known yet; every leg after it begins
            // with the chain's total and the count so far.
            for (let i = 0; i < started.length; ++i) {
                const leg = started[i];
                compare(leg.phase, "", run + ": " + leg.format + " starts with no step of its own yet: " + JSON.stringify(leg));
                if (i > 0) {
                    verify(leg.total > 0, run + ": " + leg.format + " begins with the chain's total: " + JSON.stringify(leg));
                    compare(leg.total, started[started.length - 1].total, run + ": " + leg.format + " keeps the chain's total");
                    verify(leg.current >= started[i - 1].current,
                           run + ": " + leg.format + " continues the count: " + JSON.stringify(started));
                }
            }
            compare(controller.scanTotal, started[started.length - 1].total, run + ": one total to the end");
            compare(controller.scanPhase, "", run + ": nothing is left standing once the check is done");
        }
    }
}
